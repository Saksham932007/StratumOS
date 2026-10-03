/* StratumOS - the network stack's shared parts: addresses, the checksum,
 * the device registration, and Ethernet.
 *
 * See kernel/include/net/net.h for the scope of the stack and
 * docs/NETWORK.md for the design.
 */
#define LOG_TAG "net"

#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/string.h>

#include <net/net.h>

const struct mac_addr mac_broadcast = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
const struct mac_addr mac_zero = {{0, 0, 0, 0, 0, 0}};

static const struct net_device *device;
static struct net_config config;
static struct net_stats stats;

/* ---- addresses ---------------------------------------------------------- */

bool mac_equal(const struct mac_addr *a, const struct mac_addr *b)
{
    return memcmp(a->b, b->b, ETH_ALEN) == 0;
}

bool mac_is_broadcast(const struct mac_addr *a)
{
    return mac_equal(a, &mac_broadcast);
}

bool mac_is_multicast(const struct mac_addr *a)
{
    /* The low bit of the first octet. Broadcast is a special case of
     * multicast by this definition, which is what the standard says. */
    return (a->b[0] & 1) != 0;
}

const char *mac_str(const struct mac_addr *m, char *buf, size_t len)
{
    ksnprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x", m->b[0], m->b[1],
              m->b[2], m->b[3], m->b[4], m->b[5]);
    return buf;
}

const char *ipv4_str(ipv4_addr ip, char *buf, size_t len)
{
    /* Addresses are held in network order, so the first octet is the low
     * byte on this machine. Writing it out this way rather than calling
     * ntohl() keeps the one place that knows that in one expression. */
    const u8 *b = (const u8 *)&ip;

    ksnprintf(buf, len, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

bool ipv4_parse(const char *s, ipv4_addr *out)
{
    u8 octet[4];

    for (u32 i = 0; i < 4; i++) {
        u32 v = 0;
        u32 digits = 0;

        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (u32)(*s - '0');
            if (v > 255)
                return false;
            digits++;
            s++;
        }

        /* A missing or over-long group is a rejection rather than a partial
         * parse: "10.0.2" and "10.0.0.1.5" are both mistakes, and silently
         * accepting either would send a packet somewhere unintended. */
        if (digits == 0 || digits > 3)
            return false;

        octet[i] = (u8)v;

        if (i < 3) {
            if (*s != '.')
                return false;
            s++;
        }
    }

    if (*s != '\0')
        return false;

    *out = IPV4(octet[0], octet[1], octet[2], octet[3]);
    return true;
}

/* ---- the checksum ------------------------------------------------------- */

/* RFC 1071's one's-complement sum of 16-bit words, accumulated in 32 bits so
 * the carries can be folded in at the end rather than after every word.
 *
 * The incremental form takes a running sum so that a TCP or UDP pseudo-header
 * - which is not contiguous with the payload it covers - can be summed
 * without building a temporary buffer to make it contiguous.
 *
 * Endianness here is a trap worth naming. The sum is computed over the bytes
 * *as they lie in memory*, treating each pair as a big-endian 16-bit word,
 * and the result is stored without conversion. That is correct on either
 * endianness because the one's-complement sum is byte-order agnostic in
 * exactly this arrangement - a fact that is true, useful, and completely
 * non-obvious, so it is written down rather than rediscovered. */
u32 checksum_partial(const void *data, size_t len, u32 sum)
{
    const u8 *p = data;

    while (len > 1) {
        sum += (u32)(((u16)p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }

    /* An odd trailing byte is the high half of a word whose low half is
     * zero, not the low half of one. Getting this backwards gives a checksum
     * that is right for every even-length packet, which is most of them. */
    if (len)
        sum += (u32)p[0] << 8;

    return sum;
}

u16 checksum_finish(u32 sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);

    /* The complement, byte-swapped into network order. The sum above was
     * built from big-endian words, so the 16-bit result is in host order and
     * needs one conversion to go on the wire. */
    return htons((u16)~sum);
}

/* ---- the device --------------------------------------------------------- */

void net_set_device(const struct net_device *dev)
{
    device = dev;
}

const struct net_device *net_device(void)
{
    return device;
}

bool net_up(void)
{
    return device != NULL && device->link_up && device->link_up();
}

void net_configure(ipv4_addr addr, ipv4_addr netmask, ipv4_addr gateway)
{
    char a[IPV4_STR_LEN], m[IPV4_STR_LEN], g[IPV4_STR_LEN];

    config.addr = addr;
    config.netmask = netmask;
    config.gateway = gateway;

    pr_info("address %s netmask %s gateway %s", ipv4_str(addr, a, sizeof(a)),
            ipv4_str(netmask, m, sizeof(m)), ipv4_str(gateway, g, sizeof(g)));
}

const struct net_config *net_get_config(void)
{
    return &config;
}

bool net_is_local(ipv4_addr ip)
{
    return (ip & config.netmask) == (config.addr & config.netmask);
}

ipv4_addr net_next_hop(ipv4_addr ip)
{
    /* The entirety of this stack's routing: on-link addresses are reached
     * directly, everything else goes to the one gateway. A routing table
     * would be the right shape for a second interface, and there is not
     * one - see docs/NETWORK.md. */
    return net_is_local(ip) ? ip : config.gateway;
}

struct net_stats *net_stats(void)
{
    return &stats;
}

void net_stats_reset(void)
{
    memset(&stats, 0, sizeof(stats));
}

/* ---- the poll loop ------------------------------------------------------ */

u32 net_poll(u32 max_frames)
{
    u32 handled = 0;

    if (device && device->poll)
        handled = device->poll(max_frames);

    /* The timers run on every poll rather than on a separate tick, so that a
     * retransmission happens even if nothing is arriving - which is exactly
     * the situation a retransmission exists for. */
    tcp_tick();

    return handled;
}

/* The network thread.
 *
 * Receive is interrupt-driven, so this thread exists for the timers: a
 * retransmission has to happen when *nothing* is arriving, which is exactly
 * when no interrupt will come. It also polls, which covers the case of an
 * interrupt lost to a shared line.
 *
 * 10 ms is a compromise. The retransmission timeout is 500 ms, so the timer
 * resolution this gives is twenty times finer than the shortest interval it
 * has to measure - and a kernel thread that wakes 100 times a second to do
 * nothing is cheap, where one that wakes 1000 times is measurable in the
 * profiler. */
static void net_thread(void *arg)
{
    UNUSED(arg);

    for (;;) {
        net_poll(16);
        task_sleep_ms(10);
    }
}

void net_init(void)
{
    /* The addresses QEMU's user-mode networking hands out, which is the only
     * configuration this stack can be tested in without a real network.
     * Static because there is no DHCP client - see docs/NETWORK.md for why
     * that is an ordering decision rather than an omission.
     *
     * 10.0.2.15 is the guest, 10.0.2.2 the gateway, and 10.0.2.3 the DNS
     * server QEMU emulates. Hardcoding them is honest for a kernel whose
     * only network is an emulator; a `ifconfig`-style command lets them be
     * changed. */
    net_configure(IPV4(10, 0, 2, 15), IPV4(255, 255, 255, 0),
                  IPV4(10, 0, 2, 2));

    /* A port that echoes, for each of the two transports. Enough to prove
     * the path works in both directions without a sockets API existing. */
    udp_set_echo_port(7); /* the traditional echo port */
    tcp_listen(7);
}

void net_start(void)
{
    /* Separate from net_init() because it needs the scheduler, and the
     * driver has to come up before it - the controller is found during the
     * PCI scan, which is long before there is anything to schedule.
     *
     * Found the hard way: task_create() asserts it is not called before
     * sched_init(), and that assertion fired on the first boot with a
     * network card attached. The panic named task_create+0x2ff under
     * net_init+0x57, which is the embedded symbol table earning its keep
     * again. */
    if (!device) {
        return; /* no controller; nothing to poll */
    }

    if (!task_create("net", net_thread, NULL))
        pr_warn("no network thread: retransmission timers will not run");
}

/* ---- Ethernet ----------------------------------------------------------- */

struct eth_hdr {
    struct mac_addr dst;
    struct mac_addr src;
    be16 ethertype;
} PACKED;

void eth_input(const u8 *frame, size_t len)
{
    const struct eth_hdr *eh = (const struct eth_hdr *)frame;

    if (!device)
        return; /* a frame arrived before the device registered */

    if (len < ETH_HDR_LEN) {
        stats.rx_dropped_short++;
        return;
    }

    /* Accept our own unicast address, broadcast, and multicast. Anything
     * else arrived because the hardware filter is wider than it should be,
     * or because something upstream is flooding - either way it is not ours
     * and counting it separately is what distinguishes the two. */
    if (!mac_equal(&eh->dst, &device->mac) && !mac_is_multicast(&eh->dst)) {
        stats.rx_dropped_not_ours++;
        return;
    }

    const u8 *payload = frame + ETH_HDR_LEN;
    size_t payload_len = len - ETH_HDR_LEN;

    switch (ntohs(eh->ethertype)) {
    case ETHERTYPE_ARP:
        arp_input(&eh->src, payload, payload_len);
        break;
    case ETHERTYPE_IPV4:
        ipv4_input(&eh->src, payload, payload_len);
        break;
    default:
        stats.rx_unknown_ethertype++;
        break;
    }
}

bool eth_output_frame(struct net_frame *f, const struct mac_addr *dst,
                      u16 ethertype)
{
    struct eth_hdr *eh;

    if (!device || !device->transmit)
        return false;

    eh = (struct eth_hdr *)net_frame_prepend(f, ETH_HDR_LEN);
    if (!eh)
        return false; /* a layer above used more than NET_HEADROOM */

    eh->dst = *dst;
    eh->src = device->mac;
    eh->ethertype = htons(ethertype);

    /* No padding to the 60-byte Ethernet minimum here: the controller's
     * TCTL.PSP does it, which is both cheaper and the hardware's job. A
     * driver without that bit would need it done here, and this comment is
     * the only thing connecting the two. */
    return device->transmit(net_frame_data(f), net_frame_len(f));
}

bool eth_output(const struct mac_addr *dst, u16 ethertype, const void *payload,
                size_t len)
{
    /* Built through the frame builder so that the Ethernet header is
     * assembled in exactly one place. The copy this form makes is the price
     * of a caller that already has its payload somewhere else - only ARP,
     * whose packet is 28 bytes. */
    struct net_frame f;
    u8 *p;

    net_frame_reset(&f);

    if (len > NET_PAYLOAD_MAX)
        return false;

    p = net_frame_append(&f, (u32)len);
    if (!p)
        return false;

    memcpy(p, payload, len);
    return eth_output_frame(&f, dst, ethertype);
}
