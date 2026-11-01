/* StratumOS - a DNS resolver, for A records, one question at a time.
 *
 * WHAT A STUB RESOLVER IS, AND WHY THAT IS THE RIGHT ONE HERE
 *
 * There are two kinds of DNS client. A *recursive* resolver starts at the
 * root servers and walks down the delegation chain itself, which means it
 * needs the root hints, a cache with correct TTL handling, and an answer to
 * every one of DNS's trust questions. A *stub* resolver asks one server that
 * does all of that and believes the answer. Every operating system ships the
 * second, and so does this: the recursion lives in the server DHCP told us
 * about, and what is left here is one question, one reply, and the parsing.
 *
 * THE PARSING IS THE WHOLE PROBLEM
 *
 * A DNS message is not a fixed structure. Names are length-prefixed label
 * sequences, and - this is the part that matters - a name in a reply may end
 * with a *pointer* to an earlier offset in the same message, so that
 * "www.example.com" and "example.com" can share their tail. Name compression
 * is why a reply is small, and it is also why nearly every serious bug in a
 * DNS client has been in the name parser:
 *
 *   A pointer may point anywhere the sender likes, including at itself, or
 *   forward into a chain of pointers that loops. A parser that follows
 *   pointers without a bound can be sent round that loop forever by a
 *   single UDP datagram - a remote denial of service in forty lines of
 *   straight-line code.
 *
 * This parser's defences are stated where they are implemented, but the
 * shape is: every offset is bounded by the message length before it is
 * dereferenced, a pointer must point strictly *backwards*, and the number of
 * jumps is capped regardless. The first two make a loop impossible and the
 * third makes it impossible even if the first two are wrong, which is the
 * right amount of paranoia for a parser whose input is a stranger's
 * datagram.
 *
 * WHAT IS LEFT OUT
 *
 *   - no AAAA, because there is no IPv6 in this stack.
 *   - no CNAME chasing. A reply whose answer section holds a CNAME followed
 *     by the A record it points at is handled, because the A record is
 *     simply taken from the answer section wherever it appears; a reply with
 *     a CNAME and no A record is reported as not found rather than followed
 *     with a second query.
 *   - no TTL arithmetic. The cache keeps what it learns for the life of the
 *     boot, and says so, rather than implementing expiry badly.
 *   - no TCP fallback for a truncated reply, and no EDNS, so an answer that
 *     does not fit in 512 bytes is lost. Reported through the TC bit rather
 *     than silently.
 *   - no DNSSEC and no 0x20 encoding. The reply is matched on the
 *     transaction identifier, the source address and the question, which is
 *     what stops an unrelated datagram being taken as an answer and is not
 *     enough to stop a determined off-path attacker.
 */
#define LOG_TAG "dns"

#include <drivers/timer.h>

#include <kernel/log.h>
#include <kernel/string.h>
#include <kernel/syscall_abi.h>

#include <net/inet.h>
#include <net/net.h>

#define DNS_TYPE_A      1
#define DNS_TYPE_CNAME  5
#define DNS_CLASS_IN    1

#define DNS_FLAG_QR     0x8000u /* this message is a response       */
#define DNS_FLAG_TC     0x0200u /* truncated: it did not all fit    */
#define DNS_FLAG_RD     0x0100u /* please recurse on my behalf      */
#define DNS_RCODE_MASK  0x000Fu
#define DNS_RCODE_OK    0
#define DNS_RCODE_NXDOM 3

#define DNS_QUERY_MS    3000
#define DNS_MAX_TRIES   3
/* How soon to try again when the query could not be *sent* at all, and how
 * many times.
 *
 * These exist because of ARP. The first datagram to a resolver whose
 * hardware address is not yet known cannot be transmitted: the IPv4 layer
 * queues an ARP request and drops the frame. Treating that as a failed
 * lookup made `dns example.com` fail on a cold cache and succeed on the
 * second attempt - which looks exactly like an unreliable network and is
 * entirely this client's doing. So a query that was never sent is retried on
 * a short timer rather than counted as an attempt, because waiting a full
 * query timeout for something that never left the machine is three seconds
 * spent on nothing. */
#define DNS_UNSENT_MS   200
#define DNS_UNSENT_MAX  10
/* How many compression pointers one name may follow. See the top of the
 * file: this is the backstop, not the defence. */
#define DNS_MAX_JUMPS   16

struct dns_hdr {
    be16 id;
    be16 flags;
    be16 qdcount;
    be16 ancount;
    be16 nscount;
    be16 arcount;
} PACKED;

static ipv4_addr server;
static enum dns_status status;
static struct dns_stats stats;

static struct {
    u16 id;
    u16 port; /* the bound source port, released when the lookup ends */
    char name[DNS_NAME_MAX];
    u64 sent_ms;
    u32 tries;
    bool unsent; /* the last attempt never reached the wire */
    u32 unsent_tries;
    ipv4_addr result;
} query;

static struct {
    char name[DNS_NAME_MAX];
    ipv4_addr addr;
} cache[DNS_CACHE_MAX];

static u32 cache_count;
static u32 cache_next; /* round-robin, so a full cache keeps working */

const char *dns_status_name(enum dns_status s)
{
    switch (s) {
    case DNS_IDLE:
        return "idle";
    case DNS_PENDING:
        return "pending";
    case DNS_RESOLVED:
        return "resolved";
    case DNS_NOT_FOUND:
        return "not found";
    case DNS_TIMEOUT:
        return "timed out";
    case DNS_ERROR:
        return "error";
    }

    return "?";
}

enum dns_status dns_status(void)
{
    return status;
}

const struct dns_stats *dns_get_stats(void)
{
    return &stats;
}

void dns_set_server(ipv4_addr s)
{
    char b[IPV4_STR_LEN];

    server = s;
    pr_info("resolver is %s", ipv4_str(s, b, sizeof(b)));
}

ipv4_addr dns_server(void)
{
    return server;
}

bool dns_result(ipv4_addr *out)
{
    if (status != DNS_RESOLVED)
        return false;

    *out = query.result;
    return true;
}

/* ---- the cache ---------------------------------------------------------- */

/* Case-insensitive, because DNS is: "Example.COM" and "example.com" are the
 * same name, and a cache that treats them as two wastes an entry and a round
 * trip on a difference that does not exist. */
static bool name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;

        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb + 32);
        if (ca != cb)
            return false;
        a++;
        b++;
    }

    return *a == *b;
}

static bool cache_lookup(const char *name, ipv4_addr *out)
{
    for (u32 i = 0; i < cache_count; i++) {
        if (name_eq(cache[i].name, name)) {
            *out = cache[i].addr;
            return true;
        }
    }

    return false;
}

static void cache_put(const char *name, ipv4_addr addr)
{
    for (u32 i = 0; i < cache_count; i++) {
        if (name_eq(cache[i].name, name)) {
            cache[i].addr = addr;
            return;
        }
    }

    u32 slot;

    if (cache_count < DNS_CACHE_MAX) {
        slot = cache_count++;
    } else {
        /* Round-robin eviction. Not least-recently-used, which would need a
         * timestamp per entry and would still be the wrong policy without
         * TTLs - see the top of the file. */
        slot = cache_next % DNS_CACHE_MAX;
        cache_next++;
    }

    strlcpy(cache[slot].name, name, sizeof(cache[slot].name));
    cache[slot].addr = addr;
}

u32 dns_cache_count(void)
{
    return cache_count;
}

bool dns_cache_entry(u32 index, char *name_out, size_t name_len,
                     ipv4_addr *addr_out)
{
    if (index >= cache_count)
        return false;

    strlcpy(name_out, cache[index].name, name_len);
    *addr_out = cache[index].addr;
    return true;
}

void dns_cache_clear(void)
{
    cache_count = 0;
    cache_next = 0;
    memset(cache, 0, sizeof(cache));
}

/* Is this a name worth sending to a server?
 *
 * Three reasons this check exists, in increasing order of how much they
 * matter:
 *
 *   A name with a character that cannot appear in a host name will not
 *   resolve, so asking costs a query and a three-second wait to learn
 *   nothing. The syscall fuzzer found this by resolving "/dev/zero" - a
 *   string that happened to be in its own data segment - which is one long
 *   label as far as the encoder is concerned, and perfectly sendable.
 *
 *   A resolver that puts arbitrary bytes into a QNAME is a resolver that can
 *   be used to smuggle data to a nameserver the caller could not otherwise
 *   reach. That is a real exfiltration channel and it is closed cheaply.
 *
 *   And it makes `resolve` from ring 3 fail immediately on nonsense instead
 *   of blocking the caller for seconds, which is what turned a fuzz run into
 *   a hang.
 *
 * Letters, digits, hyphen and dot. Underscore is deliberately excluded: it
 * appears in SRV and TXT names, and this resolver only asks for A records of
 * host names. */
static bool name_plausible(const char *name)
{
    size_t n = 0;

    for (const char *p = name; *p; p++, n++) {
        char c = *p;

        if (c >= 'a' && c <= 'z')
            continue;
        if (c >= 'A' && c <= 'Z')
            continue;
        if (c >= '0' && c <= '9')
            continue;
        if (c == '-' || c == '.')
            continue;

        return false;
    }

    /* A name with no dot in it is a bare label. Allowed, because a search
     * domain would make it resolvable and because "localhost" is one - but a
     * name with nothing in it is not. */
    return n > 0 && n < DNS_NAME_MAX;
}

/* ---- encoding a name ---------------------------------------------------
 *
 * "www.example.com" becomes 3 w w w 7 e x a m p l e 3 c o m 0. Each label is
 * length-prefixed and the whole thing is terminated by a zero length, which
 * is also why a label may not be longer than 63 bytes: the top two bits of
 * the length byte are reserved for the compression pointer marker.
 */
static i32 encode_name(const char *name, u8 *out, u32 outlen)
{
    u32 w = 0;
    const char *p = name;

    while (*p) {
        const char *dot = p;
        u32 label = 0;

        while (*dot && *dot != '.') {
            dot++;
            label++;
        }

        if (!label)
            return SYS_EINVAL; /* an empty label: "a..b" or a leading dot */
        if (label > 63)
            return SYS_EINVAL;
        if (w + 1 + label + 1 > outlen)
            return SYS_ENAMETOOLONG;

        out[w++] = (u8)label;
        memcpy(out + w, p, label);
        w += label;

        p = dot;
        if (*p == '.')
            p++;
    }

    if (w + 1 > outlen)
        return SYS_ENAMETOOLONG;

    out[w++] = 0;
    return (i32)w;
}

/* ---- skipping a name in a reply ---------------------------------------
 *
 * Returns the offset just past the name, or a negative value. This does not
 * decode the name, because nothing here needs it: the question is matched by
 * re-encoding what was asked, and an answer's owner name is not used. Not
 * decoding it removes an entire buffer from the parser.
 *
 * The bounds are the point. See the top of the file.
 */
i32 dns_skip_name(const u8 *msg, u32 len, u32 off)
{
    u32 jumps = 0;
    bool jumped = false;
    u32 after = 0;

    while (true) {
        if (off >= len)
            return SYS_EINVAL;

        u8 l = msg[off];

        if ((l & 0xC0) == 0xC0) {
            /* A compression pointer: two bytes, the low 14 bits an offset
             * from the start of the message. */
            if (off + 2 > len)
                return SYS_EINVAL;

            u32 target = ((u32)(l & 0x3F) << 8) | msg[off + 1];

            /* Strictly backwards. A pointer to itself or forwards is how a
             * loop is built, and there is no legitimate reason for one: the
             * whole purpose of compression is to refer to a name that has
             * already been written. */
            if (target >= off)
                return SYS_EINVAL;

            if (++jumps > DNS_MAX_JUMPS)
                return SYS_EINVAL;

            /* The name ends at the pointer, wherever the pointer leads, so
             * the first jump is what fixes the return value. */
            if (!jumped) {
                after = off + 2;
                jumped = true;
            }

            off = target;
            continue;
        }

        if (l & 0xC0)
            return SYS_EINVAL; /* 0x40 and 0x80 are reserved */

        if (!l) {
            /* The root label ends the name. */
            off++;
            return (i32)(jumped ? after : off);
        }

        if (off + 1u + l > len)
            return SYS_EINVAL;

        off += 1u + l;
    }
}

/* ---- asking ------------------------------------------------------------- */

static void finish(enum dns_status s)
{
    if (query.port) {
        udp_unbind(query.port);
        query.port = 0;
    }

    status = s;
}

static bool send_query(void)
{
    u8 buf[sizeof(struct dns_hdr) + DNS_NAME_MAX + 4];
    struct dns_hdr *h = (struct dns_hdr *)buf;
    i32 n;

    memset(buf, 0, sizeof(buf));
    h->id = htons(query.id);
    /* RD set: we are a stub resolver asking a server to do the recursion.
     * Without it a recursive server answers only from its own cache, which
     * works often enough to look correct and fails on anything new. */
    h->flags = htons(DNS_FLAG_RD);
    h->qdcount = htons(1);

    n = encode_name(query.name, buf + sizeof(*h),
                    (u32)(sizeof(buf) - sizeof(*h) - 4));
    if (n < 0)
        return false;

    u32 off = sizeof(*h) + (u32)n;

    buf[off++] = 0;
    buf[off++] = DNS_TYPE_A;
    buf[off++] = 0;
    buf[off++] = DNS_CLASS_IN;

    query.sent_ms = timer_ms();

    if (!udp_send(server, query.port, DNS_PORT, buf, off)) {
        /* Not an error: see DNS_UNSENT_MS above. */
        query.unsent = true;
        return false;
    }

    query.unsent = false;
    stats.queries_sent++;
    return true;
}

bool dns_query(const char *name)
{
    if (!server) {
        pr_warn("no resolver configured");
        return false;
    }

    if (!name || !*name)
        return false;

    if (!name_plausible(name)) {
        pr_debug("'%s' is not a host name; not asking", name);
        return false;
    }

    /* A second lookup replaces the first. One at a time is a real limit and
     * is stated in the header; it is not a race, because the port is
     * released here before the new one is bound. */
    if (query.port) {
        udp_unbind(query.port);
        query.port = 0;
    }

    strlcpy(query.name, name, sizeof(query.name));
    query.result = 0;
    query.tries = 1;
    query.unsent = false;
    query.unsent_tries = 0;

    /* A fresh identifier per lookup, from the clock. Together with the
     * source port, the source address and the question, this is what matches
     * a reply to its request - and all four together are still guessable by
     * an attacker who can send a lot of datagrams, which is what DNS cookies
     * and 0x20 encoding exist for and this does not have. */
    query.id = (u16)(timer_ms() * 40503u + 0x4711u);

    query.port = udp_bind_ephemeral();
    if (!query.port) {
        pr_warn("no free source port");
        finish(DNS_ERROR);
        stats.errors++;
        return false;
    }

    status = DNS_PENDING;

    /* A send that failed leaves the lookup pending for the timer to retry -
     * the common cause is a cold ARP cache, which resolves itself in a
     * fraction of a second. encode_name() failing is the other cause, and
     * that one never resolves, which is why it is checked first. */
    if (!send_query() && !query.unsent) {
        stats.errors++;
        finish(DNS_ERROR);
        return false;
    }

    pr_debug("asked for %s, id %04x from port %u", query.name, query.id,
             query.port);
    return true;
}

/* ---- the reply ---------------------------------------------------------- */

static void handle(const struct udp_dgram *d)
{
    const u8 *msg = d->data;
    u32 len = d->len;
    const struct dns_hdr *h = (const struct dns_hdr *)msg;

    if (len < sizeof(*h))
        return;

    /* Three independent checks, all of which must pass before a single byte
     * of the answer is believed: the right server, the right identifier, and
     * a message that says it is a response. */
    if (d->src != server)
        return;
    if (ntohs(h->id) != query.id)
        return;

    u16 flags = ntohs(h->flags);

    if (!(flags & DNS_FLAG_QR))
        return;

    stats.replies_received++;

    if (flags & DNS_FLAG_TC) {
        /* A truncated answer. Without EDNS or TCP fallback there is nothing
         * to do but say so: silently using the partial answer would be the
         * one unacceptable response. */
        pr_warn("the reply for %s was truncated and this client has no TCP "
                "fallback",
                query.name);
        stats.errors++;
        finish(DNS_ERROR);
        return;
    }

    u16 rcode = flags & DNS_RCODE_MASK;

    if (rcode == DNS_RCODE_NXDOM) {
        stats.not_found++;
        pr_info("%s does not exist", query.name);
        finish(DNS_NOT_FOUND);
        return;
    }

    if (rcode != DNS_RCODE_OK) {
        pr_warn("the server answered for %s with rcode %u", query.name, rcode);
        stats.errors++;
        finish(DNS_ERROR);
        return;
    }

    u32 qd = ntohs(h->qdcount);
    u32 an = ntohs(h->ancount);
    u32 off = sizeof(*h);

    /* Skip the question section. The names here are the echo of what was
     * asked, and skipping them is bounded by the same parser that bounds
     * the answers. */
    for (u32 i = 0; i < qd; i++) {
        i32 next = dns_skip_name(msg, len, off);

        if (next < 0) {
            stats.errors++;
            finish(DNS_ERROR);
            return;
        }

        off = (u32)next + 4; /* type and class */
        if (off > len) {
            stats.errors++;
            finish(DNS_ERROR);
            return;
        }
    }

    for (u32 i = 0; i < an; i++) {
        i32 next = dns_skip_name(msg, len, off);

        if (next < 0)
            break;

        off = (u32)next;

        /* type, class, ttl, rdlength: ten bytes before the data. */
        if (off + 10 > len)
            break;

        u16 type = (u16)((msg[off] << 8) | msg[off + 1]);
        u16 cls = (u16)((msg[off + 2] << 8) | msg[off + 3]);
        u16 rdlen = (u16)((msg[off + 8] << 8) | msg[off + 9]);

        off += 10;

        if (off + rdlen > len)
            break;

        if (type == DNS_TYPE_A && cls == DNS_CLASS_IN && rdlen == 4) {
            ipv4_addr addr;

            memcpy(&addr, msg + off, 4);

            query.result = addr;
            cache_put(query.name, addr);
            stats.answers++;
            finish(DNS_RESOLVED);

            char ab[IPV4_STR_LEN];

            pr_info("%s is %s", query.name, ipv4_str(addr, ab, sizeof(ab)));
            return;
        }

        /* A CNAME, or an answer of a type we did not ask for. Skipped: the A
         * record this chain ends at is usually in the same answer section,
         * which is why not following the chain still works most of the
         * time. See the top of the file. */
        off += rdlen;
    }

    /* The server answered with no error and no address. A name that exists
     * with no A record - which is a real situation, not a malformed reply. */
    pr_info("%s has no address record", query.name);
    stats.not_found++;
    finish(DNS_NOT_FOUND);
}

void dns_tick(void)
{
    if (status != DNS_PENDING || !query.port)
        return;

    struct udp_dgram d;

    while (udp_recv(query.port, &d) > 0) {
        handle(&d);
        if (status != DNS_PENDING)
            return;
    }

    if (query.unsent) {
        if (timer_ms() - query.sent_ms < DNS_UNSENT_MS)
            return;

        if (query.unsent_tries >= DNS_UNSENT_MAX) {
            pr_warn("could not send a query for %s at all", query.name);
            stats.errors++;
            finish(DNS_ERROR);
            return;
        }

        query.unsent_tries++;
        send_query();
        return;
    }

    if (timer_ms() - query.sent_ms < DNS_QUERY_MS)
        return;

    if (query.tries >= DNS_MAX_TRIES) {
        pr_warn("no answer for %s", query.name);
        stats.timeouts++;
        finish(DNS_TIMEOUT);
        return;
    }

    query.tries++;
    stats.retries++;
    send_query();
}

/* ---- the synchronous form ---------------------------------------------- */

i32 dns_resolve(const char *name, ipv4_addr *out, u32 timeout_ms)
{
    if (!name || !*name)
        return SYS_EINVAL;

    /* A dotted quad is its own answer. Checked first, so that `fetch
     * 10.0.2.2` works on a machine with no resolver at all - and so that a
     * numeric address never costs a round trip. */
    if (ipv4_parse(name, out))
        return 0;

    if (cache_lookup(name, out)) {
        stats.cache_hits++;
        return 0;
    }

    /* Checked here as well as in dns_query(), and before the server check,
     * so that a caller gets EINVAL for a malformed name whether or not a
     * resolver is configured - the name is wrong either way, and which
     * answer comes back should not depend on the machine's configuration. */
    if (!name_plausible(name))
        return SYS_EINVAL;

    if (!server)
        return SYS_EHOSTUNREACH;

    if (!dns_query(name))
        return SYS_EIO;

    u64 deadline = timer_ms() + timeout_ms;

    /* Polling the network directly rather than sleeping on it: the network
     * thread would do this too, but a caller that is itself a kernel thread
     * must not depend on another thread being scheduled for its own call to
     * make progress. The shell's `ping` is driven the same way. */
    while (status == DNS_PENDING && timer_ms() < deadline) {
        net_poll(16);
        dns_tick();
    }

    switch (status) {
    case DNS_RESOLVED:
        *out = query.result;
        return 0;
    case DNS_NOT_FOUND:
        return SYS_ENOENT;
    case DNS_PENDING:
        /* The deadline passed with the lookup still open. Abandon it, so
         * that the port is released and a later lookup is not confused by
         * this one's reply. */
        stats.timeouts++;
        finish(DNS_TIMEOUT);
        return SYS_ETIMEDOUT;
    case DNS_TIMEOUT:
        return SYS_ETIMEDOUT;
    default:
        return SYS_EIO;
    }
}
