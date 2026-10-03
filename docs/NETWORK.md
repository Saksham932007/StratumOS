# Networking

An Intel e1000, Ethernet, ARP, IPv4, ICMP, UDP and a small TCP. Enough that
the machine answers a ping, answers a who-has, accepts a TCP connection and
echoes on it.

```bash
make run-net                     # boot with a network card attached
stratum> net                     # the controller, addressing, per-layer counters
stratum> arp 10.0.2.2            # resolve, and show the cache
stratum> ping 10.0.2.2 4
stratum> udpsend 10.0.2.2 7 hello
python3 tools/run-tests.py --only network
```

---

## What is here, and what is not

**Here:** the driver, with descriptor rings and interrupts; Ethernet framing
and filtering; ARP with a cache that ages out; IPv4 with checksums, header
validation and a one-gateway route; ICMP echo in both directions; UDP with
the pseudo-header checksum; TCP with a three-way handshake, a send queue,
retransmission, graceful close and a reset for a port nothing listens on.

**Not here**, stated up front because a stack that quietly lacks these is a
stack that works in a lab:

| Missing | Why it is a decision, not an oversight |
| --- | --- |
| **Congestion control** | No slow start, no congestion window, no fast retransmit. The single biggest omission, and the reason this is not a general-purpose TCP: on a congested path it is antisocial. It needs an RTT estimator first, which needs timestamps. |
| **Out-of-order reassembly** | A segment that arrives early is dropped and the expected sequence re-ACKed. Correct, and slow — one loss becomes a round trip per segment after it. A reassembly queue needs a bound on memory a remote host controls. |
| **IP fragment reassembly** | Dropped and counted. Needs a hole list, an expiry timer and the same remote-controlled bound; it is where several famous denial-of-service bugs lived. Nothing this stack sends or receives is fragmented. |
| **DHCP** | The addressing is static. DHCP is a UDP application, so it belongs *above* this layer — it would be the first thing built on it, not part of it. |
| **A sockets API** | Ring 3 cannot open a connection. That needs file descriptors, which this kernel does not have: there is no `open`, so there is nothing for a socket to be. It is the next real piece of work, and it is a process-model change rather than a network one. |
| **A routing table** | One gateway, and `net_is_local()` is the whole of the routing. A table is the right shape for a second interface, and there is not one. |
| **Window scaling, SACK, timestamps, PAWS** | All need the TCP options parser this does not have beyond MSS. |
| **ICMP errors** | A UDP datagram to a closed port is counted, not answered with a port-unreachable. Generating ICMP errors needs rate limiting, or a flood of datagrams becomes a flood of errors — the same shape of bug the syscall fuzzer found in phase 7. |

---

## The driver

### The ring, and who owns what

Receive and transmit are each a circular array of 16-byte descriptors in
memory the device reads and writes by DMA. Each descriptor holds a
**physical** address, a length and a status byte. There are two indices per
ring, and the whole protocol is which side moves which:

```
head (RDH/TDH)   the device moves.  Where it will work next.
tail (RDT/TDT)   the driver moves.  Where the device must stop.
```

The device consumes descriptors from head towards tail and will not pass
tail. So:

- **receive**: the driver hands the device empty buffers by *advancing tail*.
  A frame arrives with the DD (descriptor done) bit set.
- **transmit**: the driver writes a descriptor at tail, then advances tail,
  which is what tells the device there is work.

One slot is always left unused. With `count` descriptors the ring holds
`count - 1`, because `tail == head` has to mean *empty* rather than *full* —
there is no third state to tell them apart. Getting that wrong gives a ring
the device thinks is empty the moment it is full, which presents as
transmits that silently never happen.

### Why there is no cache flushing, and why there are barriers

x86 DMA is cache-coherent: the device's reads snoop the processor's caches.
So descriptors live in ordinary cached memory and no flush is needed.

What *is* needed is `volatile` and compiler barriers, for exactly the reason
the page-table code needs them (decision 28). The compiler cannot see that
another agent reads these words, so it is entitled to keep a status byte in
a register forever, and entitled to move the store that advances tail above
the stores that filled the descriptor that store publishes:

```c
d->cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;

/* Advancing tail is what tells the device to read the descriptor, so every
 * store above has to be visible first. x86's store ordering makes the
 * hardware side of this free; the compiler side is not. */
barrier();

nic.tx_next = (slot + 1) % TX_DESCS;
reg_write(E1000_TDT, nic.tx_next);
```

On an architecture without coherent DMA this file would need explicit cache
maintenance, and that is one of the things a port would have to deal with.

### One buffer, not three

The first version gave each layer its own full-size buffer and copied the
payload into the next one: TCP built a segment in 1484 bytes of stack, IP
copied it into 1520, Ethernet copied that into 1514. Three full frames on the
stack at once — about 5 KiB of a 16 KiB kernel stack — and every byte copied
three times.

That is thin rather than broken, and it is thin in the worst place. The
receive path runs in an interrupt handler, on whatever task happened to be
running, so the 5 KiB lands on top of however deep that task already was. A
task 11 KiB into a syscall taking a network interrupt would hit its guard
page. The guard page makes that a panic rather than corruption — which is why
it was a fragility and not a vulnerability — but a panic triggered by a
remote peer's packet is still a remote peer deciding when the machine stops.

So there is one buffer, filled from the inside out, with headroom at the
front for the headers each layer prepends:

```c
#define NET_HEADROOM (ETH_HDR_LEN + IPV4_HDR_LEN) /* 34 */

struct net_frame {
    u8 buf[ETH_HDR_LEN + ETH_MTU];
    u32 head; /* index of the first valid byte */
    u32 tail; /* one past the last valid byte  */
};
```

The transport appends its header and payload at `NET_HEADROOM`; IPv4
prepends 20 bytes in front of that; Ethernet prepends 14 more. Zero copies
between layers, and 1514 bytes of stack instead of 4518.

The copying forms (`ipv4_output`, `eth_output`) still exist for callers that
already have a contiguous payload somewhere else — ARP, whose packet is 28
bytes, and ICMP's echo reply, which is a copy of the request. They are
implemented *through* the frame builder, so each header is assembled in
exactly one place.

### Three bits that are not obvious

- **`RCTL.SECRC`** strips the 4-byte Ethernet CRC, so a received length is
  the frame length the layers above expect. Without it every length is four
  too big and every payload has four bytes of garbage on the end — which
  presents as checksums failing for no visible reason.
- **`RCTL.BAM`** accepts broadcast, which ARP needs: a who-has for this
  machine arrives addressed to `ff:ff:ff:ff:ff:ff`, so a receiver that only
  accepts its own unicast address can never be found.
- **PCI bus mastering** has to be enabled in the command register. The device
  cannot touch memory without it, so a driver that forgets it gets a ring the
  device never reads and transmits that never happen — with every register
  reading back correctly.

### Why e1000 and not virtio-net

virtio is simpler, and that is the argument against it here. It is a
paravirtual interface; the point of writing a driver is the part where the
hardware does not cooperate — rings the device owns half of, a reset sequence
with required delays, a MAC address behind a polled EEPROM register. The
e1000 is also a card people actually have.

---

## The bug the packet capture found

The first working version of the driver transmitted every frame with a source
MAC of `00:00:00:00:00:00`.

```
ffffffffffff 000000000000 0806 0001 0800 06 04 0001 000000000000 0a00020f ...
^dst=bcast   ^src=ZERO                        ^sender_mac=ZERO   ^our IP
```

`struct net_device` was `const`, and its `.mac` was never filled in — the
driver kept its own copy in `nic.mac`, which is what the boot log printed.
So the log showed the right address while `eth_output()` built frames from a
second copy that was all zeros.

The peer replied, politely, **to the zero address**. The controller's own
receive filter then dropped the reply, because it was not addressed to the
card. The result: transmit worked, 204 ARP requests went out, the device's
own counter confirmed them, receive was completely silent, and every log
line was correct.

No assertion in the kernel could have caught it, because every value the
kernel could check was consistent with itself. What found it was asking
QEMU to dump the wire to a pcap and reading the bytes.

That is why the `network` CI scenario parses a capture rather than the log,
and why the harness recomputes the checksums itself — see below.

---

## The flake the network thread exposed

Worth recording because it is the useful kind.

The `profile` suite feeds the sampling profiler synthetic interrupt frames
and asserts *exact* sample counts. The profiler is driven by the real timer
interrupt, which fires every 10 ms, and the test's window between resetting
the counters and reading them is a few microseconds — so a real tick landing
in that window adds a sample the test did not ask for.

It had always been possible and had never happened. Adding a network thread
that wakes 100 times a second, and a suite before it that sleeps in a polling
loop, made the machine preempt far more often and turned the race into a
*reliable* failure: three runs out of three, with a network card attached,
and a clean pass without one.

```c
/* A test that asserts an exact count against an asynchronous producer has to
 * exclude the producer. */
bool irqs = irq_save();
```

A flake that becomes deterministic before it reaches someone else is the best
outcome available. The fix is one `irq_save`/`irq_restore` pair, and the real
lesson is in the comment: the test was passing on luck rather than on
exclusion, and nothing in it said so.

## The checksum, and the two ways to get it wrong

RFC 1071's one's-complement sum, used by IPv4, ICMP, UDP and TCP with
different coverage each time. Two bugs account for almost every broken
hand-written implementation:

**Byte order.** The sum is computed over the bytes *as they lie in memory*,
treating each pair as a big-endian 16-bit word, and the result is stored
without conversion. That is correct on either endianness in exactly this
arrangement — a fact that is true, useful, and completely non-obvious, so it
is written down in `net.c` rather than rediscovered.

**The odd trailing byte** is the *high* half of a word whose low half is
zero, not the low half of one. Getting it backwards gives a checksum that is
right for every even-length packet, which is most of them.

Both are tested, and tested against packets this kernel did not produce:

```c
/* A real IPv4 header from a UDP datagram QEMU's gateway sent us. A correct
 * header, including its checksum field, sums to zero. */
static const u8 real_udp_ip_hdr[] = {
    0x45, 0x00, 0x00, 0x35, 0x00, 0x00, 0x00, 0x00, 0x40, 0x11,
    0x62, 0xa8, 0x0a, 0x00, 0x02, 0x02, 0x0a, 0x00, 0x02, 0x0f,
};
KT_EQ(r, checksum(real_udp_ip_hdr, sizeof(real_udp_ip_hdr)), 0u);
```

A checksum tested only against its own output is a checksum tested against
itself, and both of those bugs survive that. These headers came out of a
capture of this kernel talking to somebody else's stack. The suite also flips
every single byte in turn and requires the checksum to break, because one
that ignores the last word passes a test that only flips the first.

### The pseudo-header

TCP and UDP checksum twelve bytes that are not in their own header: the
source and destination addresses, the protocol number and the length. It
exists so that a datagram delivered to the wrong host or the wrong protocol
fails rather than being accepted. The suite proves it does its job rather
than trusting that it does — the same real segment, checksummed with the
wrong destination and then the wrong protocol, must fail both times.

---

## ARP, and why the first packet is always lost

`arp_resolve()` on a cache miss sends a request and returns false. The
caller's packet is **dropped**.

A queue per unresolved address needs a timer to expire it, a bound to stop a
hostile peer filling it, and a retry policy. IP is allowed to drop packets
and every protocol above it already copes, so the first datagram to a new
address is lost and the second — after the reply arrives — goes out.

This is written down because "the first ping is always lost" looks like a bug
otherwise, and because the shell says so when it happens:

```
seq 1: not sent (address not resolved yet?)
```

The cache learns from requests as well as replies: the sender's mapping is in
both, and a machine that only learns from replies has to ARP for every host
that ARPs for it. It learns from the ARP packet's `sender_mac` field rather
than the Ethernet source, which is the standard's wording — they can differ,
and a proxy ARP responder is the legitimate case. Cross-checking them is what
a real stack does to spot spoofing; this one does not, and says so rather
than implying the check exists.

---

## TCP

A passive open, one connection at a time, with a real send queue.

```
CLOSED --listen--> LISTEN --SYN--> SYN_RECEIVED --ACK--> ESTABLISHED
                                        |                     |
                                   (timeout)              (peer FIN)
                                        v                     v
                                     LISTEN              CLOSE_WAIT
                                                              |
                                                          LAST_ACK --ACK--> TIME_WAIT
```

### Sequence arithmetic

Sequence numbers are 32 bits and they wrap. Comparing them with `<` or `>` is
wrong for exactly the reason it is subtle: it works for the whole first four
gigabytes of a connection and then silently inverts. The comparison has to be
done on the *signed difference*:

```c
static inline bool seq_lt(u32 a, u32 b) { return (i32)(a - b) < 0; }
```

Used everywhere rather than sometimes.

### The send queue

Data is buffered, sent in MSS-sized segments within the peer's window, and
released from the front of the buffer as it is acknowledged. A FIN goes out
only once every queued byte has been sent, because a FIN consumes a sequence
number and sending it early would claim the stream ended before the data in
it.

Retransmission rewinds `snd_nxt` to `snd_una` and re-sends from there, up to
five times, after which the connection is reset rather than retried forever.
The timeout is a constant — 500 ms — where a real stack measures the path and
adapts. That is slow on a LAN and fast on a satellite link, and it is the
cost of not measuring.

### Two details worth getting right

**A half-open connection is not kept.** A peer that sent a SYN and never
completed the handshake loses its slot after ten seconds; otherwise one
spoofed packet denies service to everyone else, since there is one connection
slot. This is the small version of what SYN cookies solve.

**A reset is built from the offending segment**, not from a connection —
there isn't one. RFC 793: if the offending segment had ACK set, the reset's
sequence is that ACK and it carries no ACK of its own; otherwise the reset
acknowledges the offending segment and its own sequence is zero. Getting it
wrong produces a reset the peer ignores, which presents as a connection that
hangs rather than one that fails.

### What it was tested against

A real host-side peer, through QEMU's `hostfwd`:

```
TCP echo result: (b'hello from the host\n', b'second line\n')
```

Two separate writes, echoed in order, connection closed gracefully to
TIME_WAIT, zero retransmits, zero out-of-order. The host's own TCP did the
other half of the handshake, which is the point: nothing in the
implementation can be quietly non-conformant and still produce that.

---

## Testing it

| Where | What it asserts | Checks |
| --- | --- | --- |
| `net` suite, no controller | every checksum, parser, address helper and the ARP cache | 63 |
| `net` suite, with a controller | the above, plus the rings, the MAC, a live ARP resolve and a ping that comes back | 98 |
| `network` scenario | **the frames on the wire**, parsed from a pcap | — |
| `no-network` scenario | the kernel reports no controller and carries on | — |
| `interactive-shell` | `net`, `arp`, `ping` and `udpsend` report correctly | 6 commands |

### The scenario that reads the wire

QEMU attaches a default e1000 unless told otherwise, so every other scenario
has a network whether it asked for one or not — which is why `no-network`
exists, with `-nic none`, to exercise the path where there is no controller
at all. Both suite paths are asserted by *check count*, because a run that
quietly took the narrower branch would otherwise pass while testing less.

The `network` scenario adds `filter-dump`, which writes every frame in both
directions to a pcap, and the harness parses it:

```python
# Every IPv4 header the kernel sent, checksummed here rather than trusted.
if body[12:16] == GUEST_IP:
    if inet_checksum(body[:ihl]) != 0:
        bad_ip_checksum += 1
```

`inet_checksum` in `run-tests.py` is written independently of the kernel's,
so the two agreeing means something. The scenario also requires an ARP
request from our address, a reply from the gateway, an echo request out, an
echo reply in, and — specifically, so it can never come back silently — that
no frame had a zero source hardware address.

That last assertion exists because of the bug above. A test that only reads
the kernel's own output could not have found it, and would not catch it
again.
