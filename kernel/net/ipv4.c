/* StratumOS - IPv4, and the ICMP echo that makes the machine pingable. */
#define LOG_TAG "ip"

#include <drivers/timer.h>

#include <kernel/log.h>
#include <kernel/string.h>

#include <net/net.h>

struct ipv4_hdr {
    u8 version_ihl; /* 4 bits each */
    u8 dscp_ecn;
    be16 total_length;
    be16 id;
    be16 flags_fragment; /* 3 bits of flags, 13 of offset */
    u8 ttl;
    u8 protocol;
    be16 checksum;
    ipv4_addr src;
    ipv4_addr dst;
} PACKED;

#define IPV4_FLAG_DF     0x4000u
#define IPV4_FLAG_MF     0x2000u
#define IPV4_FRAG_OFFSET 0x1FFFu

#define IPV4_DEFAULT_TTL 64

/* Monotonic, and that is all it has to be. The identification field only
 * matters for reassembling fragments, and this stack neither sends nor
 * accepts them - so a counter is correct and a random value would be
 * pretending to a property nothing uses. */
static u16 next_id = 1;

u32 ipv4_pseudo_sum(ipv4_addr src, ipv4_addr dst, u8 proto, u16 length)
{
    /* The twelve bytes TCP and UDP checksum *in addition to* their own
     * header and payload: source, destination, a zero, the protocol, and the
     * length. It exists so that a datagram delivered to the wrong host or
     * the wrong protocol fails its checksum rather than being accepted - the
     * transport checksum covers the addressing the transport header does not
     * contain.
     *
     * Built by summing the same bytes that would be on the wire, so
     * checksum_partial's byte-order argument holds here too. */
    u8 pseudo[12];

    memcpy(pseudo + 0, &src, 4);
    memcpy(pseudo + 4, &dst, 4);
    pseudo[8] = 0;
    pseudo[9] = proto;
    pseudo[10] = (u8)(length >> 8);
    pseudo[11] = (u8)length;

    return checksum_partial(pseudo, sizeof(pseudo), 0);
}

bool ipv4_output_frame(struct net_frame *f, ipv4_addr dst, u8 proto)
{
    const struct net_config *cfg = net_get_config();
    struct ipv4_hdr *h;
    struct mac_addr next;
    u32 payload_len = net_frame_len(f);

    if (!cfg->addr)
        return false;

    /* No fragmentation, so a payload that does not fit is refused. Refusing
     * is the honest failure: truncating would send a datagram whose header
     * says one length and whose contents are another, and the receiver would
     * blame its own stack. */
    if (payload_len + IPV4_HDR_LEN > ETH_MTU)
        return false;

    if (!arp_resolve(dst, &next))
        return false; /* the request is away; this datagram is dropped */

    h = (struct ipv4_hdr *)net_frame_prepend(f, IPV4_HDR_LEN);
    if (!h)
        return false;

    h->version_ihl = 0x45; /* version 4, 5 words of header, no options */
    h->dscp_ecn = 0;
    h->total_length = htons((u16)(IPV4_HDR_LEN + payload_len));
    h->id = htons(next_id++);
    h->flags_fragment = htons(IPV4_FLAG_DF);
    h->ttl = IPV4_DEFAULT_TTL;
    h->protocol = proto;
    h->checksum = 0;
    h->src = cfg->addr;
    h->dst = dst;

    /* Computed over the header with the checksum field zero, which is why it
     * is assigned after. The header has no options, so this is always 20
     * bytes. */
    h->checksum = checksum(h, IPV4_HDR_LEN);

    if (!eth_output_frame(f, &next, ETHERTYPE_IPV4))
        return false;

    net_stats()->ip_tx++;
    return true;
}

bool ipv4_output(ipv4_addr dst, u8 proto, const void *payload, size_t len)
{
    struct net_frame f;
    u8 *p;

    net_frame_reset(&f);

    if (len > NET_PAYLOAD_MAX)
        return false;

    p = net_frame_append(&f, (u32)len);
    if (!p)
        return false;

    memcpy(p, payload, len);
    return ipv4_output_frame(&f, dst, proto);
}

void ipv4_input(const struct mac_addr *from, const u8 *packet, size_t len)
{
    const struct ipv4_hdr *h = (const struct ipv4_hdr *)packet;
    const struct net_config *cfg = net_get_config();
    struct net_stats *s = net_stats();

    UNUSED(from);
    s->ip_rx++;

    if (len < IPV4_HDR_LEN)
        return;

    if ((h->version_ihl >> 4) != 4) {
        s->ip_bad_version++;
        return;
    }

    u32 ihl = (u32)(h->version_ihl & 0x0F) * 4;

    /* The header length is a field an attacker controls, and everything
     * below indexes by it. Both bounds, before it is used for anything. */
    if (ihl < IPV4_HDR_LEN || ihl > len)
        return;

    u16 total = ntohs(h->total_length);

    if (total < ihl || total > len) {
        /* total_length longer than the frame is either a truncated capture
         * or a lie; shorter is padding, which is legal - an Ethernet frame
         * has a 60-byte minimum and a short datagram gets padded to reach
         * it. So `total` is the authority for the payload length from here
         * on, not `len`. */
        if (total < ihl)
            return;
        return;
    }

    if (checksum(packet, ihl) != 0) {
        s->ip_bad_checksum++;
        return;
    }

    /* Fragments are dropped and counted rather than reassembled.
     * Reassembly needs a hole list, a timer and a bound on memory a remote
     * host controls - it is where several famous denial-of-service bugs
     * lived - and nothing this stack sends or receives is fragmented. Said
     * out loud because silently dropping them would look like packet loss. */
    u16 ff = ntohs(h->flags_fragment);

    if ((ff & IPV4_FLAG_MF) || (ff & IPV4_FRAG_OFFSET)) {
        s->ip_fragments_dropped++;
        return;
    }

    /* Ours, or broadcast. A datagram for anyone else reached us because
     * something is flooding; it is not ours to forward, because forwarding
     * is a router's job and this is a host. */
    bool ours = (h->dst == cfg->addr);
    bool bcast = (h->dst == 0xFFFFFFFFu) ||
                 (cfg->netmask && (h->dst == (cfg->addr | ~cfg->netmask)));

    if (!ours && !bcast) {
        s->ip_not_ours++;
        return;
    }

    const u8 *payload = packet + ihl;
    size_t payload_len = total - ihl;

    switch (h->protocol) {
    case IPPROTO_ICMP:
        icmp_input(h->src, payload, payload_len);
        break;
    case IPPROTO_UDP:
        udp_input(h->src, payload, payload_len);
        break;
    case IPPROTO_TCP:
        tcp_input(h->src, payload, payload_len);
        break;
    default:
        s->ip_unknown_proto++;
        break;
    }
}

/* ---- ICMP --------------------------------------------------------------- */

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

struct icmp_hdr {
    u8 type;
    u8 code;
    be16 checksum;
    be16 id;
    be16 seq;
} PACKED;

static struct icmp_reply last_reply;

bool icmp_echo_request(ipv4_addr dst, u16 id, u16 seq, size_t payload_len)
{
    u8 buf[sizeof(struct icmp_hdr) + 64];
    struct icmp_hdr *h = (struct icmp_hdr *)buf;

    if (payload_len > 64)
        payload_len = 64;

    h->type = ICMP_ECHO_REQUEST;
    h->code = 0;
    h->checksum = 0;
    h->id = htons(id);
    h->seq = htons(seq);

    /* A recognisable pattern rather than zeros, so that a reply which comes
     * back with the payload mangled is distinguishable from one that comes
     * back correct. The echo reply must quote the request's data verbatim. */
    for (size_t i = 0; i < payload_len; i++)
        buf[sizeof(*h) + i] = (u8)('a' + (i % 26));

    size_t total = sizeof(*h) + payload_len;

    /* ICMP's checksum covers the ICMP header and payload only - no
     * pseudo-header, unlike UDP and TCP. That asymmetry is a real part of
     * the protocol and getting it wrong gives a checksum every host
     * rejects. */
    h->checksum = checksum(buf, total);

    return ipv4_output(dst, IPPROTO_ICMP, buf, total);
}

void icmp_input(ipv4_addr src, const u8 *packet, size_t len)
{
    const struct icmp_hdr *h = (const struct icmp_hdr *)packet;
    struct net_stats *s = net_stats();

    s->icmp_rx++;

    if (len < sizeof(*h))
        return;

    if (checksum(packet, len) != 0) {
        s->icmp_bad_checksum++;
        return;
    }

    if (h->type == ICMP_ECHO_REPLY) {
        s->icmp_echo_replies_received++;
        last_reply.valid = true;
        last_reply.from = src;
        last_reply.id = ntohs(h->id);
        last_reply.seq = ntohs(h->seq);
        last_reply.at_ms = timer_ms();
        return;
    }

    if (h->type != ICMP_ECHO_REQUEST || h->code != 0)
        return;

    s->icmp_echo_requests++;

    /* The reply is the request with the type changed and the checksum
     * recomputed: the identifier, the sequence number and every byte of
     * payload have to come back verbatim, because that is how the sender
     * matches a reply to a request. Rebuilding it from scratch is how an
     * implementation ends up echoing the wrong payload length.
     *
     * Bounded by what will fit, since the request's length is the sender's
     * choice. */
    /* Sized to exactly what ipv4_output() will accept, so that the bound
     * checked here and the bound enforced there are the same number rather
     * than two numbers that happen to be compatible. A larger buffer would
     * accept a request this function then silently failed to answer. */
    u8 reply[NET_PAYLOAD_MAX];

    if (len > sizeof(reply))
        return;

    memcpy(reply, packet, len);

    struct icmp_hdr *rh = (struct icmp_hdr *)reply;

    rh->type = ICMP_ECHO_REPLY;
    rh->checksum = 0;
    rh->checksum = checksum(reply, len);

    char ib[IPV4_STR_LEN];

    pr_debug("echo request from %s id %u seq %u, %u bytes; replying",
             ipv4_str(src, ib, sizeof(ib)), ntohs(h->id), ntohs(h->seq),
             (unsigned)len);

    if (ipv4_output(src, IPPROTO_ICMP, reply, len))
        s->icmp_echo_replies_sent++;
}

const struct icmp_reply *icmp_last_reply(void)
{
    return &last_reply;
}

void icmp_clear_last_reply(void)
{
    memset(&last_reply, 0, sizeof(last_reply));
}
