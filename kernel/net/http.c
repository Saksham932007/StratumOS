/* StratumOS - an HTTP/1.1 client that does one GET.
 *
 * WHY THIS FILE IS WORTH HAVING IN A KERNEL
 *
 * It is not, really - a fetcher belongs in user space, and will move there
 * once there is a user-space program worth putting it in. It is here because
 * it is the first thing in this kernel that uses TCP the way an application
 * does, and that turned out to matter: every one of the following was found
 * by writing this file and not by writing TCP.
 *
 *   An outgoing connection is not established when connect() returns. The
 *   shell's `fetch` had to learn to wait, which is what made TCP_SYN_SENT a
 *   state with something in it.
 *
 *   A response has no length in it, in general. HTTP/1.1 ends a response
 *   either at Content-Length, or at the end of a chunked body, or - and this
 *   is the case everything else ignores - when the server closes the
 *   connection. The third one is why `tcp_recv_eof()` has to be
 *   distinguishable from "nothing to read right now", and getting that wrong
 *   gives you a client that hangs on exactly the servers that work.
 *
 *   Reading until the peer closes means the *receive* path has to drain
 *   after the FIN. A connection in CLOSE_WAIT still has bytes in its
 *   buffer, and a client that treats the FIN as the end of the data loses
 *   the tail of every response.
 *
 * CHUNKED TRANSFER ENCODING
 *
 * The one piece of real parsing. A chunked body is a sequence of
 * hexadecimal lengths, each followed by that many bytes and a CRLF, ending
 * with a zero length. It exists because a server generating a response
 * dynamically does not know its length in advance and the alternative is
 * closing the connection to signal the end - which costs a connection per
 * response.
 *
 * It is decoded here rather than handed to the caller raw, because a caller
 * that has to know about chunk framing is a caller that has to implement
 * this again. The decoder is a four-state machine and the states are named
 * below.
 *
 * WHAT THIS DOES NOT DO
 *
 *   - no HTTPS. See net/inet.h: TLS is a certificate store and four
 *     primitives, and a half-checked validator is worse than an honest
 *     refusal. A URL with the https scheme is rejected with a message
 *     saying so, rather than quietly tried on port 443 where it would
 *     produce a binary handshake and an unreadable error.
 *   - no redirects. A 301 or 302 is returned to the caller as a status with
 *     its Location header unparsed, because following redirects needs a loop
 *     bound, a same-origin decision and a cookie policy to be wrong about.
 *   - no keep-alive. Connection: close is sent, one request per connection.
 *     Keep-alive without a connection pool saves nothing here.
 *   - no compression, no authentication, no cookies, no POST.
 *   - headers are read into a fixed buffer and a response with more header
 *     bytes than that is an error rather than a truncation, because a
 *     truncated header set means a missing Content-Length and silently
 *     wrong framing.
 */
#define LOG_TAG "http"

#include <drivers/timer.h>

#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/string.h>
#include <kernel/syscall_abi.h>

#include <net/inet.h>
#include <net/net.h>
#include <net/netlock.h>

#define HTTP_HDR_MAX   2048
#define HTTP_CHUNK_MAX 1024

static struct http_stats stats;

static const struct http_stats *http_get_stats_inner(void)
{
    return &stats;
}

/* ---- the URL ------------------------------------------------------------ */

bool http_parse_url(const char *url, char *host, size_t host_len, u16 *port,
                    char *path, size_t path_len, bool *tls_wanted)
{
    const char *p = url;

    *tls_wanted = false;
    *port = 80;

    if (!url || !*url)
        return false;

    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        /* Reported rather than refused silently, and reported as what it is:
         * the scheme is understood and the transport is not. */
        *tls_wanted = true;
        return false;
    } else if (strchr(p, ':') && strncmp(p, "//", 2) != 0) {
        /* Something like "ftp://" or "file:". A scheme this client does not
         * speak, as distinct from a bare host with a port, which is handled
         * below - the difference is that a port is digits. */
        const char *colon = strchr(p, ':');

        if (colon[1] == '/' && colon[2] == '/')
            return false;
    }

    /* The authority ends at the first '/', '?' or end of string. */
    const char *end = p;

    while (*end && *end != '/' && *end != '?')
        end++;

    const char *colon = NULL;

    for (const char *q = p; q < end; q++)
        if (*q == ':')
            colon = q;

    const char *host_end = colon ? colon : end;
    size_t n = (size_t)(host_end - p);

    if (!n || n >= host_len)
        return false;

    memcpy(host, p, n);
    host[n] = '\0';

    if (colon) {
        u32 v = 0;
        const char *q = colon + 1;

        if (q == end)
            return false;

        for (; q < end; q++) {
            if (*q < '0' || *q > '9')
                return false;
            v = v * 10u + (u32)(*q - '0');
            if (v > 65535)
                return false;
        }

        if (!v)
            return false;

        *port = (u16)v;
    }

    /* An empty path is "/" - which is not a detail: a request line with an
     * empty target is malformed and a server answers it with a 400. */
    if (!*end) {
        if (path_len < 2)
            return false;
        path[0] = '/';
        path[1] = '\0';
        return true;
    }

    if (strlen(end) >= path_len)
        return false;

    strlcpy(path, end, path_len);
    return true;
}

/* ---- the response ------------------------------------------------------- */

/* "HTTP/1.1 200 OK" -> 200, "OK". A status line that does not parse is
 * reported as status 0 rather than guessed at, because a caller that acts on
 * a made-up 200 is worse off than one that knows it got nonsense. */
static void parse_status(const char *line, struct http_response *r)
{
    r->status = 0;
    r->reason[0] = '\0';

    if (strncmp(line, "HTTP/", 5) != 0)
        return;

    const char *sp = strchr(line, ' ');

    if (!sp)
        return;

    sp++;

    int code = 0;
    int digits = 0;

    while (*sp >= '0' && *sp <= '9') {
        code = code * 10 + (*sp - '0');
        sp++;
        digits++;
    }

    if (digits != 3)
        return;

    r->status = code;

    if (*sp == ' ')
        sp++;

    strlcpy(r->reason, sp, sizeof(r->reason));
}

static bool header_is(const char *line, const char *name)
{
    size_t n = strlen(name);

    /* Field names are case-insensitive, which is not a nicety: servers
     * genuinely send "content-length" and "Content-Length" and a client that
     * matches one of them works on half the web. */
    for (size_t i = 0; i < n; i++) {
        char a = line[i], b = name[i];

        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + 32);
        if (!a || a != b)
            return false;
    }

    return line[n] == ':';
}

static const char *header_value(const char *line, const char *name)
{
    const char *v = line + strlen(name) + 1;

    while (*v == ' ' || *v == '\t')
        v++;

    return v;
}

/* ---- the chunked-body decoder ------------------------------------------ */

enum chunk_state {
    CHUNK_SIZE = 0, /* reading the hexadecimal length    */
    CHUNK_DATA,     /* copying that many bytes            */
    CHUNK_CRLF,     /* the CRLF that follows the data     */
    CHUNK_DONE,     /* a zero-length chunk was seen       */
};

struct body_sink {
    u8 *buf;
    u32 cap;
    u32 len;
    bool truncated;

    bool chunked;
    enum chunk_state cs;
    u32 remaining; /* bytes left in the current chunk  */
    u32 size_digits;
    u32 size_acc;
};

static void sink_raw(struct body_sink *s, const u8 *data, u32 len)
{
    u32 room = s->cap > s->len ? s->cap - s->len : 0;
    u32 take = len < room ? len : room;

    if (take)
        memcpy(s->buf + s->len, data, take);

    s->len += take;

    if (take < len)
        s->truncated = true;
}

static void sink(struct body_sink *s, const u8 *data, u32 len)
{
    if (!s->chunked) {
        sink_raw(s, data, len);
        return;
    }

    u32 i = 0;

    while (i < len && s->cs != CHUNK_DONE) {
        switch (s->cs) {
        case CHUNK_SIZE: {
            u8 c = data[i++];

            if (c == '\r')
                break; /* ignored; the '\n' ends the line */

            if (c == '\n') {
                if (!s->size_digits) {
                    /* A line with no hexadecimal digits where a length was
                     * expected. This is also the CRLF *after* a chunk's data
                     * when the decoder has already consumed it, so it is
                     * skipped rather than treated as an error - which is the
                     * one place this state machine is forgiving, and it is
                     * forgiving because both readings are correct. */
                    break;
                }

                s->remaining = s->size_acc;
                s->size_acc = 0;
                s->size_digits = 0;
                s->cs = s->remaining ? CHUNK_DATA : CHUNK_DONE;
                break;
            }

            /* A chunk-size line may carry extensions after a ';'. Nothing
             * uses them, and the bytes up to the newline are skipped by
             * simply not being digits. */
            u32 d;

            if (c >= '0' && c <= '9')
                d = (u32)(c - '0');
            else if (c >= 'a' && c <= 'f')
                d = (u32)(c - 'a') + 10u;
            else if (c >= 'A' && c <= 'F')
                d = (u32)(c - 'A') + 10u;
            else
                break;

            /* A length a caller could never hold is a length to refuse
             * rather than overflow on. Eight digits is 4 GiB. */
            if (s->size_digits < 8) {
                s->size_acc = s->size_acc * 16u + d;
                s->size_digits++;
            }
            break;
        }

        case CHUNK_DATA: {
            u32 avail = len - i;
            u32 take = avail < s->remaining ? avail : s->remaining;

            sink_raw(s, data + i, take);
            i += take;
            s->remaining -= take;

            if (!s->remaining)
                s->cs = CHUNK_CRLF;
            break;
        }

        case CHUNK_CRLF: {
            u8 c = data[i++];

            if (c == '\n')
                s->cs = CHUNK_SIZE;
            break;
        }

        case CHUNK_DONE:
            break;
        }
    }
}

/* ---- the request -------------------------------------------------------- */

/* Not wrapped in the network lock. This polls the stack and watches the
 * clock, and the clock is a timer interrupt: holding a lock that masks
 * interrupts across a wait on timer_ms() means the deadline never arrives.
 * It needs no lock of its own either - every call it makes into the stack
 * takes one. */
i32 http_get(const char *url, void *buf, u32 buflen, struct http_response *resp,
             u32 timeout_ms)
{
    char host[HTTP_HOST_MAX];
    char path[HTTP_URL_MAX];
    u16 port;
    bool tls;

    memset(resp, 0, sizeof(*resp));

    if (!http_parse_url(url, host, sizeof(host), &port, path, sizeof(path),
                        &tls)) {
        if (tls)
            pr_warn("%s is https, and this client has no TLS - see "
                    "net/inet.h",
                    url);
        else
            pr_warn("could not parse '%s' as a URL this client can use", url);
        return SYS_EINVAL;
    }

    ipv4_addr addr;
    i32 rc = dns_resolve(host, &addr, timeout_ms / 2);

    if (rc < 0) {
        pr_warn("could not resolve %s", host);
        stats.failures++;
        return rc;
    }

    stats.requests++;

    int h = tcp_connect(addr, port, 0);

    if (h < 0) {
        char ab[IPV4_STR_LEN];

        pr_warn("could not open a connection to %s:%u",
                ipv4_str(addr, ab, sizeof(ab)), port);
        stats.failures++;
        return h;
    }

    u64 deadline = timer_ms() + timeout_ms;

    /* Wait for the handshake. connect() is non-blocking, which is the honest
     * shape for a stack whose timers run in a poll loop - and it means this
     * wait is the client's job. */
    while (tcp_handle_state(h) == TCP_SYN_SENT && timer_ms() < deadline) {
        net_poll(16);
    }

    if (tcp_handle_state(h) != TCP_ESTABLISHED) {
        enum tcp_state st = tcp_handle_state(h);

        pr_warn("the connection did not establish (%s)", tcp_state_name(st));
        tcp_abort(h);
        stats.failures++;
        /* CLOSED after SYN_SENT means a reset came back, which is a refusal;
         * still SYN_SENT means nothing answered at all. The two are worth
         * telling apart, because the first says the host is there. */
        return st == TCP_CLOSED ? SYS_ECONNREFUSED : SYS_ETIMEDOUT;
    }

    /* The request. One GET, HTTP/1.1 with a Host header - which 1.1 requires
     * and which is the whole reason virtual hosting works - and
     * Connection: close, because there is no connection pool for keep-alive
     * to reuse. */
    {
        char req[HTTP_URL_MAX + HTTP_HOST_MAX + 128];
        int n = 0;

        n = ksnprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "User-Agent: StratumOS/1.0\r\n"
                      "Accept: */*\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      path, host);

        if (n <= 0) {
            tcp_abort(h);
            stats.failures++;
            return SYS_EINVAL;
        }

        u32 sent = 0;

        while (sent < (u32)n && timer_ms() < deadline) {
            i32 w = tcp_send(h, req + sent, (u32)n - sent);

            if (w > 0) {
                sent += (u32)w;
                continue;
            }

            if (w == SYS_EAGAIN) {
                net_poll(16);
                continue;
            }

            tcp_abort(h);
            stats.failures++;
            return w;
        }

        if (sent < (u32)n) {
            tcp_abort(h);
            stats.failures++;
            return SYS_ETIMEDOUT;
        }
    }

    /* ---- the reply ---------------------------------------------------- */

    static char hdr[HTTP_HDR_MAX];
    u32 hdr_len = 0;
    bool headers_done = false;
    u32 content_length = 0;
    bool have_length = false;

    struct body_sink bs;

    memset(&bs, 0, sizeof(bs));
    bs.buf = buf;
    bs.cap = buflen;

    while (timer_ms() < deadline) {
        u8 tmp[HTTP_CHUNK_MAX];
        i32 got = tcp_recv(h, tmp, sizeof(tmp));

        if (got > 0) {
            stats.bytes_received += (u32)got;
            resp->total_bytes += (u32)got;

            u32 off = 0;

            if (!headers_done) {
                /* Copy into the header buffer a byte at a time while looking
                 * for the blank line. A byte at a time because the
                 * terminator can straddle two reads, and the alternative -
                 * searching the concatenation each time - is the same work
                 * with a bug in it. */
                while (off < (u32)got && !headers_done) {
                    if (hdr_len + 1 >= sizeof(hdr)) {
                        pr_warn("the response headers exceed %u bytes",
                                (unsigned)sizeof(hdr));
                        tcp_abort(h);
                        stats.failures++;
                        return SYS_ENOSPC;
                    }

                    hdr[hdr_len++] = (char)tmp[off++];

                    if (hdr_len >= 4 && hdr[hdr_len - 4] == '\r' &&
                        hdr[hdr_len - 3] == '\n' && hdr[hdr_len - 2] == '\r' &&
                        hdr[hdr_len - 1] == '\n')
                        headers_done = true;
                    /* A server that sends bare newlines rather than CRLF is
                     * not conformant and is common enough to accommodate. */
                    else if (hdr_len >= 2 && hdr[hdr_len - 2] == '\n' &&
                             hdr[hdr_len - 1] == '\n')
                        headers_done = true;
                }

                if (headers_done) {
                    hdr[hdr_len] = '\0';
                    resp->header_bytes = hdr_len;

                    /* Split on line endings in place and walk the lines. */
                    char *line = hdr;
                    bool first = true;

                    while (line && *line) {
                        char *nl = strchr(line, '\n');

                        if (nl)
                            *nl = '\0';

                        size_t ll = strlen(line);

                        if (ll && line[ll - 1] == '\r')
                            line[ll - 1] = '\0';

                        if (first) {
                            parse_status(line, resp);
                            first = false;
                        } else if (header_is(line, "content-length")) {
                            u32 v;

                            if (str_to_u32(header_value(line, "content-length"),
                                           &v)) {
                                content_length = v;
                                have_length = true;
                            }
                        } else if (header_is(line, "transfer-encoding")) {
                            const char *v =
                                header_value(line, "transfer-encoding");

                            if (strcasecmp(v, "chunked") == 0) {
                                bs.chunked = true;
                                resp->chunked = true;
                            }
                        } else if (header_is(line, "content-type")) {
                            strlcpy(resp->content_type,
                                    header_value(line, "content-type"),
                                    sizeof(resp->content_type));
                        }

                        line = nl ? nl + 1 : NULL;
                    }

                    /* A chunked response has no Content-Length, and a
                     * response with both is a framing attack: RFC 7230 says
                     * chunked wins and the length is ignored. */
                    if (bs.chunked)
                        have_length = false;
                }
            }

            if (headers_done && off < (u32)got)
                sink(&bs, tmp + off, (u32)got - off);

            /* Three ways a response ends, and all three have to be here. */
            if (headers_done && bs.chunked && bs.cs == CHUNK_DONE)
                break;
            if (headers_done && have_length && bs.len >= content_length)
                break;

            continue;
        }

        if (got == 0) {
            /* End of stream: the peer closed and the buffer is drained. For
             * a response with no Content-Length and no chunking, this *is*
             * the framing - see the top of the file. */
            break;
        }

        if (got == SYS_EAGAIN) {
            net_poll(16);
            continue;
        }

        /* A reset, or a handle that went away. Whatever arrived before it is
         * still returned, with the error visible in the status being 0 if
         * the headers never completed. */
        break;
    }

    resp->body_bytes = bs.len;
    resp->truncated = bs.truncated;

    /* An orderly close rather than a reset: the server is entitled to a FIN,
     * and tcp_close() hands the rest of the exchange to the stack so that
     * this function does not have to wait out TIME_WAIT. */
    tcp_close(h);

    if (!headers_done) {
        pr_warn("the response ended before its headers did");
        stats.failures++;
        return SYS_EIO;
    }

    stats.responses++;
    return (i32)bs.len;
}

/* ---------------------------------------------------------------------------
 * The subsystem boundary.
 *
 * Everything above is the stack's internals; each public name below takes the
 * network lock and calls it. The lock counts its depth, so an internal caller
 * reaching one of these - socket_send() calling tcp_send() - is correct
 * without having to know whether it is already inside the stack. See
 * kernel/include/net/netlock.h for why this subsystem is the one that gets a
 * recursive lock.
 * ------------------------------------------------------------------------- */

const struct http_stats *http_get_stats(void)
{
    net_lock();
    const struct http_stats *ret = http_get_stats_inner();

    net_unlock();
    return ret;
}
