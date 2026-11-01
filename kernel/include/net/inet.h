/* StratumOS - the three protocols that turn a stack into a connection.
 *
 * WHY THESE THREE, AND WHY TOGETHER
 *
 * The kernel below this header could already put a correct TCP segment on a
 * wire. It still could not reach anything, and the reason is worth stating
 * exactly, because it is not a networking problem:
 *
 *   It had no address of its own except the one compiled into it.
 *   It had no way to turn a name into an address.
 *   It had nothing to say once a connection was open.
 *
 * Those are DHCP, DNS and HTTP, and they are the whole of the distance
 * between "the stack works" and "the machine is on the internet". None of
 * the three is hard; what makes them worth having is that each one is the
 * first user of something underneath it that had never been exercised:
 *
 *   DHCP is the first thing to send a broadcast datagram from an address it
 *   does not have yet, which is the one case where the IP source address is
 *   0.0.0.0 and the destination is 255.255.255.255 - and the first thing to
 *   find out whether the stack handles that at all.
 *
 *   DNS is the first request/reply protocol over UDP, so it is the first
 *   user of a bound port with a queue, and the first thing that has to match
 *   a reply to a request rather than assuming the next datagram is the
 *   answer.
 *
 *   HTTP is the first user of an outgoing TCP connection, which means it is
 *   the first thing to exercise the active open, the congestion window on a
 *   path with real delay, and a stream that ends because the peer closed it
 *   rather than because a length said so.
 *
 * WHAT THIS IS NOT
 *
 * There is no TLS, so this speaks http:// and not https://. That is not a
 * missing function: TLS needs a certificate store, a chain validator, RSA or
 * ECDSA, AES-GCM and a hash, and every one of those is a correctness problem
 * where being approximately right is the same as being broken. A kernel that
 * shipped a half-checked certificate validator would be worse than one that
 * says plainly it cannot do HTTPS.
 *
 * And this is not a browser. A browser is HTML parsing, CSS layout, a
 * JavaScript engine, font rasterisation and a compositor; the fetching is
 * the part that fits in a file this size.
 */
#ifndef _NET_INET_H
#define _NET_INET_H

#include <kernel/types.h>

#include <net/net.h>

/* ---- DHCP --------------------------------------------------------------
 *
 * The four-message exchange, as a client: DISCOVER, then OFFER from a
 * server, then REQUEST naming the offer, then ACK. It is four and not two
 * because of the case where several servers answer: the client has to say
 * which offer it took, so that the other servers can release what they had
 * tentatively set aside.
 *
 * Driven by polling from the network thread rather than by a callback, which
 * keeps the state machine readable in one function and matches how the rest
 * of this stack is driven.
 */
enum dhcp_state {
    DHCP_IDLE = 0,
    DHCP_DISCOVERING, /* a DISCOVER is out, waiting for an OFFER */
    DHCP_REQUESTING,  /* an offer was taken, waiting for the ACK */
    DHCP_BOUND,
    DHCP_FAILED,
};

const char *dhcp_state_name(enum dhcp_state s);

/* Start the exchange. Returns false if there is no link to send on.
 *
 * The address the kernel already has is left alone until an ACK arrives, so
 * a DHCP attempt that fails leaves a statically configured machine exactly
 * as it was. That matters here more than it would elsewhere: this kernel's
 * own test suite runs against a static address, and a DHCP client that
 * cleared the address before it had a replacement would break every
 * scenario whenever the server was slow. */
bool dhcp_start(void);
void dhcp_tick(void);
enum dhcp_state dhcp_state(void);

struct dhcp_lease {
    bool valid;
    ipv4_addr addr;
    ipv4_addr netmask;
    ipv4_addr gateway;
    ipv4_addr dns;
    ipv4_addr server; /* which server granted it */
    u32 lease_seconds;
    u64 obtained_ms;
    u32 discovers_sent, offers_received, requests_sent, acks_received;
    u32 naks_received, timeouts;
};

const struct dhcp_lease *dhcp_get_lease(void);

/* ---- DNS ---------------------------------------------------------------
 *
 * A resolver for one question at a time: an A record for a name, asked of
 * one server, over UDP. No recursion of its own - it asks a server that
 * recurses, which is what every client does - no cache eviction policy
 * beyond a fixed table, and no AAAA because there is no IPv6 here.
 *
 * The part that is easy to get wrong and is therefore spelled out in dns.c
 * is name compression: a name in a *reply* may end in a pointer back into
 * the message, so a parser that walks names without a bound and without
 * rejecting a pointer that does not go backwards can be sent in a circle by
 * a malicious server. That is a remote denial of service in a parser, which
 * is the single most common class of bug in a DNS client.
 */
#define DNS_PORT      53
#define DNS_NAME_MAX  128
#define DNS_CACHE_MAX 8

/* Start a lookup. Returns true if the query went out. One lookup is in
 * flight at a time; starting a second replaces the first. */
bool dns_query(const char *name);

enum dns_status {
    DNS_IDLE = 0,
    DNS_PENDING,
    DNS_RESOLVED,
    DNS_NOT_FOUND, /* the server answered, and the name does not exist */
    DNS_TIMEOUT,
    DNS_ERROR, /* a malformed or unexpected reply                     */
};

const char *dns_status_name(enum dns_status s);
enum dns_status dns_status(void);
/* The answer to the last completed lookup. */
bool dns_result(ipv4_addr *out);
void dns_tick(void);

/* The resolver's server, which DHCP sets and the shell can override. */
void dns_set_server(ipv4_addr server);
ipv4_addr dns_server(void);

/* The synchronous form, for the shell and for the HTTP client: runs the
 * network poll loop until the lookup completes or the deadline passes.
 * Returns a negative SYS_E* value on failure.
 *
 * A dotted-quad name resolves to itself without a query, which is what makes
 * `fetch 10.0.2.2` work on a machine with no DNS server at all. */
i32 dns_resolve(const char *name, ipv4_addr *out, u32 timeout_ms);

/* A small cache, so that two fetches of the same host ask once. Bounded and
 * overwritten round-robin: a cache with an eviction policy would need the
 * TTL arithmetic to be right, and this is honest about keeping entries for
 * the life of the boot instead. */
u32 dns_cache_count(void);
bool dns_cache_entry(u32 index, char *name_out, size_t name_len,
                     ipv4_addr *addr_out);
void dns_cache_clear(void);

/* Step over a name at `off`, returning the offset just past it or a
 * negative value.
 *
 * Exposed, and the only part of the parser that is, because this is where a
 * DNS client gets attacked. A compression pointer may point anywhere the
 * sender chooses, so a parser that follows one without bounding the offset,
 * requiring the pointer to go backwards, and capping the number of jumps can
 * be sent round a loop forever by one datagram. Three defences for one
 * hazard looks like too many until you notice that each covers a different
 * mistake, and a function like this earns direct tests rather than being
 * exercised only through whatever replies a live server happens to send. */
i32 dns_skip_name(const u8 *msg, u32 len, u32 off);

struct dns_stats {
    u32 queries_sent, replies_received, answers, not_found, timeouts, errors;
    u32 cache_hits, retries;
};

const struct dns_stats *dns_get_stats(void);

/* ---- HTTP --------------------------------------------------------------
 *
 * A client that does one GET and reads the response into a buffer the caller
 * supplies. Synchronous, because the only callers are the shell and the test
 * suite and neither wants a callback; it polls the network while it waits,
 * which is what the rest of this kernel does to wait on the network.
 */
#define HTTP_URL_MAX  256
#define HTTP_HOST_MAX 64

struct http_response {
    int status; /* 200, 404, ...; 0 if the status line was unparseable */
    u32 header_bytes;
    u32 body_bytes;
    u32 total_bytes; /* what arrived, which may exceed the buffer      */
    bool truncated;
    bool chunked; /* the server used chunked transfer encoding         */
    char reason[48];
    char content_type[64];
};

/* Fetch `url` into `buf`. Returns the number of body bytes stored, or a
 * negative SYS_E* value. `url` may omit the scheme and may name a host by
 * name or by dotted quad. */
i32 http_get(const char *url, void *buf, u32 buflen, struct http_response *resp,
             u32 timeout_ms);

/* Parse a URL into its parts, exposed because it is worth testing on its own
 * and because the shell wants to print what it understood before it goes to
 * the network. Returns false on a URL this client cannot use - which
 * includes https://, deliberately and with a message. */
bool http_parse_url(const char *url, char *host, size_t host_len, u16 *port,
                    char *path, size_t path_len, bool *tls_wanted);

struct http_stats {
    u32 requests, responses, failures, bytes_received;
};

const struct http_stats *http_get_stats(void);

#endif /* _NET_INET_H */
