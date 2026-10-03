/* StratumOS - ARP: turning an IPv4 address into a MAC address.
 *
 * The protocol is small enough to state completely: broadcast "who has
 * 10.0.2.2, tell 10.0.2.15", and whoever has it answers with its MAC. The
 * parts worth attention are the cache and what happens on a miss.
 */
#define LOG_TAG "arp"

#include <drivers/timer.h>

#include <kernel/log.h>
#include <kernel/string.h>

#include <net/net.h>

#define ARP_HTYPE_ETHERNET 1
#define ARP_OP_REQUEST     1
#define ARP_OP_REPLY       2

/* Long enough that a burst of traffic to one host does not re-ARP, short
 * enough that a machine whose address moved is noticed. The usual value. */
#define ARP_ENTRY_TTL_MS   (2 * 60 * 1000)

struct arp_packet {
    be16 htype;
    be16 ptype;
    u8 hlen;
    u8 plen;
    be16 op;
    struct mac_addr sender_mac;
    ipv4_addr sender_ip;
    struct mac_addr target_mac;
    ipv4_addr target_ip;
} PACKED;

static struct arp_entry cache[ARP_CACHE_ENTRIES];

/* Insert or refresh. Replaces the oldest entry when full rather than
 * refusing: a full cache that cannot learn is a machine that stops being
 * able to talk to anything new, which is worse than forgetting something. */
static void cache_put(ipv4_addr ip, const struct mac_addr *mac)
{
    u32 oldest = 0;
    u64 oldest_at = (u64)-1;

    for (u32 i = 0; i < ARP_CACHE_ENTRIES; i++) {
        if (cache[i].valid && cache[i].ip == ip) {
            cache[i].mac = *mac;
            cache[i].learned_ms = timer_ms();
            return;
        }

        if (!cache[i].valid) {
            oldest = i;
            oldest_at = 0;
            continue;
        }

        if (cache[i].learned_ms < oldest_at) {
            oldest_at = cache[i].learned_ms;
            oldest = i;
        }
    }

    cache[oldest].ip = ip;
    cache[oldest].mac = *mac;
    cache[oldest].valid = true;
    cache[oldest].learned_ms = timer_ms();
}

static bool cache_get(ipv4_addr ip, struct mac_addr *out)
{
    u64 now = timer_ms();

    for (u32 i = 0; i < ARP_CACHE_ENTRIES; i++) {
        if (!cache[i].valid || cache[i].ip != ip)
            continue;

        if (now - cache[i].learned_ms > ARP_ENTRY_TTL_MS) {
            cache[i].valid = false;
            return false;
        }

        *out = cache[i].mac;
        return true;
    }

    return false;
}

static bool send(u16 op, const struct mac_addr *target_mac, ipv4_addr target_ip)
{
    const struct net_config *cfg = net_get_config();
    const struct net_device *dev = net_device();
    struct arp_packet p;

    if (!dev)
        return false;

    p.htype = htons(ARP_HTYPE_ETHERNET);
    p.ptype = htons(ETHERTYPE_IPV4);
    p.hlen = ETH_ALEN;
    p.plen = 4;
    p.op = htons(op);
    p.sender_mac = dev->mac;
    p.sender_ip = cfg->addr;
    p.target_mac = (op == ARP_OP_REQUEST) ? mac_zero : *target_mac;
    p.target_ip = target_ip;

    /* A request goes to the broadcast address, because the point of asking
     * is that we do not know who to ask. A reply is unicast back to whoever
     * asked - which is why target_mac is carried through. */
    const struct mac_addr *dst =
        (op == ARP_OP_REQUEST) ? &mac_broadcast : target_mac;

    if (!eth_output(dst, ETHERTYPE_ARP, &p, sizeof(p)))
        return false;

    net_stats()->arp_tx++;
    if (op == ARP_OP_REQUEST)
        net_stats()->arp_requests_sent++;
    else
        net_stats()->arp_replies_sent++;

    return true;
}

bool arp_request(ipv4_addr ip)
{
    return send(ARP_OP_REQUEST, &mac_broadcast, ip);
}

bool arp_resolve(ipv4_addr ip, struct mac_addr *out)
{
    /* Anything not on our own link is reached through the gateway, so it is
     * the gateway's MAC that has to be resolved. Doing this here rather than
     * in the caller means every caller gets it right. */
    ipv4_addr hop = net_next_hop(ip);

    if (cache_get(hop, out))
        return true;

    /* A miss sends a request and fails. The caller's packet is dropped - see
     * the comment on arp_resolve() in net.h for why that is the right answer
     * at this size rather than a queue. */
    arp_request(hop);
    return false;
}

void arp_input(const struct mac_addr *from, const u8 *packet, size_t len)
{
    const struct arp_packet *p = (const struct arp_packet *)packet;
    const struct net_config *cfg = net_get_config();

    net_stats()->arp_rx++;

    if (len < sizeof(*p))
        return;

    /* Every field checked before any is used. An ARP packet claiming a
     * 17-byte hardware address is not something to accommodate. */
    if (ntohs(p->htype) != ARP_HTYPE_ETHERNET ||
        ntohs(p->ptype) != ETHERTYPE_IPV4 || p->hlen != ETH_ALEN ||
        p->plen != 4)
        return;

    u16 op = ntohs(p->op);

    /* Learn from anything that passes, request or reply. The sender's
     * mapping is in both, and a machine that only learns from replies has to
     * ARP for every host that ARPs for it.
     *
     * Learning from the *sender_mac field* rather than the Ethernet source
     * is the standard's wording, and they can differ - a proxy ARP responder
     * answers for an address that is not its own. Cross-checking them is
     * what a real stack does to spot spoofing; this one does not, and says
     * so rather than implying the check exists. */
    if (p->sender_ip && !mac_is_multicast(&p->sender_mac))
        cache_put(p->sender_ip, &p->sender_mac);

    if (op != ARP_OP_REQUEST)
        return;

    /* A who-has for us gets an answer. A who-has for anyone else is not our
     * business: answering would be proxy ARP, which is a deliberate
     * configuration and not a default. */
    if (p->target_ip != cfg->addr || !cfg->addr)
        return;

    char ib[IPV4_STR_LEN], mb[MAC_STR_LEN];

    pr_debug("who-has %s from %s; replying",
             ipv4_str(p->target_ip, ib, sizeof(ib)),
             mac_str(from, mb, sizeof(mb)));

    send(ARP_OP_REPLY, &p->sender_mac, p->sender_ip);
}

u32 arp_cache_count(void)
{
    u32 n = 0;

    for (u32 i = 0; i < ARP_CACHE_ENTRIES; i++)
        if (cache[i].valid)
            n++;

    return n;
}

const struct arp_entry *arp_cache_at(u32 index)
{
    u32 n = 0;

    for (u32 i = 0; i < ARP_CACHE_ENTRIES; i++) {
        if (!cache[i].valid)
            continue;
        if (n++ == index)
            return &cache[i];
    }

    return NULL;
}

void arp_cache_clear(void)
{
    memset(cache, 0, sizeof(cache));
}
