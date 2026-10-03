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
    u32 tcp_connections_accepted, tcp_bytes_echoed;
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

void udp_input(ipv4_addr src, const u8 *packet, size_t len);
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

/* ---- TCP ---------------------------------------------------------------- */

void tcp_input(ipv4_addr src, const u8 *packet, size_t len);

/* Listen on one port, accept one connection at a time, echo what arrives,
 * and close when the peer does. The honest limits are in docs/NETWORK.md and
 * restated in tcp.c. Port 0 disables it. */
void tcp_listen(u16 port);
u16 tcp_listen_port(void);

enum tcp_state {
    TCP_CLOSED = 0,
    TCP_LISTEN,
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
};

void tcp_get_status(struct tcp_status *out);

/* Called from the network poll loop. Retransmits an unacknowledged segment
 * once its timer expires and times a half-open connection out. */
void tcp_tick(void);

/* ---- the poll loop ------------------------------------------------------ */

/* Pull frames from the device and run the protocol timers. Called by the
 * network kernel thread, and directly by the shell's blocking commands so
 * that a `ping` does not depend on the scheduler running the thread. */
u32 net_poll(u32 max_frames);

#endif /* _NET_NET_H */
