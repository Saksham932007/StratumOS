/* StratumOS - sockets over the VFS. See net/socket.h for why this is short. */
#define LOG_TAG "sock"

#include <drivers/timer.h>

#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/syscall_abi.h>

#include <fs/vfs.h>
#include <net/inet.h>
#include <net/net.h>
#include <net/socket.h>

/* How long a blocking call waits before giving up, when no timeout has been
 * set with SO_RCVTIMEO. Not infinite: a ring-3 program that blocks forever on
 * a socket nobody will ever write to is indistinguishable from a hung
 * kernel, and this kernel has no way to interrupt it. */
#define SOCK_DEFAULT_TIMEOUT_MS 10000
/* How long to sleep between polls while blocked. A timer tick, which is the
 * resolution the scheduler has - see the note in socket.h on why this is
 * polling and not a wait queue. */
#define SOCK_POLL_MS            2

struct socket {
    bool used;
    u8 type; /* SOCK_STREAM or SOCK_DGRAM            */

    /* Stream state: a handle into TCP's connection table, or -1. */
    int tcp_handle;
    bool listening;

    /* Datagram state: the bound local port, and the peer connect() named. */
    u16 udp_port;
    ipv4_addr peer_addr;
    u16 peer_port;
    bool connected; /* a default peer has been set              */

    u16 local_port; /* for a stream socket: what bind() claimed  */
    bool bound;

    u32 rcv_timeout_ms;
    u32 snd_timeout_ms;
    i32 last_error; /* what SO_ERROR reports                    */
};

static struct socket sockets[SOCKET_MAX];
static struct socket_stats stats;

/* ---- the pool ----------------------------------------------------------- */

static struct socket *sock_alloc(void)
{
    for (u32 i = 0; i < SOCKET_MAX; i++) {
        if (!sockets[i].used) {
            memset(&sockets[i], 0, sizeof(sockets[i]));
            sockets[i].used = true;
            sockets[i].tcp_handle = -1;
            sockets[i].rcv_timeout_ms = SOCK_DEFAULT_TIMEOUT_MS;
            sockets[i].snd_timeout_ms = SOCK_DEFAULT_TIMEOUT_MS;
            return &sockets[i];
        }
    }

    return NULL;
}

static void sock_release(struct socket *s)
{
    if (s->type == SOCK_STREAM) {
        if (s->listening && s->local_port)
            tcp_unbind_listener(s->local_port);
        if (s->tcp_handle >= 0)
            tcp_close(s->tcp_handle);
    } else if (s->udp_port) {
        udp_unbind(s->udp_port);
    }

    memset(s, 0, sizeof(*s));
    stats.closed++;
}

u32 socket_count(void)
{
    u32 n = 0;

    for (u32 i = 0; i < SOCKET_MAX; i++)
        if (sockets[i].used)
            n++;

    return n;
}

const struct socket_stats *socket_get_stats(void)
{
    return &stats;
}

/* ---- the vfs_ops -------------------------------------------------------
 *
 * This is the part that makes a socket a file. `read` and `write` are the
 * same calls a program uses on the console, which is what lets a program
 * written against descriptors work on a socket without knowing it is one.
 */

static struct socket *sock_of(struct vfs_file *f)
{
    return (struct socket *)f->priv;
}

/* Wait until a predicate holds, polling the network. Returns true if it
 * became true, false on the deadline.
 *
 * This is where the absence of a wait queue is paid for, and it is paid in
 * one place rather than in every call. net_poll() is called directly because
 * the network thread might not be scheduled before this task's timeslice
 * ends, and a receive that depends on another thread running is a receive
 * that behaves differently under load. */
static bool wait_for(bool (*ready)(struct socket *), struct socket *s,
                     u32 timeout_ms)
{
    if (ready(s))
        return true;

    stats.blocking_waits++;

    u64 deadline = timer_ms() + timeout_ms;

    while (timer_ms() < deadline) {
        net_poll(16);

        if (ready(s))
            return true;

        task_sleep_ms(SOCK_POLL_MS);
    }

    return ready(s);
}

static bool stream_readable(struct socket *s)
{
    if (s->tcp_handle < 0)
        return true; /* nothing to wait for; the caller gets its error */

    return tcp_recv_available(s->tcp_handle) > 0 || tcp_recv_eof(s->tcp_handle);
}

static bool stream_writable(struct socket *s)
{
    if (s->tcp_handle < 0)
        return true;

    enum tcp_state st = tcp_handle_state(s->tcp_handle);

    if (st != TCP_ESTABLISHED && st != TCP_CLOSE_WAIT)
        return true; /* not going to become writable; report the error */

    return tcp_send_room(s->tcp_handle) > 0;
}

static bool dgram_readable(struct socket *s)
{
    return !s->udp_port || udp_queued(s->udp_port) > 0;
}

static bool stream_connected(struct socket *s)
{
    return s->tcp_handle < 0 || tcp_handle_state(s->tcp_handle) != TCP_SYN_SENT;
}

static bool stream_has_peer(struct socket *s)
{
    return !s->listening || !s->local_port ||
           tcp_accept(s->local_port) != SYS_EAGAIN;
}

/* Defined below, next to recvfrom, because the two have to agree. */
static i32 dgram_recv(struct socket *s, u32 flags, void *buf, u32 len,
                      struct sys_sockaddr *from);

static i32 sock_read(struct vfs_file *f, void *buf, u32 len)
{
    struct socket *s = sock_of(f);

    if (!s)
        return SYS_EBADF;

    if (s->type == SOCK_STREAM) {
        if (s->tcp_handle < 0)
            return SYS_ENOTCONN;

        i32 n = tcp_recv(s->tcp_handle, buf, len);

        if (n != SYS_EAGAIN)
            goto done;

        if (f->flags & O_NONBLOCK)
            return SYS_EAGAIN;

        if (!wait_for(stream_readable, s, s->rcv_timeout_ms))
            return SYS_ETIMEDOUT;

        n = tcp_recv(s->tcp_handle, buf, len);
        if (n == SYS_EAGAIN)
            return SYS_ETIMEDOUT;

    done:
        if (n > 0)
            stats.bytes_received += (u32)n;
        return n;
    }

    /* A datagram socket. `read` is `recvfrom` with the sender's address
     * thrown away, and is written as exactly that so the two cannot
     * diverge. */
    return dgram_recv(s, f->flags, buf, len, NULL);
}

/* A datagram read, split out because `read` on a datagram socket discards
 * the sender's address and `recvfrom` keeps it, and the two must not diverge
 * in anything else. */
static i32 dgram_recv(struct socket *s, u32 flags, void *buf, u32 len,
                      struct sys_sockaddr *from)
{
    if (!s->udp_port)
        return SYS_ENOTCONN;

    /* One datagram per call, and a short buffer truncates it rather than
     * splitting it across two calls. That is what a datagram socket does
     * everywhere: a datagram is a message, and half a message delivered as
     * if it were whole would be worse than a visible truncation. */
    static struct udp_dgram d;

    i32 n = udp_recv(s->udp_port, &d);

    if (n < 0)
        return n;

    if (n == 0) {
        if (flags & O_NONBLOCK)
            return SYS_EAGAIN;

        if (!wait_for(dgram_readable, s, s->rcv_timeout_ms))
            return SYS_ETIMEDOUT;

        n = udp_recv(s->udp_port, &d);
        if (n <= 0)
            return n < 0 ? n : SYS_ETIMEDOUT;
    }

    u32 take = (u32)n < len ? (u32)n : len;

    memcpy(buf, d.data, take);

    if (from) {
        memset(from, 0, sizeof(*from));
        from->family = AF_INET;
        from->port = htons(d.src_port);
        from->addr = d.src;
    }

    stats.bytes_received += take;
    return (i32)take;
}

static i32 sock_write(struct vfs_file *f, const void *buf, u32 len)
{
    struct socket *s = sock_of(f);

    if (!s)
        return SYS_EBADF;

    if (s->type == SOCK_DGRAM) {
        if (!s->connected)
            return SYS_ENOTCONN;
        if (!udp_send(s->peer_addr, s->udp_port, s->peer_port, buf, len))
            return SYS_EIO;
        stats.bytes_sent += len;
        return (i32)len;
    }

    if (s->tcp_handle < 0)
        return SYS_ENOTCONN;

    i32 n = tcp_send(s->tcp_handle, buf, len);

    if (n == SYS_EAGAIN && !(f->flags & O_NONBLOCK)) {
        if (!wait_for(stream_writable, s, s->snd_timeout_ms))
            return SYS_ETIMEDOUT;

        n = tcp_send(s->tcp_handle, buf, len);
        if (n == SYS_EAGAIN)
            return SYS_ETIMEDOUT;
    }

    if (n > 0)
        stats.bytes_sent += (u32)n;

    return n;
}

static i32 sock_stat(struct vfs_file *f, struct sys_stat *out)
{
    struct socket *s = sock_of(f);

    memset(out, 0, sizeof(*out));
    out->type = S_TYPE_SOCKET;
    out->mode = S_MODE_READ | S_MODE_WRITE;
    /* `size` is how much could be read now. A socket has no length, and
     * reporting the readable count is more use than reporting zero. */
    if (s && s->type == SOCK_STREAM && s->tcp_handle >= 0)
        out->size = tcp_recv_available(s->tcp_handle);
    else if (s && s->type == SOCK_DGRAM && s->udp_port)
        out->size = udp_queued(s->udp_port);

    return 0;
}

static i32 sock_ioctl(struct vfs_file *f, u32 request, u32 arg)
{
    struct socket *s = sock_of(f);

    if (!s)
        return SYS_EBADF;

    if (request == FIONBIO) {
        /* The flag lives on the open file, not on the socket, because that
         * is where every other blocking decision in this kernel reads it
         * from - the device filesystem checks the same bit. A socket that
         * kept its own copy would be a second source of truth for one
         * question. */
        if (arg)
            f->flags |= O_NONBLOCK;
        else
            f->flags &= ~(u32)O_NONBLOCK;
        return 0;
    }

    /* The same request the console answers, meaning the same thing: how much
     * could be read without blocking. A program that asks this of stdin and
     * of a socket should not need two calls. */
    if (request == TIOCNREAD) {
        if (s->type == SOCK_STREAM)
            return s->tcp_handle < 0 ? 0
                                     : (i32)tcp_recv_available(s->tcp_handle);
        return s->udp_port ? (i32)udp_queued(s->udp_port) : 0;
    }

    return SYS_ENOTSUP;
}

static bool sock_poll_read(struct vfs_file *f)
{
    struct socket *s = sock_of(f);

    if (!s)
        return false;
    if (s->listening)
        return stream_has_peer(s);
    if (s->type == SOCK_STREAM)
        return s->tcp_handle >= 0 && stream_readable(s);

    return dgram_readable(s);
}

static bool sock_poll_write(struct vfs_file *f)
{
    struct socket *s = sock_of(f);

    if (!s)
        return false;
    if (s->type == SOCK_DGRAM)
        return true;

    return s->tcp_handle >= 0 && stream_writable(s);
}

static void sock_close(struct vfs_file *f)
{
    struct socket *s = sock_of(f);

    if (s)
        sock_release(s);

    f->priv = NULL;
}

static const struct vfs_ops socket_ops = {
    .name = "socket",
    .read = sock_read,
    .write = sock_write,
    /* No seek, no getdent, no truncate. A NULL here is the answer, not an
     * oversight: a stream has no position, so the VFS returns EINVAL once
     * rather than this file saying so three times. See fs/vfs.h. */
    .stat = sock_stat,
    .ioctl = sock_ioctl,
    .close = sock_close,
    .poll_read = sock_poll_read,
    .poll_write = sock_poll_write,
};

/* ---- the calls ---------------------------------------------------------- */

void socket_init(void)
{
    memset(sockets, 0, sizeof(sockets));
    memset(&stats, 0, sizeof(stats));
}

static struct socket *sock_from_fd(i32 fd, i32 *err)
{
    struct vfs_file *f = fd_get(fd);

    if (!f) {
        *err = SYS_EBADF;
        return NULL;
    }

    /* The type check, not merely a NULL check: bind() on an open text file
     * has to fail with "that is not a socket" rather than corrupt whatever
     * `priv` happened to point at. */
    if (f->ops != &socket_ops || f->type != S_TYPE_SOCKET) {
        *err = SYS_ENOTSUP;
        return NULL;
    }

    struct socket *s = sock_of(f);

    if (!s) {
        *err = SYS_EBADF;
        return NULL;
    }

    *err = 0;
    return s;
}

i32 socket_create(int domain, int type, int protocol)
{
    if (domain != AF_INET)
        return SYS_ENOTSUP;
    if (type != SOCK_STREAM && type != SOCK_DGRAM)
        return SYS_ENOTSUP;

    /* A protocol of 0 means "the usual one for this type", which is what
     * every caller passes and what Berkeley sockets define. Naming the
     * mismatched one explicitly is an error rather than a hint. */
    if (protocol) {
        if (type == SOCK_STREAM && protocol != IPPROTO_TCP)
            return SYS_ENOTSUP;
        if (type == SOCK_DGRAM && protocol != IPPROTO_UDP)
            return SYS_ENOTSUP;
    }

    struct socket *s = sock_alloc();

    if (!s)
        return SYS_ENFILE;

    s->type = (u8)type;

    struct vfs_file *f =
        vfs_alloc(&socket_ops, S_TYPE_SOCKET, O_RDWR, s,
                  type == SOCK_STREAM ? "<tcp socket>" : "<udp socket>");

    if (!f) {
        sock_release(s);
        return SYS_ENFILE;
    }

    i32 fd = fd_install(f);

    if (fd < 0) {
        /* fd_install() does not consume the reference on failure, so the
         * open file is still ours to drop - and dropping it runs sock_close,
         * which releases the socket. One cleanup path, not two. */
        vfs_put(f);
        return fd;
    }

    stats.created++;
    return fd;
}

i32 socket_bind(i32 fd, const struct sys_sockaddr *addr)
{
    i32 err;
    struct socket *s = sock_from_fd(fd, &err);

    if (!s)
        return err;

    if (addr->family != AF_INET)
        return SYS_ENOTSUP;

    u16 port = ntohs(addr->port);

    if (!port)
        return SYS_EINVAL;

    /* The address is not checked against this machine's: there is one
     * interface, so every local address is the same address, and refusing a
     * bind to 0.0.0.0 would refuse the only thing anyone writes. */

    if (s->type == SOCK_DGRAM) {
        if (s->udp_port)
            return SYS_EINVAL; /* already bound */
        if (!udp_bind(port))
            return SYS_EADDRINUSE;
        s->udp_port = port;
    } else {
        if (s->bound)
            return SYS_EINVAL;
        s->local_port = port;
    }

    s->bound = true;
    stats.binds++;
    return 0;
}

i32 socket_listen(i32 fd, int backlog)
{
    i32 err;
    struct socket *s = sock_from_fd(fd, &err);

    if (!s)
        return err;

    if (s->type != SOCK_STREAM)
        return SYS_ENOTSUP;
    if (!s->bound || !s->local_port)
        return SYS_EINVAL;
    if (s->listening)
        return 0;

    /* The backlog is accepted and ignored, which is worth saying rather than
     * pretending. TCP's connection table is the real queue: a SYN that
     * arrives with no free connection gets a reset, and that limit is
     * TCP_MAX_CONNS, not this number. A backlog that pretended to be
     * independent of the table would be a number with no effect. */
    UNUSED(backlog);

    if (!tcp_bind_listener(s->local_port))
        return SYS_EADDRINUSE;

    s->listening = true;
    return 0;
}

i32 socket_accept(i32 fd, struct sys_sockaddr *peer)
{
    i32 err;
    struct socket *s = sock_from_fd(fd, &err);

    if (!s)
        return err;

    if (!s->listening)
        return SYS_EINVAL;

    struct vfs_file *lf = fd_get(fd);
    int h = tcp_accept(s->local_port);

    if (h == SYS_EAGAIN) {
        if (lf && (lf->flags & O_NONBLOCK))
            return SYS_EAGAIN;

        /* Blocking accept. stream_has_peer() calls tcp_accept() to test, and
         * a successful test *claims* the connection - so the handle it
         * returned must not be thrown away. That is why the wait is written
         * as a poll of tcp_accept() here rather than through wait_for(). */
        stats.blocking_waits++;

        u64 deadline = timer_ms() + s->rcv_timeout_ms;

        while (h == SYS_EAGAIN && timer_ms() < deadline) {
            net_poll(16);
            h = tcp_accept(s->local_port);
            if (h != SYS_EAGAIN)
                break;
            task_sleep_ms(SOCK_POLL_MS);
        }
    }

    if (h < 0)
        return h == SYS_EAGAIN ? SYS_ETIMEDOUT : h;

    /* The accepted connection is a new socket and a new descriptor. The
     * listening one keeps listening, which is the whole reason TCP needed a
     * table of connections before this call could exist. */
    struct socket *ns = sock_alloc();

    if (!ns) {
        tcp_abort(h);
        return SYS_ENFILE;
    }

    ns->type = SOCK_STREAM;
    ns->tcp_handle = h;
    ns->local_port = s->local_port;
    ns->bound = true;
    ns->connected = true;

    struct vfs_file *f =
        vfs_alloc(&socket_ops, S_TYPE_SOCKET, O_RDWR, ns, "<tcp accepted>");

    if (!f) {
        sock_release(ns);
        return SYS_ENFILE;
    }

    i32 nfd = fd_install(f);

    if (nfd < 0) {
        vfs_put(f);
        return nfd;
    }

    struct tcp_status ts;

    if (peer) {
        memset(peer, 0, sizeof(*peer));
        peer->family = AF_INET;
        if (tcp_get_status_at(h, &ts)) {
            peer->addr = ts.remote;
            peer->port = htons(ts.remote_port);
            ns->peer_addr = ts.remote;
            ns->peer_port = ts.remote_port;
        }
    }

    stats.accepts++;
    stats.created++;
    return nfd;
}

i32 socket_connect(i32 fd, const struct sys_sockaddr *addr)
{
    i32 err;
    struct socket *s = sock_from_fd(fd, &err);

    if (!s)
        return err;

    if (addr->family != AF_INET)
        return SYS_ENOTSUP;

    u16 port = ntohs(addr->port);

    if (!port)
        return SYS_EINVAL;

    if (s->type == SOCK_DGRAM) {
        /* A datagram connect sets a default peer and sends nothing, which is
         * what Berkeley sockets do: it is what makes send() and recv() work
         * on a datagram socket, and it is why it cannot fail for any reason
         * to do with the network. */
        if (!s->udp_port) {
            s->udp_port = udp_bind_ephemeral();
            if (!s->udp_port)
                return SYS_EADDRINUSE;
        }

        s->peer_addr = addr->addr;
        s->peer_port = port;
        s->connected = true;
        stats.connects++;
        return 0;
    }

    if (s->tcp_handle >= 0)
        return SYS_EISCONN;
    if (s->listening)
        return SYS_EINVAL;

    int h = tcp_connect(addr->addr, port, s->local_port);

    if (h < 0) {
        s->last_error = h;
        return h;
    }

    s->tcp_handle = h;
    stats.connects++;

    struct vfs_file *f = fd_get(fd);

    if (f && (f->flags & O_NONBLOCK)) {
        /* Non-blocking connect reports "in progress" as EAGAIN, and the
         * caller finds out it succeeded by the socket becoming writable.
         * EINPROGRESS would be the Berkeley answer; there is no such code in
         * this ABI and inventing one for a single call is worse than reusing
         * the one that already means "not yet". */
        return SYS_EAGAIN;
    }

    if (!wait_for(stream_connected, s, s->snd_timeout_ms)) {
        tcp_abort(h);
        s->tcp_handle = -1;
        s->last_error = SYS_ETIMEDOUT;
        return SYS_ETIMEDOUT;
    }

    enum tcp_state st = tcp_handle_state(h);

    if (st != TCP_ESTABLISHED) {
        /* CLOSED after SYN_SENT means a reset came back, which is a refusal
         * and says the host is there. Anything else is a timeout. */
        i32 rc = st == TCP_CLOSED ? SYS_ECONNREFUSED : SYS_ETIMEDOUT;

        tcp_abort(h);
        s->tcp_handle = -1;
        s->last_error = rc;
        return rc;
    }

    s->connected = true;
    s->peer_addr = addr->addr;
    s->peer_port = port;
    return 0;
}

i32 socket_send(i32 fd, const void *buf, u32 len, u32 flags)
{
    UNUSED(flags);

    struct vfs_file *f = fd_get(fd);

    if (!f)
        return SYS_EBADF;
    if (f->ops != &socket_ops)
        return SYS_ENOTSUP;

    return vfs_write(f, buf, len);
}

i32 socket_recv(i32 fd, void *buf, u32 len, u32 flags)
{
    i32 err;
    struct socket *s = sock_from_fd(fd, &err);

    if (!s)
        return err;

    struct vfs_file *f = fd_get(fd);

    if (s->type == SOCK_DGRAM)
        return dgram_recv(s, f ? f->flags : 0, buf, len, NULL);

    UNUSED(flags);
    return vfs_read(f, buf, len);
}

i32 socket_sendto(i32 fd, const void *buf, u32 len,
                  const struct sys_sockaddr *to)
{
    i32 err;
    struct socket *s = sock_from_fd(fd, &err);

    if (!s)
        return err;

    if (s->type != SOCK_DGRAM)
        return SYS_ENOTSUP;
    if (!to || to->family != AF_INET)
        return SYS_EINVAL;

    if (!s->udp_port) {
        /* An unbound datagram socket gets a source port on its first send,
         * which is what lets a client skip bind() entirely. */
        s->udp_port = udp_bind_ephemeral();
        if (!s->udp_port)
            return SYS_EADDRINUSE;
    }

    if (!udp_send(to->addr, s->udp_port, ntohs(to->port), buf, len))
        return SYS_EIO;

    stats.bytes_sent += len;
    return (i32)len;
}

i32 socket_recvfrom(i32 fd, void *buf, u32 len, struct sys_sockaddr *from)
{
    i32 err;
    struct socket *s = sock_from_fd(fd, &err);

    if (!s)
        return err;

    if (s->type != SOCK_DGRAM)
        return SYS_ENOTSUP;

    struct vfs_file *f = fd_get(fd);

    return dgram_recv(s, f ? f->flags : 0, buf, len, from);
}

i32 socket_shutdown(i32 fd, int how)
{
    i32 err;
    struct socket *s = sock_from_fd(fd, &err);

    if (!s)
        return err;

    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
        return SYS_EINVAL;

    if (s->type != SOCK_STREAM)
        return SYS_ENOTSUP;
    if (s->tcp_handle < 0)
        return SYS_ENOTCONN;

    /* SHUT_RD is accepted and does nothing, which is honest: there is no
     * per-direction flag on a connection here, and a receive shutdown that
     * silently kept delivering data would be worse than one that says it did
     * nothing. SHUT_WR is the one that has an effect on the wire - it sends
     * the FIN - and SHUT_RDWR is the same thing. */
    if (how != SHUT_RD)
        tcp_shutdown(s->tcp_handle);

    return 0;
}

i32 socket_setsockopt(i32 fd, int level, int name, u32 value)
{
    i32 err;
    struct socket *s = sock_from_fd(fd, &err);

    if (!s)
        return err;

    if (level != SOL_SOCKET)
        return SYS_ENOTSUP;

    switch (name) {
    case SO_RCVTIMEO:
        /* Bounded, for the same reason sleep() is: a timeout long enough to
         * look like a hang is a timeout a ring-3 program must not be able to
         * ask for. Zero means the default rather than "never". */
        s->rcv_timeout_ms =
            value ? MIN(value, 60000u) : SOCK_DEFAULT_TIMEOUT_MS;
        return 0;
    case SO_SNDTIMEO:
        s->snd_timeout_ms =
            value ? MIN(value, 60000u) : SOCK_DEFAULT_TIMEOUT_MS;
        return 0;
    case SO_REUSEADDR:
        /* Accepted and ignored. There is no TIME_WAIT bind conflict to work
         * around, because a listener lives in its own table and is not held
         * by a closing connection. */
        return 0;
    default:
        return SYS_ENOTSUP;
    }
}

i32 socket_getsockopt(i32 fd, int level, int name, u32 *out)
{
    i32 err;
    struct socket *s = sock_from_fd(fd, &err);

    if (!s)
        return err;

    if (level != SOL_SOCKET)
        return SYS_ENOTSUP;

    switch (name) {
    case SO_RCVTIMEO:
        *out = s->rcv_timeout_ms;
        return 0;
    case SO_SNDTIMEO:
        *out = s->snd_timeout_ms;
        return 0;
    case SO_ERROR:
        /* Read-and-clear, as Berkeley sockets define it: the error belongs
         * to the operation that failed, and leaving it set would make the
         * next getsockopt report a failure that had already been read. */
        *out = (u32)s->last_error;
        s->last_error = 0;
        return 0;
    case SO_REUSEADDR:
        *out = 0;
        return 0;
    default:
        return SYS_ENOTSUP;
    }
}

bool socket_describe(u32 index, char *out, size_t outlen)
{
    u32 seen = 0;

    for (u32 i = 0; i < SOCKET_MAX; i++) {
        struct socket *s = &sockets[i];

        if (!s->used)
            continue;
        if (seen++ != index)
            continue;

        char pb[IPV4_STR_LEN];

        if (s->type == SOCK_STREAM) {
            enum tcp_state st = s->tcp_handle >= 0
                                    ? tcp_handle_state(s->tcp_handle)
                                    : TCP_CLOSED;

            ksnprintf(out, outlen, "tcp  local %-5u %-12s %s:%u", s->local_port,
                      s->listening ? "LISTEN" : tcp_state_name(st),
                      ipv4_str(s->peer_addr, pb, sizeof(pb)), s->peer_port);
        } else {
            ksnprintf(out, outlen, "udp  local %-5u %-12s %s:%u", s->udp_port,
                      s->connected ? "connected" : "unconnected",
                      ipv4_str(s->peer_addr, pb, sizeof(pb)), s->peer_port);
        }

        return true;
    }

    return false;
}
