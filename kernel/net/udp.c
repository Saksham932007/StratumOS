/* StratumOS - UDP. Eight bytes of header and one real decision in it. */
#define LOG_TAG "udp"

#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/syscall_abi.h>

#include <net/net.h>

struct udp_hdr {
    be16 src_port;
    be16 dst_port;
    be16 length; /* header + payload, unlike every other length on the wire */
    be16 checksum;
} PACKED;

static u16 echo_port;
static struct udp_last last;

/* ---- bound ports -------------------------------------------------------
 *
 * A fixed array, searched linearly. Eight bindings does not justify a hash
 * table, and a linear search over eight entries on every datagram is
 * cheaper than the hash would be. See the note in net.h on why the queues
 * are two deep and why a full one drops its oldest entry.
 */
struct udp_binding {
    bool used;
    u16 port;
    u32 head;  /* the next to be taken out    */
    u32 count; /* how many are queued          */
    struct udp_dgram q[UDP_QUEUE_DEPTH];
};

static struct udp_binding bindings[UDP_BINDINGS_MAX];
static u32 dropped;
static u16 next_ephemeral = 49152;

static struct udp_binding *binding_for(u16 port)
{
    if (!port)
        return NULL;

    for (u32 i = 0; i < UDP_BINDINGS_MAX; i++)
        if (bindings[i].used && bindings[i].port == port)
            return &bindings[i];

    return NULL;
}

bool udp_port_bound(u16 port)
{
    return binding_for(port) != NULL;
}

bool udp_bind(u16 port)
{
    if (!port)
        return false;
    /* The echo service answers from inside udp_input() and keeps nothing, so
     * a bound port on top of it would see datagrams that had already been
     * replied to. One owner per port. */
    if (echo_port && port == echo_port)
        return false;
    if (binding_for(port))
        return false;

    for (u32 i = 0; i < UDP_BINDINGS_MAX; i++) {
        if (!bindings[i].used) {
            memset(&bindings[i], 0, sizeof(bindings[i]));
            bindings[i].used = true;
            bindings[i].port = port;
            return true;
        }
    }

    return false;
}

void udp_unbind(u16 port)
{
    struct udp_binding *b = binding_for(port);

    if (b)
        memset(b, 0, sizeof(*b));
}

u16 udp_bind_ephemeral(void)
{
    for (u32 tries = 0; tries < 16384; tries++) {
        u16 port = next_ephemeral++;

        if (next_ephemeral < 49152)
            next_ephemeral = 49152;

        if (udp_bind(port))
            return port;
    }

    return 0;
}

u32 udp_queued(u16 port)
{
    struct udp_binding *b = binding_for(port);

    return b ? b->count : 0;
}

u32 udp_dropped(void)
{
    return dropped;
}

i32 udp_recv(u16 port, struct udp_dgram *out)
{
    struct udp_binding *b = binding_for(port);

    if (!b)
        return SYS_EINVAL;
    if (!b->count)
        return 0;

    *out = b->q[b->head];
    b->head = (b->head + 1) % UDP_QUEUE_DEPTH;
    b->count--;
    return (i32)out->len;
}

/* Put a datagram on a bound port's queue, dropping the oldest if it is
 * full. Returns false when nothing was listening, which is what decides
 * whether the no-port counter moves. */
static bool enqueue(struct udp_binding *b, ipv4_addr src, u16 src_port,
                    u16 dst_port, const u8 *payload, u16 len)
{
    if (b->count == UDP_QUEUE_DEPTH) {
        /* Drop the oldest. See net.h: for a request/reply protocol the new
         * datagram is more likely to be the awaited reply than the stale one
         * already queued. */
        b->head = (b->head + 1) % UDP_QUEUE_DEPTH;
        b->count--;
        dropped++;
    }

    u32 slot = (b->head + b->count) % UDP_QUEUE_DEPTH;
    struct udp_dgram *d = &b->q[slot];

    d->src = src;
    d->src_port = src_port;
    d->dst_port = dst_port;
    d->real_len = len;
    d->truncated = len > UDP_DGRAM_MAX;
    d->len = d->truncated ? UDP_DGRAM_MAX : len;
    memcpy(d->data, payload, d->len);
    b->count++;
    return true;
}

bool udp_send(ipv4_addr dst, u16 src_port, u16 dst_port, const void *payload,
              size_t len)
{
    const struct net_config *cfg = net_get_config();
    struct net_frame f;
    struct udp_hdr *h;
    u8 *body;

    if (len + UDP_HDR_LEN > NET_PAYLOAD_MAX)
        return false;

    u16 total = (u16)(UDP_HDR_LEN + len);

    /* One frame for the datagram, its IPv4 header and its Ethernet header -
     * see struct net_frame in net.h. */
    net_frame_reset(&f);

    body = net_frame_append(&f, total);
    if (!body)
        return false;

    h = (struct udp_hdr *)body;
    h->src_port = htons(src_port);
    h->dst_port = htons(dst_port);
    h->length = htons(total);
    h->checksum = 0;
    memcpy(body + UDP_HDR_LEN, payload, len);

    /* UDP's checksum covers a pseudo-header of IP addresses as well as its
     * own header and payload, so that a datagram delivered to the wrong host
     * fails rather than being accepted.
     *
     * The one real decision: a computed checksum of zero has to be sent as
     * 0xFFFF, because zero is the reserved value meaning "no checksum". They
     * are the same number in one's complement, so this loses nothing - and
     * without it, roughly one datagram in 65536 is sent with its checksum
     * silently disabled. */
    u32 sum = ipv4_pseudo_sum(cfg->addr, dst, IPPROTO_UDP, total);

    h->checksum = checksum_finish(checksum_partial(body, total, sum));
    if (h->checksum == 0)
        h->checksum = 0xFFFFu;

    if (!ipv4_output_frame(&f, dst, IPPROTO_UDP))
        return false;

    net_stats()->udp_tx++;
    return true;
}

void udp_input(ipv4_addr src, ipv4_addr dst, const u8 *packet, size_t len)
{
    const struct udp_hdr *h = (const struct udp_hdr *)packet;
    struct net_stats *s = net_stats();

    s->udp_rx++;

    if (len < UDP_HDR_LEN)
        return;

    u16 total = ntohs(h->length);

    /* The length field is the sender's, and it must agree with what arrived.
     * A header claiming more than the frame holds would make the payload
     * bounds below read past the buffer. */
    if (total < UDP_HDR_LEN || total > len)
        return;

    /* A zero checksum means the sender chose not to compute one, which is
     * legal in IPv4 UDP. Anything else has to verify. */
    if (h->checksum != 0) {
        /* Against the datagram's own destination, not this machine's
         * address: see the note on this function in net.h. */
        u32 sum = ipv4_pseudo_sum(src, dst, IPPROTO_UDP, total);

        if (checksum_finish(checksum_partial(packet, total, sum)) != 0) {
            s->udp_bad_checksum++;
            return;
        }
    }

    u16 dst_port = ntohs(h->dst_port);
    u16 src_port = ntohs(h->src_port);
    const u8 *payload = packet + UDP_HDR_LEN;
    u16 payload_len = (u16)(total - UDP_HDR_LEN);

    /* Kept for the shell, so that `net` can show what arrived without a
     * receive API existing. Truncated to what the struct holds, and the real
     * length recorded, so a reader can tell the difference. */
    last.valid = true;
    last.src = src;
    last.src_port = src_port;
    last.dst_port = dst_port;
    last.len = payload_len;
    memcpy(last.data, payload,
           payload_len < sizeof(last.data) ? payload_len : sizeof(last.data));

    struct udp_binding *b = binding_for(dst_port);

    if (b) {
        enqueue(b, src, src_port, dst_port, payload, payload_len);
        return;
    }

    if (!echo_port || dst_port != echo_port) {
        /* A real stack sends an ICMP port-unreachable here. This one counts
         * it instead: generating ICMP errors needs rate limiting, or a
         * machine can be made to answer a flood of datagrams with a flood of
         * errors - and that is the same console-flooding shape the syscall
         * fuzzer found in phase 7. Counted, not silent. */
        s->udp_no_port++;
        return;
    }

    char ib[IPV4_STR_LEN];

    pr_debug("echoing %u byte(s) back to %s:%u", payload_len,
             ipv4_str(src, ib, sizeof(ib)), src_port);

    udp_send(src, dst_port, src_port, payload, payload_len);
}

void udp_set_echo_port(u16 port)
{
    /* A bound port and the echo service cannot share a number - see
     * udp_bind(). The echo port wins, because it is set at boot from the
     * kernel's own configuration and a binding is set by whatever is
     * running. */
    udp_unbind(port);
    echo_port = port;
}

u16 udp_echo_port(void)
{
    return echo_port;
}

const struct udp_last *udp_last_datagram(void)
{
    return &last;
}
