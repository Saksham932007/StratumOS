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
validation, a one-gateway route and a software loopback path; ICMP echo in
both directions; UDP with the pseudo-header checksum and bound ports with a
receive queue; TCP with both opens, several connections, congestion control
and a measured retransmission timeout; DHCP, DNS and an HTTP client; and
sockets, as descriptors, usable from ring 3.

**Not here**, stated up front because a stack that quietly lacks these is a
stack that works in a lab:

| Missing | Why it is a decision, not an oversight |
| --- | --- |
| **Out-of-order reassembly** | Now the largest gap. A segment that arrives early is dropped and the expected sequence re-ACKed. Fast retransmit makes the *sender* side of a single loss cheap; the receiver still costs a round trip per segment after the hole, because there is nowhere to put a segment that is not next. A reassembly queue needs a bound on memory a remote host controls. |
| **TLS, and therefore `https`** | A certificate store, a chain validator, RSA or ECDSA, AES-GCM and a hash — every one a correctness problem where being approximately right is the same as being broken. A kernel shipping a half-checked certificate validator would be worse than one that says plainly it cannot do HTTPS, so `fetch` refuses the scheme with a reason rather than quietly trying port 443. |
| **IP fragment reassembly** | Dropped and counted. Needs a hole list, an expiry timer and the same remote-controlled bound; it is where several famous denial-of-service bugs lived. Nothing this stack sends or receives is fragmented. |
| **A routing table** | One gateway, and `net_is_local()` is the whole of the routing. A table is the right shape for a second interface, and there is not one. |
| **Window scaling, SACK, timestamps, PAWS** | All need the TCP options parser this does not have beyond MSS. Window scaling in particular caps the window at 65535, so this cannot fill a path whose bandwidth-delay product is larger. |
| **`select`/`poll` across descriptors** | The `poll_read` and `poll_write` ops exist and the blocking paths use them, so the mechanism is there. What is missing is a call that waits on a *set*, which needs a wait queue the scheduler does not have — which is also why a blocking socket call polls the network and sleeps instead of waiting. |
| **DHCP lease renewal** | The lease time is recorded and reported and nothing acts on it, so an address is used until reboot. Renewal is a timer at T1 and a rebind at T2; it is straightforward and is not what makes DHCP interesting. |
| **Nagle, and delayed ACK** | A one-byte write becomes a one-byte segment and every segment is acknowledged immediately. Chatty, and much easier to follow in a packet capture. |
| **ICMP errors** | A UDP datagram to a closed port is counted, not answered with a port-unreachable. Generating ICMP errors needs rate limiting, or a flood of datagrams becomes a flood of errors — the same shape of bug the syscall fuzzer found in phase 7. |
| **CNAME chasing, DNSSEC, EDNS** | The resolver takes an A record from wherever it appears in the answer section, which handles the common CNAME-then-A reply; a reply with a CNAME and no A record is reported as not found rather than followed. A truncated reply is reported rather than silently used, because there is no TCP fallback. |

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

Both opens, several connections at once, a real send queue, and congestion
control.

```
                    .--listen--> LISTEN --SYN--> SYN_RECEIVED --.
CLOSED -------------<                                            >--> ESTABLISHED
                    `--connect--> SYN_SENT --SYN,ACK------------'         |
                                                              .----------+----------.
                                                       (peer FIN)                (close)
                                                              v                      v
                                                        CLOSE_WAIT              FIN_WAIT_1
                                                              |                      |
                                                           (close)              (their FIN
                                                              v                  or our ACK)
                                                         LAST_ACK                     v
                                                              |                 FIN_WAIT_2
                                                              `----> TIME_WAIT <-----'
```

### Why one connection was not enough, and why three things landed together

The stack held one connection in one static struct for as long as all it did
was echo. Three of the things it was missing could not be added separately,
because each was blocked on the same thing:

- **A sockets API** needs a listener and an accepted connection to exist at
  the same time. With one connection, `accept()` would have to destroy the
  thing that produced it.
- **An outgoing connection** needs the same: the echo port is still listening
  while a client talks to a server somewhere else.
- **A congestion window** on a stack that holds one connection cannot be
  tested against anything, because there is no second flow to be fair to.

So the single struct became a small fixed table demultiplexed on the
four-tuple. A handle into it carries a generation counter, because a socket
that closed and whose slot was immediately reused would otherwise find its
stale handle silently addressing a different peer — the same hazard a file
descriptor has, solved in the stack because the stack is what reuses the slot.

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

Retransmission after a *timeout* rewinds `snd_nxt` to `snd_una` and re-sends
from there, up to five times, after which the connection is reset rather than
retried forever. A full rewind is right there, because after a timeout there
is no evidence that anything after the hole arrived — which is exactly what
distinguishes it from fast retransmit below.

### The two windows

The old send path was limited only by the peer's receive window, and that is
exactly wrong on a congested path in a way that is invisible on a loopback.
The receive window says what the *peer* can absorb and says nothing at all
about what the *path between you* can carry. A sender limited only by it
responds to loss — the signal that the path is full — by retransmitting at
the same rate, which is what turns congestion into collapse. This is the
behaviour that made the internet unusable in October 1986 and the reason Van
Jacobson's paper exists.

So there are two windows and the smaller governs:

| | What it means | Who sets it |
| --- | --- | --- |
| `snd_window` | what the peer can receive | the peer, in every segment |
| `cwnd` | what this sender believes the path can carry | inferred from loss; nobody advertises it |

`cwnd` moves by RFC 5681. In **slow start** it grows by one MSS per
acknowledgement, which doubles it every round trip — "slow" is historical and
misleading, since it is exponential; the slow part is that it starts from two
segments rather than from the receive window. Past `ssthresh` it enters
**congestion avoidance** and grows by about one MSS per round trip, by
accumulating acknowledged bytes and spending a window's worth per MSS, which
is the integer form of `cwnd += MSS*MSS/cwnd` without losing the remainder.

Loss is read two ways, and the difference between them is most of what makes
TCP usable on a link that drops the occasional packet:

- **A timeout** is the harsh signal, because nothing got through. `ssthresh`
  halves to the flight size, `cwnd` drops to one segment, and the RTO doubles.
- **Three duplicate acknowledgements** is the weaker one, because the
  duplicates are themselves proof that later segments are arriving. So
  `ssthresh` halves, the one missing segment goes out immediately rather than
  waiting for the timer (**fast retransmit**), and the connection continues at
  the halved rate rather than restarting (**fast recovery**), with `cwnd`
  inflated by the three segments the duplicates prove have left the network.

Three is the threshold that has to be chosen and the one everyone chose: below
it a single reordered segment would trigger a spurious retransmit, above it
recovery waits longer than it needs to.

### Measuring the round trip

The retransmission timeout used to be the constant 500 ms, which is far too
long for a LAN and far too short for anything over a satellite. It is now
measured, by Jacobson's estimator:

```
srtt   = 7/8 srtt + 1/8 sample            a smoothed average
rttvar = 3/4 rttvar + 1/4 |srtt - sample| its mean deviation
rto    = srtt + 4 rttvar                  clamped to [200, 8000] ms
```

The variance term is the part that matters and the part pre-1988
implementations lacked: a timeout set from the average alone fires spuriously
whenever the path's delay is merely *variable*, and a spurious retransmission
on a congested path is the worst possible response.

**Karn's algorithm** is why a sample is thrown away when its segment was
retransmitted. If a segment is sent twice and then acknowledged, there is no
way to know which copy the acknowledgement is for, so the sample is ambiguous.
Without this, one timeout poisons the estimate with a sample roughly one RTO
too long, which raises the RTO, which makes the next sample worse.

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

## Loopback, and why it is a queue

A datagram addressed to this machine's own address, or to anything in
`127.0.0.0/8`, never reaches a wire. That is not an optimisation: a network
controller does not receive its own transmissions, and QEMU's user-mode
networking does not reflect a frame sent to the guest's own address either.
Without it, `ping 10.0.2.15` times out *on the machine it is run on*, and a
program that connects to a server it is itself running cannot work at all —
which is exactly what the ring-3 socket program does.

It is decided in `ipv4_output_frame()` **before** ARP, because there is
nothing to resolve: the datagram is not going to a link. Deciding it after
would ARP for this machine's own address, get no answer from anybody, and drop
every datagram a program sent to itself.

And it is a queue rather than a direct call. Handing the datagram straight to
`ipv4_input()` would work, and would nest: the receive runs TCP, which sends
an acknowledgement, which loops back, which runs TCP again — all on one kernel
stack, with the depth decided by how much data is in flight. The queue bounds
it at one, and `loopback_drain()` is bounded by what was queued when it
*started* rather than by the queue being empty, so a connection to oneself
cannot spin inside a single call. That is also how a real loopback device
behaves: it is an interface with a transmit ring, and nothing about it is
synchronous.

---

## The three protocols above the stack

The stack could put a correct TCP segment on a wire and still reach nothing,
for three reasons that are not networking problems: it had no address except
the one compiled into it, no way to turn a name into an address, and nothing
to say once a connection was open. Each of the three turned out to be the
first user of something underneath it that had never run.

### DHCP

Four messages — DISCOVER, OFFER, REQUEST, ACK — and the extra round trip
exists for one reason: more than one server may answer. A server that makes an
offer sets that address aside, and a client that simply used whichever offer
arrived first would leave every other server holding a reservation for a
client that was never coming back. So the client names the offer it took, and
the servers whose offers were *not* taken learn so by hearing it. That is also
why the REQUEST is broadcast rather than unicast to the chosen server, which
looks wasteful until you notice who needs to hear it.

It is the first thing to send from an address it does not have yet, which
needed a broadcast output path of its own: there is no ARP for
`255.255.255.255`, and the ordinary path refuses to send until the stack is
configured. Both are assumptions the normal path is entitled to make, which is
why it is a second function rather than a flag on the first.

**It found a real bug.** UDP verified every checksum against *this machine's*
address rather than the datagram's own destination. A broadcast reply is
addressed to `255.255.255.255`, so its pseudo-header does not match — and
every DHCP reply failed its checksum, silently, presenting as a timeout.
`udp_input()` now takes the destination as an argument, which is the one thing
it needs that ICMP and TCP do not.

The decision that matters most: **the existing configuration is not touched
until an ACK arrives.** This kernel's whole test suite runs against the static
address QEMU hands out, and a client that cleared the address when it started
asking would break every scenario for as long as the server took to answer —
or permanently, on a network with no server.

### DNS

A stub resolver: one question, one server, over UDP, and the recursion happens
in the server DHCP told us about. Every operating system ships the stub rather
than the recursive resolver, and for the same reason.

It is the first request/reply protocol here, so UDP grew bound ports with a
short receive queue — which DHCP, DNS and a datagram socket all needed at
once. The queue is two deep on purpose: a datagram protocol has no flow
control, so the only question is how many to keep before dropping, and a deep
queue on a kernel with no way to apply back pressure just delays the drop
while using more memory. A full queue drops its *oldest* entry, because for a
request/reply protocol the newest datagram is the one most likely to be the
reply being waited for.

**The parsing is the whole problem.** A name in a reply may end in a
compression *pointer* to an earlier offset in the same message — which is why
a reply is small, and why nearly every serious bug in a DNS client has been in
the name parser. A pointer may point anywhere the sender likes, including at
itself or into a chain that loops, so a parser that follows pointers without
a bound can be sent round that loop forever by one datagram: a remote denial
of service in forty lines of straight-line code.

Three defences, and each covers a mistake the others do not:

1. every offset is bounded by the message length before it is dereferenced;
2. a pointer must point strictly *backwards* — a pointer to itself or forwards
   is how a loop is built, and there is no legitimate reason for one, since the
   purpose of compression is to refer to a name already written;
3. the number of jumps is capped regardless, which catches the chain of
   individually-legal backward pointers that the first two do not.

`dns_skip_name()` is the only part of the parser that is exposed, and it is
exposed so that all three can be tested directly rather than through whatever
replies a live server happens to send. The `inet` suite feeds it a
self-referential pointer, a forward pointer, a 31-deep backward chain, a label
that runs off the end, a truncated pointer, and both reserved label types.

### HTTP

One GET, and the first user of an outgoing connection — which is where it
found the next bug. `tcp_connect()` reported failure when the first SYN could
not be transmitted, which is *always*, for a host whose hardware address is
not yet known: the IPv4 layer queues an ARP request and drops the frame.
`fetch` failed on every cold ARP cache and worked on the second try, which is
the shape of a bug that looks like a flaky network. A stack with a
retransmission timer does not need its first transmission to succeed, and
designing as if it does is how the timer comes to have an untested path. The
DNS client had the same shape and the same fix.

It is also the first thing here to care that **a response has no length in
it, in general.** HTTP/1.1 ends a response at `Content-Length`, or at the end
of a chunked body, or — the case everything else forgets — when the server
closes the connection. The third is why `tcp_recv_eof()` has to be
distinguishable from "nothing to read right now", and why a connection in
`CLOSE_WAIT` must still be drained: a client that treats the FIN as the end of
the data loses the tail of every response. The `internet` scenario serves one
document per framing mode for exactly this reason.

There is no TLS, so there is no `https`, and the refusal says so rather than
quietly trying port 443 and producing a binary handshake and an unreadable
error.

---

## Sockets

This is the layer the descriptor table was built for, and it is a short file
because of that. `read` is a receive, `write` is a send, `poll_read` is what
makes a blocking receive work, and the three calls with no file equivalent —
`bind`, `listen`, `accept` — are the only ones that needed anything new.
`accept` is the interesting one: it produces a *descriptor*, which is why
`fd_install()` exists.

That is the whole argument for having built the descriptor table first. A
socket that was not a descriptor would have needed its own table, its own
close-on-exit path, its own fork semantics and its own `read`; being a
descriptor, it inherits all four.

Blocking is polling the network and sleeping, not waiting on a queue — the
same compromise `ping` and `arp` make in the shell, and for the same reason:
there is no wait queue, and inventing one for this would be inventing it in
the wrong place. The `sockets` command counts how often a call had to wait, so
the cost is visible rather than implied.

`user/netcat.c` is the proof, and it runs in **ring 3**, which is the point:
every address it hands the kernel is a user-space address that has to be
validated, every socket is a descriptor in its own table, and the connection
has to survive a process that can be preempted between any two calls. It does
four things in increasing order of what they prove — a socket that `fstat`
calls a socket and `lseek` refuses; a UDP round trip; an outgoing TCP
connection that echoes; and a listener it connects to *from itself*, with the
listener, the client and the accepted connection all alive at the same moment.
That last one is exactly what a single-connection stack could never have done.

---

## Testing it

| Where | What it asserts | Checks |
| --- | --- | --- |
| `net` suite, no controller | every checksum, parser, address helper, the ARP cache, the connection table's handles and its refusals | 95 |
| `net` suite, with a controller | the above, plus the rings, the MAC, a live ARP resolve and a ping that comes back | 130 |
| `inet` suite | URL parsing, the DNS name parser against six malformed messages, the resolver's shortcuts, UDP's bound ports | 60 |
| `sock` suite | sockets as descriptors, every refusal, the table's ceiling, and a loopback round trip with no network involved | 116 |
| `network` scenario | **the frames on the wire**, parsed from a pcap | — |
| `internet` scenario | **that a real peer understood them**: QEMU's DHCP server, and an HTTP server on the host that counts what it served | — |
| `no-network` scenario | the kernel reports no controller and carries on | — |
| `interactive-shell` | `net`, `arp`, `ping` (including on its own address), `udpsend`, `netcat` and `sockets` report correctly | 10 commands |

### The scenario that needs a real peer

`network` and `internet` are counterparts and opposite kinds of test.
`network` asserts on the frames the kernel *sent*, parsed by the harness with
no peer involved at all. `internet` asserts that a real peer *understood*
them — because a stack can put perfectly-formed frames on a wire and still
fail to talk to anything, and both halves are needed.

The peer is on the host rather than emulated, which matters for one reason:
the host's HTTP server counts the requests it served, so the assertion that
the kernel fetched four documents is made by the server and not by the
kernel's own report of its own behaviour. Nothing in it needs the host to have
internet access — QEMU's user-mode networking provides the DHCP server, and
`10.0.2.2` *is* the host.

It is also what found the driver's receive-ring race. `e1000_poll()` is called
from both the interrupt handler and the poll loop, derives its next descriptor
from `RDT` and publishes by writing `RDT` forward; an interrupt landing inside
the loop made the interrupted copy write a stale tail *backwards*, and a few of
those wedged the ring. Receive stopped while transmit carried on working, which
is what makes that kind of fault hard to see from outside. It presented as a
SYN retransmitted five times and never answered, on the third HTTP fetch of a
run and never the first two — so a single-fetch test would not have found it.
The guard is a flag rather than a lock, because the second entry is on the same
processor and a lock would deadlock against itself, and the counter it keeps
reads 30 on a run that does four fetches.

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
