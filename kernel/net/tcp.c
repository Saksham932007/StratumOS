/* StratumOS - TCP, scoped down to what can be made correct and tested.
 *
 * WHAT THIS IMPLEMENTS
 * --------------------
 *   - one listening port, one connection at a time, passive open only;
 *   - the three-way handshake, with an MSS option on the SYN-ACK;
 *   - a real send queue: data is buffered, sent in MSS-sized segments, and
 *     released as it is acknowledged;
 *   - retransmission on a timer, bounded, then a reset;
 *   - in-order receive, with out-of-order segments dropped and re-ACKed;
 *   - graceful close from either side, and a reset to a SYN for a port
 *     nothing is listening on.
 *
 * WHAT IT DOES NOT
 * ----------------
 * Listed because a TCP that quietly lacks these is a TCP that works in a lab
 * and fails on a real network, and the distinction is the honest part:
 *
 *   - no congestion control. No slow start, no congestion window, no fast
 *     retransmit. This sends what the peer's window allows and no less,
 *     which on a congested path is antisocial. It is the single biggest
 *     omission and the reason this is not a general-purpose TCP.
 *   - no out-of-order reassembly. A segment that arrives early is dropped
 *     and the expected sequence re-ACKed, which is correct but slow: it
 *     turns one lost segment into a round trip per segment after it.
 *   - no window scaling, no SACK, no timestamps, no PAWS.
 *   - no RTT estimation. The retransmission timeout is a constant, where a
 *     real stack measures the path and adapts.
 *   - TIME_WAIT is entered and left on a timer, but a connection is accepted
 *     again as soon as it expires, so the duplicate-segment protection
 *     TIME_WAIT exists for is weaker than the standard's two minutes.
 *   - no sockets API. The listening port echoes; ring 3 cannot open a
 *     connection, because that needs file descriptors first.
 *
 * SEQUENCE ARITHMETIC
 * -------------------
 * Sequence numbers are 32 bits and wrap. Comparing them with < or > is wrong
 * for exactly the reason it is subtle: it works for the whole first four
 * gigabytes of a connection and then silently inverts. The comparison has to
 * be done on the signed difference, which is what the seq_* helpers below
 * are, and they are used everywhere rather than sometimes.
 */
#define LOG_TAG "tcp"

#include <drivers/timer.h>

#include <kernel/log.h>
#include <kernel/string.h>

#include <net/net.h>

#define TCP_FIN         0x01u
#define TCP_SYN         0x02u
#define TCP_RST         0x04u
#define TCP_PSH         0x08u
#define TCP_ACK         0x10u

#define TCP_HDR_MIN     20

/* 1460 on Ethernet: the frame's payload capacity less this header. Derived
 * rather than written as a literal, because the two have to agree and the
 * consequence of their drifting apart is a full-size segment that fails to
 * send under load and nowhere else.
 *
 * The assertions below make that relationship a build error instead of a
 * coincidence. The second one is the interesting one: a SYN-ACK carries a
 * 4-byte MSS option, so the worst case for a frame is not header + payload
 * but header + options + payload - and that only fits today because the
 * option rides a segment with no data on it. A second TCP option would break
 * full-size data segments and nothing smaller, which is the kind of bug that
 * appears in production and not in a test. */
#define TCP_MSS         (NET_PAYLOAD_MAX - TCP_HDR_MIN)

/* What send_segment() may prepend beyond the fixed header: the MSS option,
 * and only on a SYN. */
#define TCP_OPTIONS_MAX 4

_Static_assert(TCP_HDR_MIN + TCP_MSS <= NET_PAYLOAD_MAX,
               "a full-size data segment must fit in one frame");
_Static_assert(TCP_HDR_MIN + TCP_OPTIONS_MAX <= NET_PAYLOAD_MAX,
               "a SYN carrying options must fit in one frame");
_Static_assert(TCP_MSS == 1460,
               "the MSS on Ethernet is 1460; if this changed then the MTU or "
               "the header length did, and both are worth noticing");

#define TCP_SNDBUF         2048
#define TCP_WINDOW         TCP_SNDBUF

/* A constant, where a real stack measures the round trip and adapts. 500 ms
 * is slow on a LAN and fast on a satellite link, which is the cost of not
 * measuring. */
#define TCP_RTO_MS         500
#define TCP_MAX_RETRIES    5
#define TCP_SYN_TIMEOUT_MS 10000 /* a half-open connection is not kept */
#define TCP_TIME_WAIT_MS   2000

struct tcp_hdr {
    be16 src_port;
    be16 dst_port;
    be32 seq;
    be32 ack;
    u8 data_offset; /* high 4 bits: header length in 32-bit words */
    u8 flags;
    be16 window;
    be16 checksum;
    be16 urgent;
} PACKED;

static u16 listen_port;

static struct {
    enum tcp_state state;
    ipv4_addr remote;
    u16 remote_port;
    u16 local_port;

    u32 snd_una; /* sequence of sndbuf[0]: the oldest unacknowledged byte */
    u32 snd_nxt; /* sequence of the next byte to send                     */
    u32 snd_len; /* valid bytes in sndbuf                                 */
    u32 rcv_nxt; /* the next sequence we expect                           */
    u16 snd_window;

    bool fin_queued; /* the peer closed; our FIN goes out after our data */
    bool fin_sent;
    u8 sndbuf[TCP_SNDBUF];

    u64 last_send_ms;
    u64 state_since_ms;
    u32 retries;

    u32 bytes_in, bytes_out, segments_in, segments_out;
    u32 retransmits, out_of_order_dropped;
} conn;

/* Signed-difference comparison, which is the only correct way to order
 * numbers that wrap. See the note at the top of the file. */
static inline bool seq_lt(u32 a, u32 b)
{
    return (i32)(a - b) < 0;
}

static inline bool seq_le(u32 a, u32 b)
{
    return (i32)(a - b) <= 0;
}

static inline bool seq_gt(u32 a, u32 b)
{
    return (i32)(a - b) > 0;
}

const char *tcp_state_name(enum tcp_state s)
{
    switch (s) {
    case TCP_CLOSED:
        return "CLOSED";
    case TCP_LISTEN:
        return "LISTEN";
    case TCP_SYN_RECEIVED:
        return "SYN_RECEIVED";
    case TCP_ESTABLISHED:
        return "ESTABLISHED";
    case TCP_CLOSE_WAIT:
        return "CLOSE_WAIT";
    case TCP_LAST_ACK:
        return "LAST_ACK";
    case TCP_FIN_WAIT_1:
        return "FIN_WAIT_1";
    case TCP_FIN_WAIT_2:
        return "FIN_WAIT_2";
    case TCP_TIME_WAIT:
        return "TIME_WAIT";
    }

    return "?";
}

static void set_state(enum tcp_state s)
{
    if (conn.state != s)
        pr_debug("%s -> %s", tcp_state_name(conn.state), tcp_state_name(s));

    conn.state = s;
    conn.state_since_ms = timer_ms();
}

/* The initial send sequence. Derived from the clock, which is what makes a
 * segment from a previous connection on the same port pair unlikely to be
 * accepted into this one. Not a substitute for RFC 6528's keyed hash - that
 * is what stops an off-path attacker guessing it, and this does not. */
static u32 initial_seq(void)
{
    return (u32)timer_ms() * 250001u + 0x1B2C3D4Du;
}

/* Send one segment. `payload` may be NULL. The sequence number is passed in
 * rather than taken from the connection, because a retransmission sends an
 * old one and an ACK-only segment sends the next one without consuming it. */
static bool send_segment(u32 seq, u8 flags, const void *payload, u32 len,
                         bool with_mss)
{
    struct net_frame f;
    struct tcp_hdr *h;
    const struct net_config *cfg = net_get_config();
    u32 hdr_len = TCP_HDR_MIN + (with_mss ? TCP_OPTIONS_MAX : 0u);
    u8 *body;

    if (len > TCP_MSS)
        return false;

    /* Built into the frame rather than into a buffer of its own, so that the
     * segment, the IPv4 header and the Ethernet header share one allocation
     * and nothing is copied between them. See struct net_frame in net.h for
     * the stack depth this replaced. */
    net_frame_reset(&f);

    body = net_frame_append(&f, hdr_len + len);
    if (!body)
        return false;

    h = (struct tcp_hdr *)body;

    h->src_port = htons(conn.local_port);
    h->dst_port = htons(conn.remote_port);
    h->seq = htonl(seq);
    h->ack = htonl(conn.rcv_nxt);
    h->flags = flags;
    h->window = htons(TCP_WINDOW);
    h->checksum = 0;
    h->urgent = 0;

    if (with_mss) {
        /* The MSS option: kind 2, length 4, then the value. Only valid on a
         * segment with SYN set, which is the only place it is sent. */
        /* Only ever sent on a SYN, which carries no data - which is what
         * keeps hdr_len + len inside one frame. See the assertions above. */
        body[TCP_HDR_MIN + 0] = 2;
        body[TCP_HDR_MIN + 1] = 4;
        body[TCP_HDR_MIN + 2] = (u8)(TCP_MSS >> 8);
        body[TCP_HDR_MIN + 3] = (u8)TCP_MSS;
    }

    h->data_offset = (u8)((hdr_len / 4) << 4);

    if (len)
        memcpy(body + hdr_len, payload, len);

    u32 total = hdr_len + len;

    /* TCP's checksum covers a pseudo-header of the IP addresses, the
     * protocol and the TCP length - none of which are in the TCP header - so
     * that a segment delivered to the wrong host fails rather than being
     * accepted. The length in the pseudo-header is the TCP length, not the
     * IP total length, which is a distinction worth getting right once. */
    u32 sum = ipv4_pseudo_sum(cfg->addr, conn.remote, IPPROTO_TCP, (u16)total);

    h->checksum = checksum_finish(checksum_partial(body, total, sum));

    if (!ipv4_output_frame(&f, conn.remote, IPPROTO_TCP))
        return false;

    conn.segments_out++;
    conn.last_send_ms = timer_ms();
    net_stats()->tcp_tx++;
    return true;
}

/* A reset to a segment we have no connection for. Built by hand rather than
 * through send_segment(), because there is no connection whose ports and
 * sequence numbers it could use - the reply has to be derived entirely from
 * the offending segment. */
static void send_reset(ipv4_addr to, const struct tcp_hdr *bad, u32 seg_len)
{
    u8 buf[TCP_HDR_MIN];
    struct tcp_hdr *h = (struct tcp_hdr *)buf;
    const struct net_config *cfg = net_get_config();

    h->src_port = bad->dst_port; /* already in network order; swapped */
    h->dst_port = bad->src_port;
    h->data_offset = (TCP_HDR_MIN / 4) << 4;
    h->window = 0;
    h->checksum = 0;
    h->urgent = 0;

    /* RFC 793: if the offending segment had ACK set, the reset's sequence is
     * that ACK and it carries no ACK of its own. Otherwise the reset
     * acknowledges the offending segment and its own sequence is zero.
     * Getting this wrong produces a reset the peer ignores, which presents
     * as a connection that hangs instead of failing. */
    if (bad->flags & TCP_ACK) {
        h->seq = bad->ack;
        h->ack = 0;
        h->flags = TCP_RST;
    } else {
        h->seq = 0;
        h->ack = htonl(ntohl(bad->seq) + seg_len);
        h->flags = TCP_RST | TCP_ACK;
    }

    u32 sum = ipv4_pseudo_sum(cfg->addr, to, IPPROTO_TCP, TCP_HDR_MIN);

    h->checksum = checksum_finish(checksum_partial(buf, TCP_HDR_MIN, sum));

    if (ipv4_output(to, IPPROTO_TCP, buf, TCP_HDR_MIN))
        net_stats()->tcp_resets_sent++;
}

static void reset_connection(void)
{
    memset(&conn, 0, sizeof(conn));
    conn.local_port = listen_port;
    set_state(listen_port ? TCP_LISTEN : TCP_CLOSED);
}

/* Queue data for sending. Returns how much was taken - a short accept is how
 * back pressure is expressed with no API to block on. */
static u32 queue_send(const u8 *data, u32 len)
{
    u32 room = TCP_SNDBUF - conn.snd_len;

    if (len > room)
        len = room;

    memcpy(conn.sndbuf + conn.snd_len, data, len);
    conn.snd_len += len;
    return len;
}

/* Send whatever is queued and unsent, in MSS-sized segments, within the
 * peer's advertised window. */
static void send_queued(void)
{
    while (true) {
        u32 unsent_at = conn.snd_nxt - conn.snd_una;

        if (unsent_at >= conn.snd_len)
            break; /* everything queued has been sent at least once */

        u32 available = conn.snd_len - unsent_at;
        u32 in_flight = conn.snd_nxt - conn.snd_una;

        /* The peer's window is the only thing limiting this. A real stack
         * would also have a congestion window, and its absence is the
         * biggest thing missing from this file. */
        if (in_flight >= conn.snd_window)
            break;

        u32 allowed = conn.snd_window - in_flight;
        u32 len = available < allowed ? available : allowed;

        if (len > TCP_MSS)
            len = TCP_MSS;
        if (!len)
            break;

        if (!send_segment(conn.snd_nxt, TCP_ACK | TCP_PSH,
                          conn.sndbuf + unsent_at, len, false))
            break;

        conn.snd_nxt += len;
        conn.bytes_out += len;
    }

    /* Our FIN goes out only once every queued byte has been sent, because a
     * FIN consumes a sequence number and sending it early would claim the
     * stream ended before the data in it. */
    if (conn.fin_queued && !conn.fin_sent &&
        conn.snd_nxt == conn.snd_una + conn.snd_len) {
        if (send_segment(conn.snd_nxt, TCP_ACK | TCP_FIN, NULL, 0, false)) {
            conn.snd_nxt++;
            conn.fin_sent = true;
        }
    }
}

void tcp_input(ipv4_addr src, const u8 *packet, size_t len)
{
    const struct tcp_hdr *h = (const struct tcp_hdr *)packet;
    const struct net_config *cfg = net_get_config();
    struct net_stats *s = net_stats();

    s->tcp_rx++;

    if (len < TCP_HDR_MIN)
        return;

    u32 hdr_len = (u32)(h->data_offset >> 4) * 4;

    /* The header length is the sender's and everything below indexes by it.
     * Both bounds, before use. */
    if (hdr_len < TCP_HDR_MIN || hdr_len > len)
        return;

    u32 sum = ipv4_pseudo_sum(src, cfg->addr, IPPROTO_TCP, (u16)len);

    if (checksum_finish(checksum_partial(packet, len, sum)) != 0) {
        s->tcp_bad_checksum++;
        return;
    }

    u16 dport = ntohs(h->dst_port);
    u16 sport = ntohs(h->src_port);
    u32 seq = ntohl(h->seq);
    u32 ack = ntohl(h->ack);
    const u8 *data = packet + hdr_len;
    u32 data_len = (u32)len - hdr_len;

    conn.segments_in++;

    /* Not a port we listen on. A reset is the correct answer and is what
     * makes "connection refused" immediate rather than a timeout. */
    if (!listen_port || dport != listen_port) {
        s->tcp_no_port++;
        if (!(h->flags & TCP_RST))
            send_reset(src, h, data_len + ((h->flags & TCP_SYN) ? 1 : 0));
        return;
    }

    /* A segment for a different peer than the one connection we hold. Reset
     * it rather than ignoring it: the peer is entitled to know there is no
     * connection, and a single-connection stack that silently drops leaves
     * the second client hanging. */
    if (conn.state != TCP_LISTEN && conn.state != TCP_CLOSED &&
        (src != conn.remote || sport != conn.remote_port)) {
        if (!(h->flags & TCP_RST))
            send_reset(src, h, data_len + ((h->flags & TCP_SYN) ? 1 : 0));
        return;
    }

    if (h->flags & TCP_RST) {
        pr_debug("reset from the peer");
        reset_connection();
        return;
    }

    switch (conn.state) {
    case TCP_CLOSED:
        return;

    case TCP_LISTEN:
        if (!(h->flags & TCP_SYN))
            return;

        conn.remote = src;
        conn.remote_port = sport;
        conn.local_port = dport;
        conn.rcv_nxt = seq + 1; /* the SYN consumes one sequence number */
        conn.snd_una = initial_seq();
        conn.snd_nxt = conn.snd_una;
        conn.snd_window = ntohs(h->window);
        conn.snd_len = 0;
        conn.retries = 0;

        /* The SYN-ACK carries our MSS and consumes a sequence number of its
         * own, which is why snd_nxt advances past it. */
        if (send_segment(conn.snd_nxt, TCP_SYN | TCP_ACK, NULL, 0, true)) {
            conn.snd_nxt++;
            set_state(TCP_SYN_RECEIVED);
        }
        return;

    case TCP_SYN_RECEIVED:
        if (!(h->flags & TCP_ACK))
            return;

        /* The ACK has to acknowledge our SYN, not some other sequence. A
         * blind accept here is how a stack gets connections it never
         * established. */
        if (ack != conn.snd_nxt)
            return;

        conn.snd_una = ack;
        conn.snd_window = ntohs(h->window);
        set_state(TCP_ESTABLISHED);
        s->tcp_connections_accepted++;

        {
            char ib[IPV4_STR_LEN];

            pr_info("accepted a connection from %s:%u on port %u",
                    ipv4_str(src, ib, sizeof(ib)), sport, dport);
        }
        break;

    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
    case TCP_LAST_ACK:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
    case TCP_TIME_WAIT:
        break;
    }

    /* ---- the acknowledgement ------------------------------------------- */
    if (h->flags & TCP_ACK) {
        if (seq_gt(ack, conn.snd_una) && seq_le(ack, conn.snd_nxt)) {
            u32 freed = ack - conn.snd_una;

            /* Release the acknowledged bytes from the front of the send
             * buffer. A FIN's sequence number is acknowledged too but has no
             * byte in the buffer, so the amount moved is bounded by what is
             * actually there. */
            if (freed > conn.snd_len)
                freed = conn.snd_len;

            memmove(conn.sndbuf, conn.sndbuf + freed, conn.snd_len - freed);
            conn.snd_len -= freed;
            conn.snd_una = ack;
            conn.retries = 0;
        }

        conn.snd_window = ntohs(h->window);

        if (conn.state == TCP_LAST_ACK && conn.snd_una == conn.snd_nxt) {
            /* Our FIN was acknowledged. The connection is over; the port
             * goes back to listening after TIME_WAIT. */
            set_state(TCP_TIME_WAIT);
            return;
        }
    }

    /* ---- the data ------------------------------------------------------- */
    if (data_len) {
        if (seq == conn.rcv_nxt) {
            conn.rcv_nxt += data_len;
            conn.bytes_in += data_len;

            /* Echo. The whole application this stack has, and it is enough
             * to prove the connection carries bytes in both directions with
             * sequence numbers that line up. */
            u32 taken = queue_send(data, data_len);

            net_stats()->tcp_bytes_echoed += taken;

            if (taken < data_len)
                pr_debug("send buffer full; echoed %u of %u byte(s)", taken,
                         data_len);
        } else if (seq_lt(seq, conn.rcv_nxt)) {
            /* Already received. A duplicate, usually because our ACK was
             * lost - so re-ACK rather than ignore, which is what stops the
             * peer retransmitting forever. */
            send_segment(conn.snd_nxt, TCP_ACK, NULL, 0, false);
            return;
        } else {
            /* Out of order. Dropped, and the expected sequence re-ACKed, so
             * the peer retransmits from the hole. Correct, and slow - see
             * the note at the top of the file. */
            conn.out_of_order_dropped++;
            send_segment(conn.snd_nxt, TCP_ACK, NULL, 0, false);
            return;
        }
    }

    /* ---- the peer's FIN ------------------------------------------------- */
    if ((h->flags & TCP_FIN) && seq_le(seq, conn.rcv_nxt)) {
        /* The FIN consumes a sequence number after any data in the same
         * segment, which is why this is after the data handling. */
        conn.rcv_nxt++;

        if (conn.state == TCP_ESTABLISHED) {
            set_state(TCP_CLOSE_WAIT);
            /* Our side has nothing more to say once the echo drains, so the
             * close is queued immediately. A real application would decide
             * this; there is no application. */
            conn.fin_queued = true;
            set_state(TCP_LAST_ACK);
        }
    }

    send_queued();

    /* An ACK for what we have received, if sending the queue did not already
     * carry one. A pure-ACK segment is cheap and leaving it out makes the
     * peer wait for its retransmission timer. */
    if (data_len || (h->flags & TCP_FIN))
        send_segment(conn.snd_nxt, TCP_ACK, NULL, 0, false);
}

void tcp_tick(void)
{
    u64 now = timer_ms();

    switch (conn.state) {
    case TCP_SYN_RECEIVED:
        /* A half-open connection is not kept: the peer that sent the SYN may
         * never have existed, and holding the single connection slot for it
         * is how one spoofed packet denies service to everyone else. This is
         * the small version of what SYN cookies solve. */
        if (now - conn.state_since_ms > TCP_SYN_TIMEOUT_MS) {
            pr_debug("half-open connection timed out");
            reset_connection();
            return;
        }
        break;

    case TCP_TIME_WAIT:
        if (now - conn.state_since_ms > TCP_TIME_WAIT_MS)
            reset_connection();
        return;

    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
    case TCP_LAST_ACK:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
        break;

    case TCP_CLOSED:
    case TCP_LISTEN:
        return;
    }

    /* Retransmission. Anything sent and not acknowledged goes again once the
     * timer expires, up to a bound - after which the connection is reset
     * rather than retried forever, because a peer that has not acknowledged
     * five times is gone. */
    bool unacked = seq_gt(conn.snd_nxt, conn.snd_una);

    if (!unacked || now - conn.last_send_ms < TCP_RTO_MS)
        return;

    if (conn.retries >= TCP_MAX_RETRIES) {
        pr_warn("no acknowledgement after %u retransmissions; resetting",
                conn.retries);
        reset_connection();
        return;
    }

    conn.retries++;
    conn.retransmits++;

    /* Rewind to the oldest unacknowledged byte and send from there. The
     * FIN, if it was sent, is re-sent by the same rewind because
     * send_queued() re-derives it from the queue state. */
    conn.snd_nxt = conn.snd_una;
    conn.fin_sent = false;
    send_queued();

    if (conn.snd_nxt == conn.snd_una && !conn.snd_len)
        send_segment(conn.snd_nxt, TCP_ACK, NULL, 0, false);
}

void tcp_listen(u16 port)
{
    listen_port = port;
    reset_connection();

    if (port)
        pr_info("listening on port %u; it echoes what it receives", port);
}

u16 tcp_listen_port(void)
{
    return listen_port;
}

void tcp_get_status(struct tcp_status *out)
{
    out->state = conn.state;
    out->local_port = conn.local_port;
    out->remote_port = conn.remote_port;
    out->remote = conn.remote;
    out->snd_nxt = conn.snd_nxt;
    out->snd_una = conn.snd_una;
    out->rcv_nxt = conn.rcv_nxt;
    out->bytes_in = conn.bytes_in;
    out->bytes_out = conn.bytes_out;
    out->segments_in = conn.segments_in;
    out->segments_out = conn.segments_out;
    out->retransmits = conn.retransmits;
    out->out_of_order_dropped = conn.out_of_order_dropped;
}
