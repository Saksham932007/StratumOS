/* StratumOS - TCP: several connections, congestion control, and both opens.
 *
 * WHAT CHANGED, AND WHY IT ALL CHANGED AT ONCE
 * --------------------------------------------
 * This file used to hold one connection in one static struct, listen on one
 * port, and echo. That was a defensible first TCP - the handshake, the send
 * queue and the sequence arithmetic are the same whether there is one
 * connection or sixty - but three of the things it was missing could not be
 * added separately:
 *
 *   A sockets API needs more than one connection, because a listener and an
 *   accepted connection have to exist at the same time. With a single
 *   connection, accept() would have to destroy the thing that produced it.
 *
 *   An outgoing connection needs more than one connection for the same
 *   reason: the echo port is still listening while an HTTP client is talking
 *   to a server somewhere else.
 *
 *   Congestion control needs a per-connection window, and a congestion
 *   window on a stack that can only hold one connection cannot be tested
 *   against anything - there is no second flow to be fair to.
 *
 * So the single struct became a small fixed table, demultiplexed on the
 * four-tuple, and everything that was implicitly "the connection" is now a
 * `struct tcb *` passed in. That is the mechanical half of this file's
 * history. The interesting half is below.
 *
 * CONGESTION CONTROL
 * ------------------
 * The old file sent whatever the peer's receive window allowed. That is
 * exactly wrong on a congested path, and wrong in a way that is invisible
 * when you test on a loopback: the receive window says what the *peer* can
 * absorb, and says nothing at all about what the *path between you* can
 * carry. A sender limited only by the receive window responds to loss - the
 * signal that the path is full - by retransmitting at the same rate, which is
 * what turns congestion into collapse. This was the behaviour that made the
 * internet unusable in October 1986 and the reason Van Jacobson's paper
 * exists.
 *
 * So there are now two windows and the smaller one governs:
 *
 *   snd_window  what the peer said it can receive. Flow control.
 *   cwnd        what this sender believes the path can carry. Congestion
 *               control. Nobody advertises it; it is inferred from loss.
 *
 * It moves by RFC 5681:
 *
 *   Slow start (cwnd < ssthresh): cwnd grows by one MSS per acknowledgement,
 *   which doubles it every round trip. "Slow" is historical and misleading -
 *   it is exponential, and the slow part is that it starts from two segments
 *   instead of from the receive window.
 *
 *   Congestion avoidance (cwnd >= ssthresh): cwnd grows by roughly one MSS
 *   per round trip, by accumulating acknowledged bytes and adding an MSS each
 *   time a window's worth has been acknowledged. Linear growth, because past
 *   ssthresh we have evidence the path's limit is near and probing gently is
 *   the point.
 *
 *   Loss by timeout: the harshest signal, because nothing got through.
 *   ssthresh halves to the flight size and cwnd drops to one segment -
 *   back to slow start from the bottom.
 *
 *   Loss by three duplicate acknowledgements: a weaker signal, because the
 *   duplicates are themselves proof that later segments are arriving. So
 *   this halves ssthresh, retransmits the one missing segment immediately
 *   rather than waiting for the timer (fast retransmit), and continues at
 *   the halved rate rather than restarting (fast recovery). The difference
 *   between this and a timeout is most of what makes TCP usable on a link
 *   that loses the occasional packet.
 *
 * MEASURING THE ROUND TRIP
 * ------------------------
 * The retransmission timeout used to be the constant 500 ms, which is far
 * too long for a LAN and far too short for anything over a satellite. It is
 * now measured, by Jacobson's estimator:
 *
 *   srtt   = 7/8 srtt + 1/8 sample          a smoothed average
 *   rttvar = 3/4 rttvar + 1/4 |srtt-sample| its mean deviation
 *   rto    = srtt + 4 rttvar                clamped to a sane range
 *
 * The variance term is the part that matters and the part that was missing
 * from pre-1988 implementations: a timeout set from the average alone fires
 * spuriously whenever the path's delay is merely variable, and a spurious
 * retransmission on a congested path is the worst possible response.
 *
 * Karn's algorithm is why `rtt_pending` is cleared on retransmission. If a
 * segment is sent twice and then acknowledged, there is no way to know which
 * copy the acknowledgement is for, so the sample is ambiguous and is thrown
 * away rather than averaged in. Without this, one timeout poisons the
 * estimate with a sample that is roughly one RTO too long, which raises the
 * RTO, which makes the next sample worse.
 *
 * WHAT IT STILL DOES NOT DO
 * -------------------------
 *   - no out-of-order reassembly. A segment that arrives early is still
 *     dropped and the expected sequence re-ACKed. Fast retransmit makes the
 *     *sender* side of a single loss cheap; the receiver side still costs a
 *     round trip per segment after the hole, because there is nowhere to put
 *     a segment that is not next. This is the largest thing left, and it is
 *     a reassembly queue rather than a change to the state machine.
 *   - no SACK, no window scaling, no timestamps, no PAWS. Window scaling in
 *     particular means the window is capped at 65535, so this cannot fill a
 *     path with a bandwidth-delay product above that.
 *   - no Nagle and no delayed ACK, so a one-byte write becomes a one-byte
 *     segment and every segment is acknowledged immediately. Chatty, and
 *     easier to read in a packet capture.
 *   - TIME_WAIT is two seconds, not two minutes.
 *   - the initial sequence number is derived from the clock, not from
 *     RFC 6528's keyed hash, so it is not unguessable by an off-path
 *     attacker.
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
#include <kernel/syscall_abi.h>

#include <net/net.h>
#include <net/netlock.h>

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
 * coincidence. The second one is the interesting one: a SYN carries a 4-byte
 * MSS option, so the worst case for a frame is not header + payload but
 * header + options + payload - and that only fits today because the option
 * rides a segment with no data on it. A second TCP option would break
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

/* Per connection. Four of these plus the headers is what bounds the table at
 * TCP_MAX_CONNS: a send buffer and a receive buffer each, so the memory is
 * TCP_MAX_CONNS * (TCP_SNDBUF + TCP_RCVBUF) and it is all in BSS, because a
 * connection that can fail to allocate at the wrong moment is a connection
 * whose error paths cannot be tested. */
#define TCP_SNDBUF         4096
#define TCP_RCVBUF         4096

/* How many ports the socket layer may have passive opens on, over and above
 * the echo service's one. */
#define TCP_LISTENERS_MAX  8

/* The retransmission timeout is measured now, so these are only the bounds
 * on the measurement. The floor stops a fast LAN measuring an RTO so short
 * that ordinary scheduling jitter looks like loss; the ceiling stops one
 * pathological sample parking the connection for a minute. */
#define TCP_RTO_MIN_MS     200
#define TCP_RTO_MAX_MS     8000
/* Used until the first sample arrives, which is to say for the handshake. */
#define TCP_RTO_INITIAL_MS 1000

#define TCP_MAX_RETRIES    5
#define TCP_SYN_TIMEOUT_MS 10000 /* a half-open connection is not kept */
#define TCP_TIME_WAIT_MS   2000
/* How long the stack carries a connection whose owner has already gone. */
#define TCP_ORPHAN_MS      15000

/* RFC 5681 allows an initial window of up to 10 segments on a modern path.
 * Two is the conservative classic value and is what this uses, because the
 * buffers here are small enough that a larger burst would be limited by the
 * send buffer rather than by the congestion window - and a congestion window
 * that is never the binding constraint is one whose arithmetic is untested. */
#define TCP_INIT_CWND      (2u * TCP_MSS)
/* Where slow start gives way initially: high enough to be no constraint, so
 * that the first thing a connection does is slow start, as it should. */
#define TCP_INIT_SSTHRESH  0xFFFFu
/* Three, which is the number that has to be chosen and the number everyone
 * chose. Below it, a single reordered segment would trigger a spurious fast
 * retransmit; above it, recovery waits longer than it needs to. */
#define TCP_DUP_ACK_THRESH 3

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

/* A transmission control block: one connection's entire state.
 *
 * `gen` is the part that is not obvious. A handle is an index plus this
 * counter, and the counter rises every time the slot is reused. Without it, a
 * socket that closed, and whose slot was immediately handed to a new
 * connection, would find its stale handle silently addressing a different
 * peer - reads succeeding with someone else's bytes. It is the same hazard a
 * file descriptor has, and it is solved here rather than in the socket layer
 * because the stack is what reuses the slot. */
struct tcb {
    bool used;
    u16 gen;
    u8 index;

    enum tcp_state state;
    ipv4_addr remote;
    u16 remote_port;
    u16 local_port;

    /* Who this connection belongs to, which decides what happens to the
     * bytes that arrive on it. `echo` means the stack's own echo service
     * owns it and received data is turned straight around. Otherwise the
     * data goes into rcvbuf for whoever holds the handle. */
    bool echo;
    bool claimable; /* finished its handshake, nobody has accepted it yet */
    bool released;  /* the owner is gone; reclaim when the close finishes */
    bool rcv_eof;   /* the peer's FIN has arrived; no more data will come  */

    u32 snd_una; /* sequence of sndbuf[0]: the oldest unacknowledged byte */
    u32 snd_nxt; /* sequence of the next byte to send                     */
    u32 snd_len; /* valid bytes in sndbuf                                 */
    u32 rcv_nxt; /* the next sequence we expect                           */
    u32 rcv_len; /* valid bytes in rcvbuf                                 */
    u16 snd_window;

    bool fin_queued; /* our FIN goes out after whatever is queued */
    bool fin_sent;

    /* Congestion control. See the long note at the top of the file. */
    u32 cwnd;
    u32 ssthresh;
    u32 cwnd_acked; /* bytes acknowledged toward the next MSS of growth */
    u32 dup_acks;
    bool in_recovery;
    u32 recover_seq; /* snd_nxt when recovery began: where it ends       */

    /* The round-trip estimator, and the one segment currently being timed. */
    u32 srtt_ms;
    u32 rttvar_ms;
    u32 rto_ms;
    bool rtt_pending;
    u32 rtt_seq; /* the sequence an ACK must reach to close the sample */
    u64 rtt_start_ms;

    u64 last_send_ms;
    u64 state_since_ms;
    u32 retries;

    u32 bytes_in, bytes_out, segments_in, segments_out;
    u32 retransmits, out_of_order_dropped, fast_retransmits, timeouts;

    u8 sndbuf[TCP_SNDBUF];
    u8 rcvbuf[TCP_RCVBUF];
};

static struct tcb conns[TCP_MAX_CONNS];
static u16 listen_port; /* the echo service */
static u16 listeners[TCP_LISTENERS_MAX];
static u16 next_ephemeral = 49152;

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

static inline u32 umin(u32 a, u32 b)
{
    return a < b ? a : b;
}

static inline u32 umax(u32 a, u32 b)
{
    return a > b ? a : b;
}

const char *tcp_state_name(enum tcp_state s)
{
    switch (s) {
    case TCP_CLOSED:
        return "CLOSED";
    case TCP_LISTEN:
        return "LISTEN";
    case TCP_SYN_SENT:
        return "SYN_SENT";
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

static void set_state(struct tcb *t, enum tcp_state s)
{
    if (t->state != s)
        pr_debug("conn %u: %s -> %s", t->index, tcp_state_name(t->state),
                 tcp_state_name(s));

    t->state = s;
    t->state_since_ms = timer_ms();
}

/* ---- the connection table ---------------------------------------------- */

/* A handle is the generation in the high bits and the index in the low ones.
 * The generation starts at 1, so a valid handle is never 0 - which costs
 * nothing and makes a zeroed-out handle field fail closed. */
#define HANDLE_INDEX_BITS 4
_Static_assert(TCP_MAX_CONNS <= (1 << HANDLE_INDEX_BITS),
               "the handle encoding has room for every connection");

static int handle_of(const struct tcb *t)
{
    return (int)(((u32)t->gen << HANDLE_INDEX_BITS) | t->index);
}

static struct tcb *tcb_of(int handle)
{
    if (handle <= 0)
        return NULL;

    u32 idx = (u32)handle & ((1u << HANDLE_INDEX_BITS) - 1u);
    u32 gen = (u32)handle >> HANDLE_INDEX_BITS;

    if (idx >= TCP_MAX_CONNS)
        return NULL;

    struct tcb *t = &conns[idx];

    /* The generation check. A handle to a slot that has since been reused
     * refers to nothing, and saying so is the whole point of the counter. */
    if (!t->used || t->gen != gen)
        return NULL;

    return t;
}

static void tcb_free(struct tcb *t)
{
    u16 gen = t->gen;
    u8 index = t->index;

    memset(t, 0, sizeof(*t));

    /* Advance the generation so every handle to the old occupant is now
     * stale. Wrapping past 0 would make a very old handle valid again, which
     * is why 0 is skipped rather than allowed. */
    t->gen = (u16)(gen + 1);
    if (!t->gen)
        t->gen = 1;
    t->index = index;
    t->used = false;
    t->state = TCP_CLOSED;
}

static struct tcb *tcb_alloc(void)
{
    for (u32 i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcb *t = &conns[i];

        if (t->used)
            continue;

        u16 gen = t->gen ? t->gen : 1;

        memset(t, 0, sizeof(*t));
        t->gen = gen;
        t->index = (u8)i;
        t->used = true;
        t->state = TCP_CLOSED;
        t->cwnd = TCP_INIT_CWND;
        t->ssthresh = TCP_INIT_SSTHRESH;
        t->rto_ms = TCP_RTO_INITIAL_MS;
        t->state_since_ms = timer_ms();
        t->last_send_ms = t->state_since_ms;
        return t;
    }

    return NULL;
}

/* The connection this segment belongs to, matched on the full four-tuple.
 * Not on the destination port alone, which is what a single-connection stack
 * could get away with and what makes two clients of one port indistinguishable. */
static struct tcb *tcb_lookup(ipv4_addr src, u16 sport, u16 dport)
{
    for (u32 i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcb *t = &conns[i];

        if (!t->used || t->state == TCP_CLOSED)
            continue;
        if (t->local_port == dport && t->remote_port == sport &&
            t->remote == src)
            return t;
    }

    return NULL;
}

static bool port_listening(u16 port)
{
    if (!port)
        return false;
    if (listen_port && port == listen_port)
        return true;

    for (u32 i = 0; i < TCP_LISTENERS_MAX; i++)
        if (listeners[i] == port)
            return true;

    return false;
}

static bool port_in_use(u16 port)
{
    if (port_listening(port))
        return true;

    for (u32 i = 0; i < TCP_MAX_CONNS; i++)
        if (conns[i].used && conns[i].local_port == port)
            return true;

    return false;
}

static u16 tcp_ephemeral_port_inner(void)
{
    /* A linear sweep of the ephemeral range, starting where the last one
     * left off. Sequential rather than random, which is the easier thing to
     * follow in a capture and the weaker thing against an off-path attacker
     * guessing the tuple - the same trade-off initial_seq() makes, and worth
     * naming in both places rather than neither. */
    for (u32 tries = 0; tries < 16384; tries++) {
        u16 port = next_ephemeral++;

        if (next_ephemeral == 0 || next_ephemeral < 49152)
            next_ephemeral = 49152;

        if (!port_in_use(port))
            return port;
    }

    return 0;
}

/* How much room the receiving side has, which is what goes in the window
 * field. A real number rather than a constant: for an echo connection the
 * received bytes land in the send queue, so the send queue's free space is
 * what bounds them, and for an owned connection it is the receive buffer's.
 *
 * The old file advertised a constant and then dropped data when the echo
 * queue was full, which is flow control that does not control anything. */
static u16 rcv_window(const struct tcb *t)
{
    u32 room = t->echo ? TCP_SNDBUF - t->snd_len : TCP_RCVBUF - t->rcv_len;

    return (u16)umin(room, 0xFFFFu);
}

/* ---- sending ------------------------------------------------------------ */

/* The initial send sequence. Derived from the clock, which is what makes a
 * segment from a previous connection on the same port pair unlikely to be
 * accepted into this one. Not a substitute for RFC 6528's keyed hash - that
 * is what stops an off-path attacker guessing it, and this does not. */
static u32 initial_seq(void)
{
    static u32 salt;

    salt += 0x9E3779B9u;
    return (u32)timer_ms() * 250001u + 0x1B2C3D4Du + salt;
}

/* Send one segment. `payload` may be NULL. The sequence number is passed in
 * rather than taken from the connection, because a retransmission sends an
 * old one and an ACK-only segment sends the next one without consuming it. */
static bool send_segment(struct tcb *t, u32 seq, u8 flags, const void *payload,
                         u32 len, bool with_mss)
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

    h->src_port = htons(t->local_port);
    h->dst_port = htons(t->remote_port);
    h->seq = htonl(seq);
    h->ack = htonl(t->rcv_nxt);
    h->flags = flags;
    h->window = htons(rcv_window(t));
    h->checksum = 0;
    h->urgent = 0;

    if (with_mss) {
        /* The MSS option: kind 2, length 4, then the value. Only valid on a
         * segment with SYN set, which is the only place it is sent - and a
         * SYN carries no data, which is what keeps hdr_len + len inside one
         * frame. See the assertions above. */
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
    u32 sum = ipv4_pseudo_sum(cfg->addr, t->remote, IPPROTO_TCP, (u16)total);

    h->checksum = checksum_finish(checksum_partial(body, total, sum));

    if (!ipv4_output_frame(&f, t->remote, IPPROTO_TCP))
        return false;

    t->segments_out++;
    t->last_send_ms = timer_ms();
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

/* Queue data for sending. Returns how much was taken - a short accept is how
 * back pressure is expressed with no API to block on. */
static u32 queue_send(struct tcb *t, const u8 *data, u32 len)
{
    u32 room = TCP_SNDBUF - t->snd_len;

    if (len > room)
        len = room;

    memcpy(t->sndbuf + t->snd_len, data, len);
    t->snd_len += len;
    return len;
}

/* The sending window: the smaller of what the peer will accept and what the
 * path is believed to carry. This one line is the whole of what congestion
 * control does to the send path; everything else in this file about cwnd is
 * the business of deciding what to put in it. */
static u32 send_window(const struct tcb *t)
{
    return umin(t->snd_window, t->cwnd);
}

/* Send whatever is queued and unsent, in MSS-sized segments, within the
 * smaller of the two windows. */
static void send_queued(struct tcb *t)
{
    while (true) {
        u32 unsent_at = t->snd_nxt - t->snd_una;

        if (unsent_at >= t->snd_len)
            break; /* everything queued has been sent at least once */

        u32 available = t->snd_len - unsent_at;
        u32 in_flight = t->snd_nxt - t->snd_una;
        u32 window = send_window(t);

        if (in_flight >= window)
            break;

        u32 len = umin(available, window - in_flight);

        if (len > TCP_MSS)
            len = TCP_MSS;
        if (!len)
            break;

        if (!send_segment(t, t->snd_nxt, TCP_ACK | TCP_PSH,
                          t->sndbuf + unsent_at, len, false))
            break;

        /* Time this segment if no sample is in flight. One timed segment at
         * a time is all the estimator needs, and it keeps Karn's rule - no
         * sample from a retransmission - down to clearing one flag. */
        if (!t->rtt_pending) {
            t->rtt_pending = true;
            t->rtt_seq = t->snd_nxt + len;
            t->rtt_start_ms = timer_ms();
        }

        t->snd_nxt += len;
        t->bytes_out += len;
    }

    /* Our FIN goes out only once every queued byte has been sent, because a
     * FIN consumes a sequence number and sending it early would claim the
     * stream ended before the data in it. */
    if (t->fin_queued && !t->fin_sent &&
        t->snd_nxt == t->snd_una + t->snd_len) {
        if (send_segment(t, t->snd_nxt, TCP_ACK | TCP_FIN, NULL, 0, false)) {
            t->snd_nxt++;
            t->fin_sent = true;
        }
    }
}

/* ---- the round-trip estimator ------------------------------------------ */

static void rto_update(struct tcb *t)
{
    u32 rto = t->srtt_ms + 4u * t->rttvar_ms;

    t->rto_ms = umax(TCP_RTO_MIN_MS, umin(rto, TCP_RTO_MAX_MS));
}

static void rtt_sample(struct tcb *t, u32 sample_ms)
{
    if (!t->srtt_ms) {
        /* The first sample has no history to smooth against, so RFC 6298
         * seeds the average with it and the deviation with half of it. */
        t->srtt_ms = sample_ms;
        t->rttvar_ms = sample_ms / 2;
    } else {
        u32 diff = t->srtt_ms > sample_ms ? t->srtt_ms - sample_ms
                                          : sample_ms - t->srtt_ms;

        t->rttvar_ms = (3u * t->rttvar_ms + diff) / 4u;
        t->srtt_ms = (7u * t->srtt_ms + sample_ms) / 8u;
    }

    rto_update(t);
}

/* ---- congestion control ------------------------------------------------- */

/* The window to fall back to on loss: half the flight size, and never less
 * than two segments, because a congestion window below one segment cannot
 * send anything and a connection that cannot send cannot recover. */
static u32 reduced_ssthresh(const struct tcb *t)
{
    u32 flight = t->snd_nxt - t->snd_una;

    return umax(flight / 2u, 2u * TCP_MSS);
}

static void cwnd_on_ack(struct tcb *t, u32 acked)
{
    if (!acked)
        return;

    if (t->cwnd < t->ssthresh) {
        /* Slow start: one MSS per acknowledgement, which doubles the window
         * every round trip. Bounded by the acknowledged amount so that a
         * stretch ACK covering several segments does not grow the window by
         * more than it confirmed. */
        t->cwnd += umin(acked, TCP_MSS);
        t->cwnd_acked = 0;
    } else {
        /* Congestion avoidance: about one MSS per round trip. Accumulating
         * the acknowledged bytes and spending a window's worth per MSS is
         * the integer form of cwnd += MSS*MSS/cwnd, and it does not lose the
         * remainder the way the direct division does. */
        t->cwnd_acked += acked;

        while (t->cwnd_acked >= t->cwnd) {
            t->cwnd_acked -= t->cwnd;
            t->cwnd += TCP_MSS;
        }
    }

    /* There is no point in a congestion window larger than everything that
     * could ever be in flight, and letting it grow without bound would make
     * the halving on loss meaningless for a long time afterwards. */
    t->cwnd = umin(t->cwnd, TCP_SNDBUF + TCP_MSS);
}

/* Retransmit the oldest unacknowledged segment, and only that one. Used by
 * fast retransmit, where the duplicate acknowledgements are evidence that
 * everything after the hole arrived and resending it all would be waste. */
static void retransmit_one(struct tcb *t)
{
    u32 len = umin(t->snd_len, TCP_MSS);

    if (!len) {
        /* Nothing in the buffer: what is unacknowledged is the FIN. */
        if (t->fin_sent)
            send_segment(t, t->snd_una, TCP_ACK | TCP_FIN, NULL, 0, false);
        return;
    }

    if (send_segment(t, t->snd_una, TCP_ACK | TCP_PSH, t->sndbuf, len, false)) {
        t->retransmits++;
        /* Karn: a retransmitted segment can never produce a usable sample,
         * because an acknowledgement for it does not say which copy arrived. */
        t->rtt_pending = false;
    }
}

static void enter_fast_recovery(struct tcb *t)
{
    t->ssthresh = reduced_ssthresh(t);
    /* ssthresh plus the three segments the duplicate acknowledgements say
     * have left the network. Inflating the window by what has provably been
     * delivered is what lets recovery keep sending while the hole is
     * repaired, which is the difference between fast recovery and simply
     * halving and waiting. */
    t->cwnd = t->ssthresh + TCP_DUP_ACK_THRESH * TCP_MSS;
    t->cwnd_acked = 0;
    t->in_recovery = true;
    t->recover_seq = t->snd_nxt;
    t->fast_retransmits++;

    pr_debug("conn %u: fast retransmit, ssthresh %u cwnd %u", t->index,
             t->ssthresh, t->cwnd);

    retransmit_one(t);
}

static void on_timeout(struct tcb *t)
{
    /* The harsh signal: nothing got through, so assume the path is far
     * worse than believed and restart slow start from one segment. */
    t->ssthresh = reduced_ssthresh(t);
    t->cwnd = TCP_MSS;
    t->cwnd_acked = 0;
    t->in_recovery = false;
    t->dup_acks = 0;
    t->timeouts++;

    /* Exponential backoff. A path that did not answer once will often not
     * answer twice, and retrying at the same interval is how a stack adds
     * load to a link that is already failing. */
    t->rto_ms = umin(t->rto_ms * 2u, TCP_RTO_MAX_MS);
    t->rtt_pending = false;
}

/* ---- teardown ----------------------------------------------------------- */

/* Release the connection if the stack is finished with it and nobody owns
 * it. An owned connection is kept so that its holder can still read what
 * arrived before the close, which is the behaviour every socket API has. */
static void maybe_reclaim(struct tcb *t)
{
    if (t->state != TCP_CLOSED)
        return;
    if (t->echo || t->released || t->claimable)
        tcb_free(t);
}

/* ---- receiving ---------------------------------------------------------- */

static u32 deliver(struct tcb *t, const u8 *data, u32 len)
{
    if (t->echo) {
        /* Echo. The whole application the stack itself has, and it is enough
         * to prove the connection carries bytes in both directions with
         * sequence numbers that line up. Only as much as the send queue can
         * hold is accepted, and the rest is left for the peer to resend -
         * which is what the advertised window already told it. */
        u32 taken = queue_send(t, data, len);

        net_stats()->tcp_bytes_echoed += taken;
        return taken;
    }

    u32 room = TCP_RCVBUF - t->rcv_len;
    u32 taken = umin(len, room);

    memcpy(t->rcvbuf + t->rcv_len, data, taken);
    t->rcv_len += taken;
    return taken;
}

static void tcp_input_inner(ipv4_addr src, const u8 *packet, size_t len)
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
    u32 seg_consumed = data_len + ((h->flags & TCP_SYN) ? 1u : 0u);

    struct tcb *t = tcb_lookup(src, sport, dport);

    if (!t) {
        /* No connection for this tuple. A SYN to a listening port opens one;
         * anything else gets a reset, which is what makes "connection
         * refused" immediate rather than a timeout. */
        if ((h->flags & TCP_SYN) && !(h->flags & TCP_ACK) &&
            port_listening(dport)) {
            t = tcb_alloc();

            if (!t) {
                /* The table is full. A reset is the honest answer: the peer
                 * learns now rather than retransmitting its SYN for a minute
                 * into a stack that will never answer. */
                pr_debug("no free connection for a SYN on port %u", dport);
                send_reset(src, h, seg_consumed);
                return;
            }

            t->remote = src;
            t->remote_port = sport;
            t->local_port = dport;
            t->echo = (listen_port && dport == listen_port);
            t->rcv_nxt = seq + 1; /* the SYN consumes one sequence number */
            t->snd_una = initial_seq();
            t->snd_nxt = t->snd_una;
            t->snd_window = ntohs(h->window);
            t->segments_in++;

            /* The SYN-ACK carries our MSS and consumes a sequence number of
             * its own, which is why snd_nxt advances past it. */
            if (send_segment(t, t->snd_nxt, TCP_SYN | TCP_ACK, NULL, 0, true)) {
                t->snd_nxt++;
                set_state(t, TCP_SYN_RECEIVED);
            } else {
                tcb_free(t);
            }
            return;
        }

        if (!port_listening(dport))
            s->tcp_no_port++;

        if (!(h->flags & TCP_RST))
            send_reset(src, h, seg_consumed);
        return;
    }

    t->segments_in++;

    if (h->flags & TCP_RST) {
        pr_debug("conn %u: reset from the peer", t->index);
        set_state(t, TCP_CLOSED);
        t->rcv_eof = true;
        maybe_reclaim(t);
        return;
    }

    switch (t->state) {
    case TCP_CLOSED:
    case TCP_LISTEN:
        return;

    case TCP_SYN_SENT:
        /* The other half of the handshake, and the half the old file never
         * had: this stack as the side that started it. */
        if (!(h->flags & TCP_SYN) || !(h->flags & TCP_ACK))
            return;
        if (ack != t->snd_nxt)
            return;

        t->rcv_nxt = seq + 1;
        t->snd_una = ack;
        t->snd_window = ntohs(h->window);
        set_state(t, TCP_ESTABLISHED);
        s->tcp_connections_initiated++;

        if (t->rtt_pending && seq_le(t->rtt_seq, ack)) {
            rtt_sample(t, (u32)(timer_ms() - t->rtt_start_ms));
            t->rtt_pending = false;
        }

        send_segment(t, t->snd_nxt, TCP_ACK, NULL, 0, false);
        send_queued(t);
        return;

    case TCP_SYN_RECEIVED:
        if (!(h->flags & TCP_ACK))
            return;

        /* The ACK has to acknowledge our SYN, not some other sequence. A
         * blind accept here is how a stack gets connections it never
         * established. */
        if (ack != t->snd_nxt)
            return;

        t->snd_una = ack;
        t->snd_window = ntohs(h->window);
        set_state(t, TCP_ESTABLISHED);
        s->tcp_connections_accepted++;

        /* Not claimable if the echo service owns it: there is nothing for an
         * accept() to take. */
        t->claimable = !t->echo;

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
        bool advanced = seq_gt(ack, t->snd_una) && seq_le(ack, t->snd_nxt);

        if (advanced) {
            u32 acked = ack - t->snd_una;
            u32 freed = acked;

            /* Release the acknowledged bytes from the front of the send
             * buffer. A FIN's sequence number is acknowledged too but has no
             * byte in the buffer, so the amount moved is bounded by what is
             * actually there. */
            if (freed > t->snd_len)
                freed = t->snd_len;

            memmove(t->sndbuf, t->sndbuf + freed, t->snd_len - freed);
            t->snd_len -= freed;
            t->snd_una = ack;
            t->retries = 0;
            t->dup_acks = 0;

            /* Close the round-trip sample if this acknowledgement reached
             * the segment being timed. */
            if (t->rtt_pending && seq_le(t->rtt_seq, ack)) {
                rtt_sample(t, (u32)(timer_ms() - t->rtt_start_ms));
                t->rtt_pending = false;
            }

            if (t->in_recovery) {
                /* Recovery ends when everything that was in flight when it
                 * began has been acknowledged. The window deflates to
                 * ssthresh: the inflation existed only to keep data moving
                 * through the repair. */
                if (seq_le(t->recover_seq, ack)) {
                    t->in_recovery = false;
                    t->cwnd = t->ssthresh;
                    t->cwnd_acked = 0;
                } else {
                    /* A partial acknowledgement during recovery: the hole
                     * moved but is not filled. Send the next missing
                     * segment rather than waiting for the timer. */
                    retransmit_one(t);
                }
            } else {
                cwnd_on_ack(t, acked);
            }
        } else if (ack == t->snd_una && !data_len && !(h->flags & TCP_FIN) &&
                   seq_gt(t->snd_nxt, t->snd_una)) {
            /* A duplicate acknowledgement: no new data acknowledged, nothing
             * carried, and we have something outstanding. Each one means a
             * segment *after* the hole arrived, which is why three of them
             * are evidence of loss rather than of a dead path. */
            t->dup_acks++;

            if (t->dup_acks == TCP_DUP_ACK_THRESH) {
                enter_fast_recovery(t);
            } else if (t->dup_acks > TCP_DUP_ACK_THRESH && t->in_recovery) {
                /* Keep inflating while the duplicates keep coming: each one
                 * is another segment that has left the network. */
                t->cwnd = umin(t->cwnd + TCP_MSS, TCP_SNDBUF + TCP_MSS);
            }
        }

        t->snd_window = ntohs(h->window);

        if (t->state == TCP_LAST_ACK && t->snd_una == t->snd_nxt) {
            /* Our FIN was acknowledged and so was theirs. Done. */
            set_state(t, TCP_TIME_WAIT);
            return;
        }

        if (t->state == TCP_FIN_WAIT_1 && t->fin_sent &&
            t->snd_una == t->snd_nxt) {
            /* Our FIN is acknowledged; the peer may still be sending. */
            set_state(t, TCP_FIN_WAIT_2);
        }
    }

    /* ---- the data ------------------------------------------------------- */
    if (data_len) {
        if (seq == t->rcv_nxt) {
            u32 taken = deliver(t, data, data_len);

            t->rcv_nxt += taken;
            t->bytes_in += taken;

            if (taken < data_len)
                pr_debug("conn %u: took %u of %u byte(s); window closed",
                         t->index, taken, data_len);
        } else if (seq_lt(seq, t->rcv_nxt)) {
            /* Already received. A duplicate, usually because our ACK was
             * lost - so re-ACK rather than ignore, which is what stops the
             * peer retransmitting forever. */
            send_segment(t, t->snd_nxt, TCP_ACK, NULL, 0, false);
            return;
        } else {
            /* Out of order. Dropped, and the expected sequence re-ACKed, so
             * the peer retransmits from the hole. Correct, and slow - see
             * the note at the top of the file. */
            t->out_of_order_dropped++;
            send_segment(t, t->snd_nxt, TCP_ACK, NULL, 0, false);
            return;
        }
    }

    /* ---- the peer's FIN ------------------------------------------------- */
    if ((h->flags & TCP_FIN) && seq_le(seq, t->rcv_nxt)) {
        /* The FIN consumes a sequence number after any data in the same
         * segment, which is why this is after the data handling. */
        t->rcv_nxt++;
        t->rcv_eof = true;

        switch (t->state) {
        case TCP_ESTABLISHED:
            set_state(t, TCP_CLOSE_WAIT);

            /* An echo connection has nothing more to say once the echo
             * drains, so its close is queued immediately. An owned
             * connection waits for its owner to close, which is the whole
             * reason CLOSE_WAIT is a state a program can observe. */
            if (t->echo || t->released) {
                t->fin_queued = true;
                set_state(t, TCP_LAST_ACK);
            }
            break;

        case TCP_FIN_WAIT_1:
        case TCP_FIN_WAIT_2:
            /* Both sides have closed. Our FIN may or may not be
             * acknowledged yet; TIME_WAIT covers both, which is a
             * simplification of RFC 793's CLOSING state and is safe because
             * TIME_WAIT also retransmits nothing. */
            set_state(t, TCP_TIME_WAIT);
            break;

        default:
            break;
        }
    }

    send_queued(t);

    /* An ACK for what we have received, if sending the queue did not already
     * carry one. A pure-ACK segment is cheap and leaving it out makes the
     * peer wait for its retransmission timer. */
    if (data_len || (h->flags & TCP_FIN))
        send_segment(t, t->snd_nxt, TCP_ACK, NULL, 0, false);
}

/* ---- the timer --------------------------------------------------------- */

static void tick_one(struct tcb *t, u64 now)
{
    switch (t->state) {
    case TCP_SYN_SENT:
    case TCP_SYN_RECEIVED:
        /* A half-open connection is not kept: the peer that sent the SYN may
         * never have existed, and holding a connection slot for it is how
         * one spoofed packet denies service to everyone else. This is the
         * small version of what SYN cookies solve. */
        if (now - t->state_since_ms > TCP_SYN_TIMEOUT_MS) {
            pr_debug("conn %u: half-open connection timed out", t->index);
            set_state(t, TCP_CLOSED);
            t->rcv_eof = true;
            maybe_reclaim(t);
            return;
        }
        break;

    case TCP_TIME_WAIT:
        if (now - t->state_since_ms > TCP_TIME_WAIT_MS) {
            set_state(t, TCP_CLOSED);
            maybe_reclaim(t);
        }
        return;

    case TCP_CLOSED:
        /* An owned connection that has finished but whose owner has not let
         * go yet. Kept, so the owner can still read what arrived - but not
         * forever, because an owner that never closes would otherwise hold
         * the slot for the life of the kernel. */
        if (now - t->state_since_ms > TCP_ORPHAN_MS)
            tcb_free(t);
        return;

    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
    case TCP_LAST_ACK:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
        break;

    case TCP_LISTEN:
        return;
    }

    /* Retransmission. Anything sent and not acknowledged goes again once the
     * timer expires, up to a bound - after which the connection is reset
     * rather than retried forever, because a peer that has not acknowledged
     * five times is gone. */
    bool unacked = seq_gt(t->snd_nxt, t->snd_una);

    if (!unacked || now - t->last_send_ms < t->rto_ms)
        return;

    if (t->retries >= TCP_MAX_RETRIES) {
        pr_warn("conn %u: no acknowledgement after %u retransmissions; "
                "resetting",
                t->index, t->retries);
        set_state(t, TCP_CLOSED);
        t->rcv_eof = true;
        maybe_reclaim(t);
        return;
    }

    t->retries++;
    t->retransmits++;
    on_timeout(t);

    if (t->state == TCP_SYN_SENT) {
        send_segment(t, t->snd_una, TCP_SYN, NULL, 0, true);
        return;
    }

    if (t->state == TCP_SYN_RECEIVED) {
        send_segment(t, t->snd_una, TCP_SYN | TCP_ACK, NULL, 0, true);
        return;
    }

    /* Rewind to the oldest unacknowledged byte and send from there. The FIN,
     * if it was sent, is re-sent by the same rewind because send_queued()
     * re-derives it from the queue state.
     *
     * Note that this is a full rewind, not the single-segment retransmit
     * that fast recovery does. After a timeout there is no evidence that
     * anything after the hole arrived, so there is nothing to preserve. */
    t->snd_nxt = t->snd_una;
    t->fin_sent = false;
    send_queued(t);

    if (t->snd_nxt == t->snd_una && !t->snd_len)
        send_segment(t, t->snd_nxt, TCP_ACK, NULL, 0, false);
}

static void tcp_tick_inner(void)
{
    u64 now = timer_ms();

    for (u32 i = 0; i < TCP_MAX_CONNS; i++)
        if (conns[i].used)
            tick_one(&conns[i], now);
}

/* ---- the echo service -------------------------------------------------- */

static void tcp_listen_inner(u16 port)
{
    listen_port = port;

    /* Drop any connection the old echo port still held. A listener that
     * changes port and leaves its connections behind is a listener whose
     * state and whose table disagree. */
    for (u32 i = 0; i < TCP_MAX_CONNS; i++)
        if (conns[i].used && conns[i].echo)
            tcb_free(&conns[i]);

    if (port)
        pr_info("listening on port %u; it echoes what it receives", port);
}

static u16 tcp_listen_port_inner(void)
{
    return listen_port;
}

/* ---- passive opens for the socket layer -------------------------------- */

static bool tcp_bind_listener_inner(u16 port)
{
    if (!port)
        return false;
    if (port_in_use(port))
        return false;

    for (u32 i = 0; i < TCP_LISTENERS_MAX; i++) {
        if (!listeners[i]) {
            listeners[i] = port;
            return true;
        }
    }

    return false;
}

static void tcp_unbind_listener_inner(u16 port)
{
    for (u32 i = 0; i < TCP_LISTENERS_MAX; i++)
        if (listeners[i] == port)
            listeners[i] = 0;
}

static int tcp_accept_inner(u16 port)
{
    for (u32 i = 0; i < TCP_MAX_CONNS; i++) {
        struct tcb *t = &conns[i];

        if (!t->used || !t->claimable || t->local_port != port)
            continue;

        /* Claimed: it is now the caller's, and a second accept must not get
         * the same connection. */
        t->claimable = false;
        return handle_of(t);
    }

    return SYS_EAGAIN;
}

/* ---- active opens ------------------------------------------------------- */

static int tcp_connect_inner(ipv4_addr dst, u16 port, u16 local_port)
{
    if (!port)
        return SYS_EINVAL;

    /* No link means no amount of retransmission will help, and an immediate
     * answer is worth more than ten seconds in SYN_SENT. */
    if (!net_up())
        return SYS_EHOSTUNREACH;

    if (!local_port) {
        local_port = tcp_ephemeral_port();
        if (!local_port)
            return SYS_EAGAIN;
    }

    struct tcb *t = tcb_alloc();

    if (!t)
        return SYS_ENOSPC;

    t->remote = dst;
    t->remote_port = port;
    t->local_port = local_port;
    t->echo = false;
    t->snd_una = initial_seq();
    t->snd_nxt = t->snd_una;
    /* Nothing is known about the peer's window until it answers, so start
     * with one segment's worth: enough to send the SYN and no more. The real
     * value arrives on the SYN-ACK. */
    t->snd_window = TCP_MSS;

    /* The SYN is sent on a best-effort basis and the connection enters
     * SYN_SENT either way.
     *
     * This is not sloppiness, it is the only thing that works. The first SYN
     * to a host whose hardware address is not yet known cannot be sent at
     * all: ipv4_output_frame() queues an ARP request and drops the frame,
     * which is correct, and means the very first connection to any host
     * always fails to transmit its first segment. Reporting that as a failed
     * connect made `fetch` fail on every cold ARP cache and work on the
     * second try, which is the shape of a bug that looks like a flaky
     * network.
     *
     * The retransmission timer is what sends it, one RTO later, by which
     * time the ARP reply has arrived. A stack with a retransmission timer
     * does not need its first transmission to succeed, and designing as if
     * it does is how the timer comes to have an untested path. */
    bool sent = send_segment(t, t->snd_nxt, TCP_SYN, NULL, 0, true);

    if (!sent)
        pr_debug("the first SYN could not be sent (no ARP entry yet); the "
                 "retransmission will carry it");

    t->snd_nxt++;
    t->rtt_pending = true;
    t->rtt_seq = t->snd_nxt;
    t->rtt_start_ms = timer_ms();
    set_state(t, TCP_SYN_SENT);

    {
        char ib[IPV4_STR_LEN];

        pr_info("connecting to %s:%u from port %u",
                ipv4_str(dst, ib, sizeof(ib)), port, local_port);
    }

    return handle_of(t);
}

/* ---- the connection API ------------------------------------------------- */

static enum tcp_state tcp_handle_state_inner(int handle)
{
    struct tcb *t = tcb_of(handle);

    return t ? t->state : TCP_CLOSED;
}

static i32 tcp_send_inner(int handle, const void *buf, u32 len)
{
    struct tcb *t = tcb_of(handle);

    if (!t)
        return SYS_EBADF;
    if (t->state == TCP_SYN_SENT)
        return SYS_EAGAIN; /* the handshake has not finished */
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT)
        return SYS_EPIPE;
    if (t->fin_queued)
        return SYS_EPIPE; /* this side is already closing */

    u32 taken = queue_send(t, buf, len);

    send_queued(t);

    /* Zero accepted with something asked for is back pressure, not an end of
     * stream, and the two have to be distinguishable. */
    if (!taken && len)
        return SYS_EAGAIN;

    return (i32)taken;
}

static i32 tcp_recv_inner(int handle, void *buf, u32 len)
{
    struct tcb *t = tcb_of(handle);

    if (!t)
        return SYS_EBADF;

    u32 take = umin(len, t->rcv_len);

    if (!take) {
        if (t->rcv_eof || t->state == TCP_CLOSED)
            return 0; /* end of stream */
        return SYS_EAGAIN;
    }

    memcpy(buf, t->rcvbuf, take);
    memmove(t->rcvbuf, t->rcvbuf + take, t->rcv_len - take);
    t->rcv_len -= take;

    /* Taking bytes out of the buffer opens the window again, and the peer
     * only learns that from a segment. Without this, a connection whose
     * window ever reached zero would stay stalled until the peer's own
     * persist timer poked it. */
    if (t->state == TCP_ESTABLISHED || t->state == TCP_FIN_WAIT_1 ||
        t->state == TCP_FIN_WAIT_2)
        send_segment(t, t->snd_nxt, TCP_ACK, NULL, 0, false);

    return (i32)take;
}

static u32 tcp_recv_available_inner(int handle)
{
    struct tcb *t = tcb_of(handle);

    return t ? t->rcv_len : 0;
}

static bool tcp_recv_eof_inner(int handle)
{
    struct tcb *t = tcb_of(handle);

    if (!t)
        return true;

    return t->rcv_len == 0 && (t->rcv_eof || t->state == TCP_CLOSED);
}

static u32 tcp_send_room_inner(int handle)
{
    struct tcb *t = tcb_of(handle);

    if (!t || t->fin_queued)
        return 0;
    if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT)
        return 0;

    return TCP_SNDBUF - t->snd_len;
}

static void tcp_shutdown_inner(int handle)
{
    struct tcb *t = tcb_of(handle);

    if (!t || t->fin_queued)
        return;

    switch (t->state) {
    case TCP_ESTABLISHED:
        t->fin_queued = true;
        send_queued(t);
        set_state(t, TCP_FIN_WAIT_1);
        break;

    case TCP_CLOSE_WAIT:
        t->fin_queued = true;
        send_queued(t);
        set_state(t, TCP_LAST_ACK);
        break;

    default:
        break;
    }
}

static void tcp_close_inner(int handle)
{
    struct tcb *t = tcb_of(handle);

    if (!t)
        return;

    t->released = true;
    t->claimable = false;

    if (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT) {
        tcp_shutdown(handle);
        return;
    }

    if (t->state == TCP_CLOSED)
        tcb_free(t);
}

static void tcp_abort_inner(int handle)
{
    struct tcb *t = tcb_of(handle);

    if (!t)
        return;

    /* A reset rather than a FIN: no round trip, no TIME_WAIT, and the peer
     * is told the connection is gone rather than that it ended tidily. */
    if (t->state != TCP_CLOSED && t->state != TCP_TIME_WAIT &&
        t->state != TCP_SYN_SENT) {
        send_segment(t, t->snd_nxt, TCP_RST | TCP_ACK, NULL, 0, false);
        net_stats()->tcp_resets_sent++;
    }

    tcb_free(t);
}

/* ---- status ------------------------------------------------------------- */

static void fill_status(const struct tcb *t, struct tcp_status *out)
{
    out->state = t->state;
    out->local_port = t->local_port;
    out->remote_port = t->remote_port;
    out->remote = t->remote;
    out->snd_nxt = t->snd_nxt;
    out->snd_una = t->snd_una;
    out->rcv_nxt = t->rcv_nxt;
    out->bytes_in = t->bytes_in;
    out->bytes_out = t->bytes_out;
    out->segments_in = t->segments_in;
    out->segments_out = t->segments_out;
    out->retransmits = t->retransmits;
    out->out_of_order_dropped = t->out_of_order_dropped;
    out->cwnd = t->cwnd;
    out->ssthresh = t->ssthresh;
    out->srtt_ms = t->srtt_ms;
    out->rttvar_ms = t->rttvar_ms;
    out->rto_ms = t->rto_ms;
    out->dup_acks = t->dup_acks;
    out->fast_retransmits = t->fast_retransmits;
    out->timeouts = t->timeouts;
    out->in_recovery = t->in_recovery;
}

static bool tcp_get_status_at_inner(int handle, struct tcp_status *out)
{
    struct tcb *t = tcb_of(handle);

    if (!t)
        return false;

    fill_status(t, out);
    return true;
}

static void tcp_get_status_inner(struct tcp_status *out)
{
    /* The first live connection, which is what the shell reports. When there
     * is none, the answer is the listener's: LISTEN on the echo port, or
     * CLOSED if nothing is listening. That is synthesised here rather than
     * held in a connection, because a listener is no longer a connection -
     * it is an entry in a table of ports, which is what lets several
     * connections share one. */
    for (u32 i = 0; i < TCP_MAX_CONNS; i++) {
        if (conns[i].used && conns[i].state != TCP_CLOSED) {
            fill_status(&conns[i], out);
            return;
        }
    }

    memset(out, 0, sizeof(*out));
    out->state = listen_port ? TCP_LISTEN : TCP_CLOSED;
    out->local_port = listen_port;
    out->cwnd = TCP_INIT_CWND;
    out->ssthresh = TCP_INIT_SSTHRESH;
    out->rto_ms = TCP_RTO_INITIAL_MS;
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

u16 tcp_ephemeral_port(void)
{
    net_lock();
    u16 ret = tcp_ephemeral_port_inner();

    net_unlock();
    return ret;
}

void tcp_input(ipv4_addr src, const u8 *packet, size_t len)
{
    net_lock();
    tcp_input_inner(src, packet, len);
    net_unlock();
}

void tcp_tick(void)
{
    net_lock();
    tcp_tick_inner();
    net_unlock();
}

void tcp_listen(u16 port)
{
    net_lock();
    tcp_listen_inner(port);
    net_unlock();
}

u16 tcp_listen_port(void)
{
    net_lock();
    u16 ret = tcp_listen_port_inner();

    net_unlock();
    return ret;
}

bool tcp_bind_listener(u16 port)
{
    net_lock();
    bool ret = tcp_bind_listener_inner(port);

    net_unlock();
    return ret;
}

void tcp_unbind_listener(u16 port)
{
    net_lock();
    tcp_unbind_listener_inner(port);
    net_unlock();
}

int tcp_accept(u16 port)
{
    net_lock();
    int ret = tcp_accept_inner(port);

    net_unlock();
    return ret;
}

int tcp_connect(ipv4_addr dst, u16 port, u16 local_port)
{
    net_lock();
    int ret = tcp_connect_inner(dst, port, local_port);

    net_unlock();
    return ret;
}

enum tcp_state tcp_handle_state(int handle)
{
    net_lock();
    enum tcp_state ret = tcp_handle_state_inner(handle);

    net_unlock();
    return ret;
}

i32 tcp_send(int handle, const void *buf, u32 len)
{
    net_lock();
    i32 ret = tcp_send_inner(handle, buf, len);

    net_unlock();
    return ret;
}

i32 tcp_recv(int handle, void *buf, u32 len)
{
    net_lock();
    i32 ret = tcp_recv_inner(handle, buf, len);

    net_unlock();
    return ret;
}

u32 tcp_recv_available(int handle)
{
    net_lock();
    u32 ret = tcp_recv_available_inner(handle);

    net_unlock();
    return ret;
}

bool tcp_recv_eof(int handle)
{
    net_lock();
    bool ret = tcp_recv_eof_inner(handle);

    net_unlock();
    return ret;
}

u32 tcp_send_room(int handle)
{
    net_lock();
    u32 ret = tcp_send_room_inner(handle);

    net_unlock();
    return ret;
}

void tcp_shutdown(int handle)
{
    net_lock();
    tcp_shutdown_inner(handle);
    net_unlock();
}

void tcp_close(int handle)
{
    net_lock();
    tcp_close_inner(handle);
    net_unlock();
}

void tcp_abort(int handle)
{
    net_lock();
    tcp_abort_inner(handle);
    net_unlock();
}

bool tcp_get_status_at(int handle, struct tcp_status *out)
{
    net_lock();
    bool ret = tcp_get_status_at_inner(handle, out);

    net_unlock();
    return ret;
}

void tcp_get_status(struct tcp_status *out)
{
    net_lock();
    tcp_get_status_inner(out);
    net_unlock();
}
