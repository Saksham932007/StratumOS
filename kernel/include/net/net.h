/* StratumOS - the network stack's shared vocabulary.
 *
 * Addresses, byte order, the one checksum every layer above Ethernet needs,
 * and the interface a driver has to implement to carry frames.
 *
 * WHAT IS HERE
 * ------------
 * Ethernet, ARP, IPv4, ICMP echo, UDP and a minimal TCP, over an Intel
 * e1000. Enough that the machine answers a ping, answers an ARP who-has,
 * accepts a TCP connection and echoes on it.
 *
 * WHAT IS NOT
 * -----------
 * Said here rather than discovered later: no IP fragment reassembly, no
 * routing table beyond one gateway, no DHCP, no congestion control, no
 * window scaling, no TCP options beyond MSS, no sockets API for ring 3.
 * docs/NETWORK.md gives the reasoning for each and docs/ROADMAP.md the
 * order.
 *
 * BYTE ORDER
 * ----------
 * Every multi-byte field on the wire is big-endian and this machine is
 * little-endian, so every one of them needs converting. The types below are
 * deliberately distinct - `be16` and `u16` are different types to a reader
 * even though the compiler cannot tell them apart - because a missing
 * htons() is the single most common bug in a hand-written network stack and
 * it produces a packet that is wrong rather than a build that fails.
 */
#ifndef _NET_NET_H
#define _NET_NET_H

#include <kernel/types.h>

/* A value already in network byte order. Not enforced by the compiler; the
 * point is that a reader can see which side of the conversion a variable is
 * on, which is the information that is otherwise nowhere. */
typedef u16 be16;
typedef u32 be32;

static inline be16 htons(u16 v)
{
    return (be16)((v << 8) | (v >> 8));
}

static inline u16 ntohs(be16 v)
{
    return (u16)((v << 8) | (v >> 8));
}

static inline be32 htonl(u32 v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

static inline u32 ntohl(be32 v)
{
    return htonl(v);
}

/* ---- addresses ---------------------------------------------------------- */

#define ETH_ALEN      6
#define ETH_HDR_LEN   14
/* Here with the other on-the-wire lengths rather than down in the IPv4
 * section, because the frame builder's headroom is computed from it. */
#define IPV4_HDR_LEN  20
#define ETH_MIN_FRAME 60 /* without the 4-byte CRC the hardware appends */
#define ETH_MTU       1500
#define ETH_MAX_FRAME (ETH_HDR_LEN + ETH_MTU)

struct mac_addr {
    u8 b[ETH_ALEN];
};

/* IPv4 addresses are kept in *network* order throughout, which is the
 * decision that removes most of the conversions: an address is only ever
 * compared, copied, or put on the wire, and none of those need host order.
 * Only printing and parsing convert, and both are in one file. */
typedef be32 ipv4_addr;

#define IPV4(a, b, c, d)                                                 \
    ((ipv4_addr)(((u32)(d) << 24) | ((u32)(c) << 16) | ((u32)(b) << 8) | \
                 (u32)(a)))

extern const struct mac_addr mac_broadcast;
extern const struct mac_addr mac_zero;

bool mac_equal(const struct mac_addr *a, const struct mac_addr *b);
bool mac_is_broadcast(const struct mac_addr *a);
bool mac_is_multicast(const struct mac_addr *a);

/* Both return their argument buffer, so they can be used inside a kprintf
 * argument list. Not reentrant-safe by accident: the caller supplies the
 * storage. */
#define MAC_STR_LEN  18 /* "00:11:22:33:44:55" */
#define IPV4_STR_LEN 16 /* "255.255.255.255"   */

const char *mac_str(const struct mac_addr *m, char *buf, size_t len);
const char *ipv4_str(ipv4_addr ip, char *buf, size_t len);
bool ipv4_parse(const char *s, ipv4_addr *out);

/* ---- the one's-complement checksum ------------------------------------- */

/* RFC 1071. Used by IPv4, ICMP, UDP and TCP, with different coverage each
 * time - which is why it is here rather than in any one of them.
 *
 * The incremental form exists because TCP and UDP checksum a pseudo-header
 * that is not contiguous with the payload, and building a temporary buffer to
 * make it contiguous would mean a copy per packet. */
u32 checksum_partial(const void *data, size_t len, u32 sum);
u16 checksum_finish(u32 sum);

static inline u16 checksum(const void *data, size_t len)
{
    return checksum_finish(checksum_partial(data, len, 0));
}

/* ---- the frame being built ---------------------------------------------
 *
 * One buffer per transmit, with room at the front for the headers each layer
 * prepends on the way down.
 *
 * The first version gave every layer its own full-size buffer and copied the
 * payload into the next one: TCP built a segment in 1484 bytes of stack, IP
 * copied it into 1520, Ethernet copied that into 1514. Three full frames on
 * the stack at once - about 5 KiB of a 16 KiB kernel stack - and every byte
 * copied three times.
 *
 * That is thin rather than broken, and it is thin in the worst place: the
 * receive path runs in an interrupt handler, on whatever task happened to be
 * running, so the 5 KiB lands on top of however deep that task already was.
 * A task 11 KiB into a syscall taking a network interrupt would hit its
 * guard page. The guard page makes that a panic rather than corruption,
 * which is why it was a fragility and not a vulnerability - but a panic
 * triggered by a remote peer's packet size is still a remote peer deciding
 * when the machine stops.
 *
 * So: one buffer, filled from the inside out. The innermost layer appends
 * its payload at NET_HEADROOM, and each layer below prepends its header into
 * the space reserved in front. Zero copies between layers.
 */
#define NET_HEADROOM    (ETH_HDR_LEN + IPV4_HDR_LEN) /* 34 */
#define NET_PAYLOAD_MAX (ETH_MTU - IPV4_HDR_LEN)     /* 1480 */

struct net_frame {
    u8 buf[ETH_HDR_LEN + ETH_MTU];
    u32 head; /* index of the first valid byte          */
    u32 tail; /* one past the last valid byte           */
};

/* Start with an empty payload and all the headroom reserved. */
static inline void net_frame_reset(struct net_frame *f)
{
    f->head = f->tail = NET_HEADROOM;
}

/* Room for `n` more payload bytes at the end, or NULL if it would not fit.
 * Returns where to write them. */
static inline u8 *net_frame_append(struct net_frame *f, u32 n)
{
    if (f->tail + n > sizeof(f->buf))
        return NULL;

    u8 *p = f->buf + f->tail;

    f->tail += n;
    return p;
}

/* Room for an `n`-byte header in front of what is already there. Returns
 * where to write it. NULL would mean a layer tried to prepend more than
 * NET_HEADROOM, which is a programming error rather than a runtime one. */
static inline u8 *net_frame_prepend(struct net_frame *f, u32 n)
{
    if (n > f->head)
        return NULL;

    f->head -= n;
    return f->buf + f->head;
}

static inline u8 *net_frame_data(struct net_frame *f)
{
    return f->buf + f->head;
}

static inline u32 net_frame_len(const struct net_frame *f)
{
    return f->tail - f->head;
}

/* ---- the driver interface ---------------------------------------------- */

struct net_device {
    const char *name;
    struct mac_addr mac;

    /* Hand one frame to the hardware. The frame is complete including its
     * Ethernet header and excluding the CRC, which the hardware appends.
     * Returns false if the transmit ring is full - which the caller has to
     * handle rather than retry forever, because a full ring with a dead link
     * never drains. */
    bool (*transmit)(const void *frame, size_t len);

    /* Link state, read from the hardware rather than cached: a cable comes
     * out without telling anybody. */
    bool (*link_up)(void);

    /* Pull up to `max` received frames and push each into the stack. Called
     * from the interrupt handler and from the poll loop, so it must be safe
     * to call when there is nothing there. Returns how many it handled. */
    u32 (*poll)(u32 max);
};

/* Register the one device the stack uses. A second call replaces the first;
 * there is no routing between interfaces and pretending otherwise with a
 * list would be the wrong kind of generality. */
void net_set_device(const struct net_device *dev);
const struct net_device *net_device(void);
bool net_up(void);

/* This machine's addressing. Static, because DHCP is a protocol this stack
 * does not speak - see docs/NETWORK.md for why that is a deliberate
 * ordering rather than an omission. */
struct net_config {
    ipv4_addr addr;
    ipv4_addr netmask;
    ipv4_addr gateway;
};

void net_configure(ipv4_addr addr, ipv4_addr netmask, ipv4_addr gateway);
const struct net_config *net_get_config(void);

/* ---- loopback ----------------------------------------------------------
 *
 * A datagram addressed to this machine's own address, or to anything in
 * 127.0.0.0/8, never reaches a wire. That is not an optimisation and it is
 * not optional: a network controller does not receive its own
 * transmissions, and QEMU's user-mode networking does not reflect a frame
 * sent to the guest's own address either. Without this, `ping 10.0.2.15`
 * times out on the machine it is run on, and a program that connects to a
 * server it is itself running cannot work at all.
 *
 * It is a queue rather than a direct call. Handing the datagram straight to
 * ipv4_input() would work, and would nest: the receive would run TCP, which
 * would send an acknowledgement, which would loop back, which would run TCP
 * again - all on one kernel stack, with the depth decided by how much data
 * was in flight. The queue bounds it at one, which is also how a real
 * loopback device behaves: it is a network interface with a transmit ring,
 * and nothing about it is synchronous.
 */
#define LOOPBACK_QUEUE_MAX 8

/* Deliver whatever is queued. Called from net_poll(), and directly by a
 * caller that is about to wait for a reply to something it just sent to
 * itself. */
u32 loopback_drain(void);
u32 loopback_queued(void);

struct loopback_stats {
    u32 sent, delivered, dropped_full, dropped_big;
};

const struct loopback_stats *loopback_get_stats(void);

/* Is this address one of ours - this machine's configured address, or
 * 127.0.0.0/8? */
bool net_is_loopback(ipv4_addr ip);

/* Is this address on our own link, or does it need the gateway? The whole of
 * this stack's routing. */
bool net_is_local(ipv4_addr ip);
ipv4_addr net_next_hop(ipv4_addr ip);

/* ---- counters ----------------------------------------------------------- */

/* Per-layer, because "the ping did not come back" is a question about which
 * layer stopped it, and a single packet count cannot answer it. */
struct net_stats {
    u32 rx_frames, tx_frames, rx_bytes, tx_bytes;
    u32 rx_dropped_short, rx_dropped_not_ours, rx_unknown_ethertype;
    u32 arp_rx, arp_tx, arp_replies_sent, arp_requests_sent;
    u32 ip_rx, ip_tx, ip_bad_checksum, ip_bad_version, ip_fragments_dropped;
    u32 ip_not_ours, ip_unknown_proto;
    u32 icmp_rx, icmp_echo_requests, icmp_echo_replies_sent;
    u32 icmp_echo_replies_received, icmp_bad_checksum;
    u32 udp_rx, udp_tx, udp_no_port, udp_bad_checksum;
    u32 tcp_rx, tcp_tx, tcp_bad_checksum, tcp_no_port, tcp_resets_sent;
    u32 tcp_connections_accepted, tcp_connections_initiated, tcp_bytes_echoed;
};

struct net_stats *net_stats(void);
void net_stats_reset(void);

/* ---- the layers -------------------------------------------------------- */

void net_init(void);

/* Start the polling thread. Separate from net_init() because it needs the
 * scheduler, which comes up long after the PCI scan that finds the
 * controller. */
void net_start(void);

/* Ethernet. `eth_input` takes a received frame; `eth_output` prepends a
 * header and transmits. */
void eth_input(const u8 *frame, size_t len);

/* Prepend an Ethernet header to a frame under construction and transmit it.
 * The frame's payload is whatever the layers above have built. */
bool eth_output_frame(struct net_frame *f, const struct mac_addr *dst,
                      u16 ethertype);

/* The copying form, for a small payload that is not worth a frame builder -
 * ARP, whose packet is 28 bytes. */
bool eth_output(const struct mac_addr *dst, u16 ethertype, const void *payload,
                size_t len);

#define ETHERTYPE_IPV4    0x0800
#define ETHERTYPE_ARP     0x0806

/* ---- ARP ---------------------------------------------------------------- */

#define ARP_CACHE_ENTRIES 16

void arp_input(const struct mac_addr *from, const u8 *packet, size_t len);

/* Ask who has `ip`. Broadcast, so it needs no cache entry to send. */
bool arp_request(ipv4_addr ip);

/* Look `ip` up in the cache. On a miss, sends a request and returns false -
 * so the caller's packet is dropped rather than queued.
 *
 * Dropping is the honest choice at this size: a queue per unresolved address
 * needs a timer to expire it, a bound to stop a hostile peer filling it, and
 * a retry policy. IP is allowed to drop packets and every protocol above it
 * already copes, so the first packet to a new address is lost and the second
 * - after the reply arrives - goes out. docs/NETWORK.md says so out loud
 * because "the first ping is always lost" looks like a bug otherwise. */
bool arp_resolve(ipv4_addr ip, struct mac_addr *out);

struct arp_entry {
    ipv4_addr ip;
    struct mac_addr mac;
    bool valid;
    u64 learned_ms;
};

u32 arp_cache_count(void);
const struct arp_entry *arp_cache_at(u32 index);
void arp_cache_clear(void);

/* ---- IPv4 --------------------------------------------------------------- */

#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

void ipv4_input(const struct mac_addr *from, const u8 *packet, size_t len);

/* Prepend an IPv4 header to a frame under construction and send it.
 * Fragmentation is not implemented, so a payload that would not fit is
 * refused rather than silently truncated. */
bool ipv4_output_frame(struct net_frame *f, ipv4_addr dst, u8 proto);

/* Send a frame to 255.255.255.255 and the Ethernet broadcast address, with
 * an explicit source address.
 *
 * Separate from ipv4_output_frame() for two reasons, both of which are about
 * DHCP being the only user. There is no ARP for a broadcast address - the
 * destination hardware address is known without asking - so the ordinary
 * path would try to resolve 255.255.255.255 and drop the frame forever. And
 * the source has to be allowed to be 0.0.0.0, because a client asking for an
 * address does not have one yet, where the ordinary path refuses to send at
 * all until the stack is configured.
 *
 * Those are exactly the two assumptions the normal output path is entitled
 * to make, which is why this is a second function rather than a flag on the
 * first one. */
bool ipv4_output_broadcast_frame(struct net_frame *f, ipv4_addr src, u8 proto);

/* The copying form, for a caller that already has a contiguous payload. */
bool ipv4_output(ipv4_addr dst, u8 proto, const void *payload, size_t len);

/* The pseudo-header sum TCP and UDP both checksum over, which is the only
 * reason either of them needs to know anything about IP. */
u32 ipv4_pseudo_sum(ipv4_addr src, ipv4_addr dst, u8 proto, u16 length);

/* ---- ICMP --------------------------------------------------------------- */

void icmp_input(ipv4_addr src, const u8 *packet, size_t len);

/* Send one echo request. The reply, if it comes, is counted and reported
 * through icmp_last_reply() rather than returned - there is no blocking
 * receive in this stack, so a caller polls. */
bool icmp_echo_request(ipv4_addr dst, u16 id, u16 seq, size_t payload_len);

struct icmp_reply {
    bool valid;
    ipv4_addr from;
    u16 id, seq;
    u64 at_ms;
    u32 ttl;
};

const struct icmp_reply *icmp_last_reply(void);
void icmp_clear_last_reply(void);

/* ---- UDP ---------------------------------------------------------------- */

#define UDP_HDR_LEN 8

/* `dst` is the datagram's destination address, which this needs and ICMP and
 * TCP do not. UDP's checksum covers a pseudo-header containing it, so a
 * broadcast datagram - which is what a DHCP reply may be - checksums against
 * 255.255.255.255 and not against this machine's address. Verifying against
 * the local address instead made every DHCP reply fail its checksum, which
 * presented as a silent timeout. */
void udp_input(ipv4_addr src, ipv4_addr dst, const u8 *packet, size_t len);
bool udp_send(ipv4_addr dst, u16 src_port, u16 dst_port, const void *payload,
              size_t len);

/* A port that echoes whatever arrives back to its sender. Deliberately not a
 * sockets API: see docs/NETWORK.md. Port 0 disables it. */
void udp_set_echo_port(u16 port);
u16 udp_echo_port(void);

struct udp_last {
    bool valid;
    ipv4_addr src;
    u16 src_port, dst_port;
    u16 len;
    u8 data[64];
};

const struct udp_last *udp_last_datagram(void);

/* ---- bound ports, and a receive queue ----------------------------------
 *
 * The echo port above is not a receive API: it answers from inside
 * udp_input() and keeps nothing. Everything that actually *uses* UDP needs
 * the opposite - a datagram sitting somewhere until the code that asked for
 * it gets round to looking - and three things need it at once: DHCP, DNS,
 * and a datagram socket.
 *
 * So a port can be bound, and a bound port has a short queue. The queue is
 * short on purpose: a datagram protocol has no flow control, so the only
 * question is how many datagrams to keep before dropping the oldest, and a
 * deep queue on a kernel with no way to apply back pressure just delays the
 * drop while using more memory. Two is enough for a request/reply protocol,
 * which is what all three of these are.
 *
 * Dropping the *oldest* rather than the newest is the choice worth naming:
 * for a request/reply protocol the newest datagram is the one most likely to
 * be the reply being waited for, and a full queue of stale datagrams that
 * shuts out the answer is the failure mode to avoid.
 */
#define UDP_BINDINGS_MAX 8
#define UDP_QUEUE_DEPTH  2
/* A DNS reply without EDNS cannot exceed 512 bytes and a DHCP message is
 * smaller still, so this is generous for what the kernel actually speaks. A
 * larger datagram is truncated, and `truncated` says so rather than the
 * caller having to infer it from a length that looks plausible. */
#define UDP_DGRAM_MAX    1024

struct udp_dgram {
    ipv4_addr src;
    u16 src_port;
    u16 dst_port;
    u16 len;      /* bytes in `data`                              */
    u16 real_len; /* what arrived, before truncation          */
    bool truncated;
    u8 data[UDP_DGRAM_MAX];
};

/* Claim a port. False if it is already bound, is the echo port, or there is
 * no free binding. */
bool udp_bind(u16 port);
void udp_unbind(u16 port);
/* Claim an unused port in the ephemeral range, or 0. */
u16 udp_bind_ephemeral(void);
bool udp_port_bound(u16 port);

/* Take the oldest queued datagram for a bound port. Returns its payload
 * length, 0 when the queue is empty, or a negative SYS_E* value when the
 * port is not bound. */
i32 udp_recv(u16 port, struct udp_dgram *out);
u32 udp_queued(u16 port);
/* Datagrams dropped because a bound port's queue was full, for the shell. */
u32 udp_dropped(void);

/* ---- TCP ---------------------------------------------------------------- */

void tcp_input(ipv4_addr src, const u8 *packet, size_t len);

/* How many connections exist at once. Small and fixed, like every other
 * table in this kernel: a connection holds send and receive buffers, so the
 * ceiling is what stops a peer opening connections until the heap is gone.
 *
 * It used to be one. One was enough for an echo server and is not enough for
 * anything that *initiates* a connection, because a socket layer needs the
 * listener and the accepted connection to exist at the same time - and an
 * HTTP client needs a connection while the echo port is still listening. */
#define TCP_MAX_CONNS 6

/* The echo service, kept because the network CI scenario asserts on it.
 * Port 0 disables it. A connection accepted on the echo port echoes; one
 * handed to a socket does not, which is a flag on the connection rather than
 * a property of the stack. */
void tcp_listen(u16 port);
u16 tcp_listen_port(void);

enum tcp_state {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    /* An active open, waiting for the peer's SYN-ACK. New with connect():
     * the stack could only ever be the passive side before, which is why
     * this state had nothing to do. */
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_TIME_WAIT,
};

const char *tcp_state_name(enum tcp_state s);

struct tcp_status {
    enum tcp_state state;
    u16 local_port, remote_port;
    ipv4_addr remote;
    u32 snd_nxt, snd_una, rcv_nxt;
    u32 bytes_in, bytes_out;
    u32 segments_in, segments_out;
    u32 retransmits;
    u32 out_of_order_dropped;
    /* Congestion control, exposed so that `net` can show it and the test
     * suite can assert on it. A congestion window that is never reported is
     * a congestion window nobody can tell is working. */
    u32 cwnd;      /* the congestion window, in bytes       */
    u32 ssthresh;  /* where slow start gives way            */
    u32 srtt_ms;   /* smoothed round-trip time              */
    u32 rttvar_ms; /* its variation                         */
    u32 rto_ms;    /* the current retransmission timeout    */
    u32 dup_acks;
    u32 fast_retransmits;
    u32 timeouts;
    bool in_recovery;
};

/* The status of the first connection that is not closed, or of connection 0
 * when they all are. Kept for `net`, which reports one connection. */
void tcp_get_status(struct tcp_status *out);
bool tcp_get_status_at(int handle, struct tcp_status *out);

/* ---- connections, by handle --------------------------------------------
 *
 * A handle is an index into the connection table with a generation counter
 * folded in, so that a handle to a connection that has since closed and been
 * reused is rejected rather than silently referring to a different peer.
 * That is the same problem a file descriptor has, and it is solved here
 * rather than in the socket layer because the stack is what reuses the slot.
 */

/* Start an active open. Returns a handle, or a negative SYS_E* value.
 * Non-blocking: the connection is in TCP_SYN_SENT when this returns, and the
 * caller polls tcp_handle_state() or waits on it. */
int tcp_connect(ipv4_addr dst, u16 port, u16 local_port);

/* Claim a connection that arrived on `port` and has completed its handshake.
 * Returns a handle or a negative SYS_E* value. The connection stops being a
 * candidate for a second accept once claimed. */
int tcp_accept(u16 port);

/* Register and unregister a passive open on a port, for the socket layer.
 * Separate from tcp_listen(), which is the echo service. */
bool tcp_bind_listener(u16 port);
void tcp_unbind_listener(u16 port);

enum tcp_state tcp_handle_state(int handle);
/* Queue data. Returns bytes accepted, which may be short when the send
 * buffer is full - that is how back pressure is expressed. */
i32 tcp_send(int handle, const void *buf, u32 len);
/* Take data out of the receive buffer. 0 means none available; a closed peer
 * with an empty buffer is reported through tcp_recv_eof(). */
i32 tcp_recv(int handle, void *buf, u32 len);
u32 tcp_recv_available(int handle);
bool tcp_recv_eof(int handle);
u32 tcp_send_room(int handle);
/* Begin an orderly shutdown: the FIN goes out after whatever is queued, and
 * the connection can still be read from until the peer closes too. */
void tcp_shutdown(int handle);

/* Give the connection back to the stack. The handle is invalid afterwards.
 *
 * This is separate from tcp_shutdown() because a close has to outlive its
 * owner. The FIN exchange takes at least a round trip, and TIME_WAIT a good
 * deal longer; a process that exits does not wait for either. So close()
 * relinquishes the connection and the stack carries it the rest of the way,
 * reclaiming the slot when the exchange finishes - which is exactly why a
 * handle carries a generation counter, because the slot outlives the handle
 * by design rather than by accident. */
void tcp_close(int handle);

/* Abort immediately with a reset, and release the slot. For an owner that
 * has no use for an orderly close - and for the test suite, which needs a
 * connection gone now rather than in two seconds. */
void tcp_abort(int handle);
/* An unused ephemeral port, for an active open that did not name one. */
u16 tcp_ephemeral_port(void);

/* Called from the network poll loop. Retransmits an unacknowledged segment
 * once its timer expires and times a half-open connection out. */
void tcp_tick(void);

/* ---- the poll loop ------------------------------------------------------ */

/* Pull frames from the device and run the protocol timers. Called by the
 * network kernel thread, and directly by the shell's blocking commands so
 * that a `ping` does not depend on the scheduler running the thread. */
u32 net_poll(u32 max_frames);

#endif /* _NET_NET_H */
