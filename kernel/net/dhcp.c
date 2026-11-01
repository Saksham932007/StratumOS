/* StratumOS - a DHCP client.
 *
 * WHY FOUR MESSAGES AND NOT TWO
 *
 * The obvious design is "ask for an address, get one". DHCP is DISCOVER,
 * OFFER, REQUEST, ACK instead, and the extra round trip exists for one
 * reason: more than one server may answer. A server that makes an offer has
 * to set that address aside, and if the client simply started using
 * whichever offer arrived first, every other server would hold its own
 * reservation indefinitely for a client that was never coming back. So the
 * client names the offer it took, in the REQUEST, and the servers whose
 * offers were not taken see it broadcast and release what they reserved.
 *
 * The same shape explains why the REQUEST is broadcast rather than unicast
 * to the chosen server, which looks wasteful until you notice that the
 * servers that need to hear it are the ones that were *not* chosen.
 *
 * WHAT THIS CLIENT LEAVES OUT
 *
 *   - no lease renewal. The lease time is recorded and reported and nothing
 *     acts on it, so an address obtained here is used until reboot even if
 *     the lease expires. Renewal is a timer at T1 and a rebind at T2, which
 *     is straightforward and is not what makes DHCP interesting.
 *   - no ARP probe for the offered address before using it. A correct client
 *     checks that nothing else is already answering for the address it was
 *     given, and sends DECLINE if something is.
 *   - no client identifier beyond the hardware address, and no vendor class.
 *   - a DECLINE is never sent, and a NAK ends the attempt rather than
 *     restarting it.
 *
 * THE DECISION THAT MATTERS MOST HERE
 *
 * The kernel's existing address is not touched until an ACK arrives. That is
 * not caution for its own sake: this kernel's whole test suite runs against
 * the static address QEMU hands out, and a client that cleared the address
 * when it started asking would break every scenario for as long as the
 * server took to answer - or permanently, on a network with no server. A
 * configuration attempt that fails has to leave the machine exactly as it
 * was, which means the new configuration is only ever written once, in one
 * place, at the end.
 */
#define LOG_TAG "dhcp"

#include <drivers/timer.h>

#include <kernel/log.h>
#include <kernel/string.h>

#include <net/inet.h>
#include <net/net.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define BOOTREQUEST      1
#define BOOTREPLY        2
#define HTYPE_ETHERNET   1

/* The magic cookie that marks the start of the options area, and the reason
 * the fixed part of the message is 236 bytes rather than 240: BOOTP had a
 * 64-byte vendor-specific area there, and DHCP is BOOTP with that area
 * redefined. The cookie is how a server tells the two apart. */
#define DHCP_MAGIC       0x63825363u

#define DHCPDISCOVER     1
#define DHCPOFFER        2
#define DHCPREQUEST      3
#define DHCPDECLINE      4
#define DHCPACK          5
#define DHCPNAK          6
#define DHCPRELEASE      7

#define OPT_PAD          0
#define OPT_SUBNET_MASK  1
#define OPT_ROUTER       3
#define OPT_DNS          6
#define OPT_HOSTNAME     12
#define OPT_REQUESTED_IP 50
#define OPT_LEASE_TIME   51
#define OPT_MSG_TYPE     53
#define OPT_SERVER_ID    54
#define OPT_PARAM_LIST   55
#define OPT_END          255

/* How long to wait for each reply, and how many times to try. Four seconds
 * is long for an emulator and short for a loaded network; the retry is what
 * covers the difference, since a DHCP message is a datagram and datagrams
 * are lost. */
#define DHCP_REPLY_MS    4000
#define DHCP_MAX_TRIES   3

/* The fixed part of a BOOTP message. 236 bytes, then the cookie, then
 * options. */
struct dhcp_msg {
    u8 op;
    u8 htype;
    u8 hlen;
    u8 hops;
    be32 xid;
    be16 secs;
    be16 flags;
    ipv4_addr ciaddr; /* the client's address, if it has one   */
    ipv4_addr yiaddr; /* "your address": what the server grants */
    ipv4_addr siaddr;
    ipv4_addr giaddr;
    u8 chaddr[16];
    char sname[64];
    char file[128];
    be32 magic;
    u8 options[312];
} PACKED;

_Static_assert(sizeof(struct dhcp_msg) == 240 + 312,
               "the BOOTP fixed part is 236 bytes, then the 4-byte cookie");

static enum dhcp_state state;
static struct dhcp_lease lease;
static u32 xid;
static u64 sent_ms;
static u32 tries;
static bool port_bound;

/* What the OFFER proposed, held between the OFFER and the ACK. Separate from
 * `lease`, which is only written once the ACK arrives - see the note at the
 * top of the file. */
static struct {
    ipv4_addr addr;
    ipv4_addr netmask;
    ipv4_addr gateway;
    ipv4_addr dns;
    ipv4_addr server;
    u32 lease_seconds;
} offer;

const char *dhcp_state_name(enum dhcp_state s)
{
    switch (s) {
    case DHCP_IDLE:
        return "idle";
    case DHCP_DISCOVERING:
        return "discovering";
    case DHCP_REQUESTING:
        return "requesting";
    case DHCP_BOUND:
        return "bound";
    case DHCP_FAILED:
        return "failed";
    }

    return "?";
}

enum dhcp_state dhcp_state(void)
{
    return state;
}

const struct dhcp_lease *dhcp_get_lease(void)
{
    return &lease;
}

/* ---- building a message ------------------------------------------------- */

static u8 *put_opt(u8 *p, u8 code, const void *val, u8 len)
{
    *p++ = code;
    *p++ = len;
    if (len)
        memcpy(p, val, len);
    return p + len;
}

static bool send_msg(u8 type, ipv4_addr requested, ipv4_addr server_id)
{
    const struct net_device *dev = net_device();
    struct net_frame f;
    struct dhcp_msg *m;
    u8 *p;

    if (!dev)
        return false;

    net_frame_reset(&f);

    /* The UDP header is prepended by hand rather than through udp_send(),
     * because udp_send() routes through ipv4_output_frame() and this
     * datagram cannot be routed - it is a broadcast from an address this
     * machine does not have yet. */
    struct {
        be16 src_port, dst_port, length, checksum;
    } PACKED *uh;

    u32 body = sizeof(struct dhcp_msg);
    u8 *all = net_frame_append(&f, UDP_HDR_LEN + body);

    if (!all)
        return false;

    uh = (void *)all;
    m = (struct dhcp_msg *)(all + UDP_HDR_LEN);

    memset(m, 0, sizeof(*m));
    m->op = BOOTREQUEST;
    m->htype = HTYPE_ETHERNET;
    m->hlen = ETH_ALEN;
    m->xid = htonl(xid);
    /* The broadcast flag, asking the server to reply by broadcast rather
     * than unicast to an address the client is not yet using. A client that
     * does not set this has to be able to receive a unicast datagram for an
     * address it has not configured, which this stack cannot do: ipv4_input
     * drops anything that is neither its own address nor a broadcast. So the
     * flag is not an optimisation here, it is what makes the reply arrive. */
    m->flags = htons(0x8000);
    memcpy(m->chaddr, dev->mac.b, ETH_ALEN);
    m->magic = htonl(DHCP_MAGIC);

    p = m->options;
    p = put_opt(p, OPT_MSG_TYPE, &type, 1);

    if (requested)
        p = put_opt(p, OPT_REQUESTED_IP, &requested, 4);
    if (server_id)
        p = put_opt(p, OPT_SERVER_ID, &server_id, 4);

    {
        static const char host[] = "stratum";

        p = put_opt(p, OPT_HOSTNAME, host, (u8)(sizeof(host) - 1));
    }

    {
        /* What the client wants to be told. A server is not obliged to send
         * anything it was not asked for, so a client that omits this list
         * and then complains about a missing gateway is complaining about
         * its own request. */
        static const u8 want[] = {OPT_SUBNET_MASK, OPT_ROUTER, OPT_DNS,
                                  OPT_LEASE_TIME};

        p = put_opt(p, OPT_PARAM_LIST, want, (u8)sizeof(want));
    }

    *p++ = OPT_END;

    /* Everything after the last option is padding. Zeroed by the memset
     * above, which is what OPT_PAD is, so there is nothing to do - noted
     * because a reader will look for it. */

    u16 total = (u16)(UDP_HDR_LEN + body);

    uh->src_port = htons(DHCP_CLIENT_PORT);
    uh->dst_port = htons(DHCP_SERVER_PORT);
    uh->length = htons(total);
    uh->checksum = 0;

    /* Source 0.0.0.0, destination 255.255.255.255 - and the checksum's
     * pseudo-header has to use those same two, not the stack's configured
     * address, or a server that verifies the checksum drops the message. */
    u32 sum = ipv4_pseudo_sum(0, 0xFFFFFFFFu, IPPROTO_UDP, total);

    uh->checksum = checksum_finish(checksum_partial(all, total, sum));
    if (uh->checksum == 0)
        uh->checksum = 0xFFFFu;

    if (!ipv4_output_broadcast_frame(&f, 0, IPPROTO_UDP))
        return false;

    net_stats()->udp_tx++;
    sent_ms = timer_ms();
    return true;
}

/* ---- parsing a reply ---------------------------------------------------- */

struct parsed {
    u8 type;
    ipv4_addr addr;
    ipv4_addr netmask;
    ipv4_addr gateway;
    ipv4_addr dns;
    ipv4_addr server;
    u32 lease_seconds;
};

/* Walk the options area. Every step is bounded by the area's length, which
 * is the whole of the security of this function: the length byte is the
 * server's, and a length that runs off the end is how a parser like this one
 * reads past its buffer. */
static bool parse(const u8 *data, u32 len, struct parsed *out)
{
    const struct dhcp_msg *m = (const struct dhcp_msg *)data;
    const struct net_device *dev = net_device();

    if (len < 240)
        return false;
    if (m->op != BOOTREPLY)
        return false;
    if (ntohl(m->magic) != DHCP_MAGIC)
        return false;
    if (ntohl(m->xid) != xid)
        return false; /* somebody else's exchange */
    if (!dev || memcmp(m->chaddr, dev->mac.b, ETH_ALEN) != 0)
        return false; /* somebody else's hardware address */

    memset(out, 0, sizeof(*out));
    out->addr = m->yiaddr;
    out->server = m->siaddr;

    u32 opt_len = len - 240;
    u32 i = 0;

    while (i < opt_len) {
        u8 code = m->options[i];

        if (code == OPT_END)
            break;
        if (code == OPT_PAD) {
            i++;
            continue;
        }

        /* A code needs a length byte after it, and the value needs that many
         * bytes after that. Both checked before either is read. */
        if (i + 2 > opt_len)
            return false;

        u8 l = m->options[i + 1];

        if (i + 2 + l > opt_len)
            return false;

        const u8 *v = &m->options[i + 2];

        switch (code) {
        case OPT_MSG_TYPE:
            if (l >= 1)
                out->type = v[0];
            break;
        case OPT_SUBNET_MASK:
            if (l == 4)
                memcpy(&out->netmask, v, 4);
            break;
        case OPT_ROUTER:
            /* A list; the first is the one this stack can use, since it has
             * one default route and no metric to choose between them. */
            if (l >= 4)
                memcpy(&out->gateway, v, 4);
            break;
        case OPT_DNS:
            if (l >= 4)
                memcpy(&out->dns, v, 4);
            break;
        case OPT_SERVER_ID:
            if (l == 4)
                memcpy(&out->server, v, 4);
            break;
        case OPT_LEASE_TIME:
            if (l == 4)
                out->lease_seconds = ntohl(*(const be32 *)v);
            break;
        default:
            break;
        }

        i += 2u + l;
    }

    return out->type != 0;
}

/* ---- the state machine -------------------------------------------------- */

static void fail(const char *why)
{
    pr_warn("%s; keeping the existing configuration", why);
    state = DHCP_FAILED;
    udp_unbind(DHCP_CLIENT_PORT);
    port_bound = false;
}

bool dhcp_start(void)
{
    if (!net_up()) {
        pr_warn("no link; not starting");
        return false;
    }

    if (!port_bound) {
        if (!udp_bind(DHCP_CLIENT_PORT)) {
            pr_warn("port %u is not available", DHCP_CLIENT_PORT);
            return false;
        }
        port_bound = true;
    }

    /* A fresh transaction identifier, which is how a reply is matched to
     * this attempt rather than to a previous one still in flight. Derived
     * from the clock and the hardware address so that two machines starting
     * at the same moment do not collide. */
    const struct net_device *dev = net_device();

    xid = (u32)timer_ms() * 2654435761u;
    if (dev)
        xid ^= (u32)dev->mac.b[5] << 24 | (u32)dev->mac.b[4] << 16 |
               (u32)dev->mac.b[3] << 8 | dev->mac.b[2];

    memset(&offer, 0, sizeof(offer));
    tries = 1;

    if (!send_msg(DHCPDISCOVER, 0, 0)) {
        fail("could not send a DISCOVER");
        return false;
    }

    lease.discovers_sent++;
    state = DHCP_DISCOVERING;
    pr_info("DISCOVER sent, xid %08x", xid);
    return true;
}

static void handle(const struct udp_dgram *d)
{
    struct parsed p;

    if (!parse(d->data, d->len, &p))
        return;

    switch (state) {
    case DHCP_DISCOVERING:
        if (p.type != DHCPOFFER)
            return;

        lease.offers_received++;

        if (!p.addr) {
            pr_warn("an OFFER with no address in it");
            return;
        }

        offer.addr = p.addr;
        offer.netmask = p.netmask;
        offer.gateway = p.gateway;
        offer.dns = p.dns;
        offer.server = p.server ? p.server : d->src;
        offer.lease_seconds = p.lease_seconds;

        {
            char ab[IPV4_STR_LEN], sb[IPV4_STR_LEN];

            pr_info("OFFER of %s from %s", ipv4_str(offer.addr, ab, sizeof(ab)),
                    ipv4_str(offer.server, sb, sizeof(sb)));
        }

        tries = 1;

        /* Broadcast, not unicast to the server that offered - the servers
         * that need to hear this are the ones whose offers are being
         * declined by omission. See the top of the file. */
        if (!send_msg(DHCPREQUEST, offer.addr, offer.server)) {
            fail("could not send a REQUEST");
            return;
        }

        lease.requests_sent++;
        state = DHCP_REQUESTING;
        return;

    case DHCP_REQUESTING:
        if (p.type == DHCPNAK) {
            lease.naks_received++;
            fail("the server refused the address it offered (NAK)");
            return;
        }

        if (p.type != DHCPACK)
            return;

        lease.acks_received++;

        /* The one place the configuration is written, and only now that
         * there is a complete answer. An ACK may differ from the OFFER, so
         * the ACK is what is believed. */
        lease.valid = true;
        lease.addr = p.addr ? p.addr : offer.addr;
        lease.netmask = p.netmask ? p.netmask : offer.netmask;
        lease.gateway = p.gateway ? p.gateway : offer.gateway;
        lease.dns = p.dns ? p.dns : offer.dns;
        lease.server = p.server ? p.server : offer.server;
        lease.lease_seconds =
            p.lease_seconds ? p.lease_seconds : offer.lease_seconds;
        lease.obtained_ms = timer_ms();

        /* A server is allowed to omit the mask. /24 is the wrong guess as
         * often as it is the right one, but a mask of zero would make
         * net_is_local() say every address is remote, which breaks ARP for
         * the gateway itself - so a guess that works on the overwhelming
         * majority of networks beats a value that cannot work on any. */
        if (!lease.netmask) {
            lease.netmask = IPV4(255, 255, 255, 0);
            pr_warn("the server sent no subnet mask; assuming /24");
        }

        net_configure(lease.addr, lease.netmask, lease.gateway);

        if (lease.dns)
            dns_set_server(lease.dns);

        state = DHCP_BOUND;
        udp_unbind(DHCP_CLIENT_PORT);
        port_bound = false;

        {
            char ab[IPV4_STR_LEN], mb[IPV4_STR_LEN], gb[IPV4_STR_LEN],
                db[IPV4_STR_LEN];

            pr_info("bound: %s mask %s gateway %s dns %s, lease %u s",
                    ipv4_str(lease.addr, ab, sizeof(ab)),
                    ipv4_str(lease.netmask, mb, sizeof(mb)),
                    ipv4_str(lease.gateway, gb, sizeof(gb)),
                    ipv4_str(lease.dns, db, sizeof(db)), lease.lease_seconds);
        }
        return;

    case DHCP_IDLE:
    case DHCP_BOUND:
    case DHCP_FAILED:
        return;
    }
}

void dhcp_tick(void)
{
    if (state != DHCP_DISCOVERING && state != DHCP_REQUESTING)
        return;

    struct udp_dgram d;

    while (udp_recv(DHCP_CLIENT_PORT, &d) > 0) {
        handle(&d);
        if (state != DHCP_DISCOVERING && state != DHCP_REQUESTING)
            return;
    }

    if (timer_ms() - sent_ms < DHCP_REPLY_MS)
        return;

    if (tries >= DHCP_MAX_TRIES) {
        lease.timeouts++;
        fail("no answer after three attempts");
        return;
    }

    tries++;
    lease.timeouts++;

    /* Resend the message for the state we are in. A datagram protocol has no
     * retransmission of its own, so this is it. */
    if (state == DHCP_DISCOVERING) {
        send_msg(DHCPDISCOVER, 0, 0);
        lease.discovers_sent++;
    } else {
        send_msg(DHCPREQUEST, offer.addr, offer.server);
        lease.requests_sent++;
    }
}
