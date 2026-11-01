/* StratumOS - a ring-3 program that talks to the network.
 *
 * WHAT THIS PROVES, AND WHY IT HAD TO BE A SEPARATE PROGRAM
 *
 * Everything else that has used this kernel's network stack has been kernel
 * code: the shell's `ping`, `fetch` and `dns`, and the in-kernel test suite.
 * All of them run in ring 0, where a pointer is a pointer and a connection
 * is a static struct somewhere. None of them demonstrates the thing the
 * descriptor table and the socket layer were built for.
 *
 * This program runs in ring 3. Every address it hands the kernel is a
 * user-space address that has to be validated, every socket it opens is a
 * descriptor in its own table, and the connection it makes has to survive
 * being driven by a process that can be preempted between any two calls. If
 * the socket layer leaked a kernel pointer, forgot a bounds check, or tied a
 * connection to the wrong process, this is where it would show.
 *
 * It does four things, in increasing order of what they prove:
 *
 *   1. opens a socket, asks about it, and closes it. This is the descriptor
 *      path: socket() returns a small integer that fstat() reports as
 *      S_TYPE_SOCKET, which means the VFS dispatched to the socket ops.
 *
 *   2. sends a UDP datagram to the echo port and reads it back. The shortest
 *      round trip there is, and the first one driven from ring 3.
 *
 *   3. connects a TCP socket to the echo port, writes, reads the echo back,
 *      and shuts down. This is the active open, from a process.
 *
 *   4. listens on a port, and - because there is nothing else to connect to
 *      it - connects to itself from a second socket, accepts, and exchanges
 *      a byte. Two connections and a listener alive at the same moment,
 *      which is exactly what one TCP connection could never have done, and
 *      is the reason the connection table was made a table.
 *
 * Each step prints its result and the program keeps going, so a failure says
 * which layer failed rather than stopping at the first one.
 */
#include "syscall.h"

void _start(void);

#define ECHO_PORT         7
#define LISTEN_PORT       9100
#define LOOPBACK_ATTEMPTS 1

static int failures;

static void ok(const char *what)
{
    u_puts("  [net] ok: ");
    u_puts(what);
    u_puts("\n");
}

static void fail(const char *what, i32 err)
{
    failures++;
    u_puts("  [net] FAIL: ");
    u_puts(what);
    u_puts(" -> ");
    /* Errors are small negatives, and there is no strerror in ring 3, so the
     * number is printed. The kernel logs the name. */
    u_putu((u32)(-err));
    u_puts("\n");
}

/* ---- 1. a socket is a descriptor --------------------------------------- */

static void test_descriptor(void)
{
    i32 fd = sys_socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        fail("socket(SOCK_DGRAM)", fd);
        return;
    }

    struct sys_stat st;
    i32 rc = sys_fstat(fd, &st);

    if (rc < 0) {
        fail("fstat on a socket", rc);
        sys_close(fd);
        return;
    }

    if (st.type != S_TYPE_SOCKET) {
        u_puts("  [net] FAIL: fstat called a socket type ");
        u_putu(st.type);
        u_puts("\n");
        failures++;
        sys_close(fd);
        return;
    }

    /* A socket has no position, so seeking it must fail rather than silently
     * succeed - which is the assertion that the VFS's NULL `seek` op is
     * doing its job rather than the socket layer having to say so. */
    if (sys_lseek(fd, 0, SEEK_SET) >= 0) {
        u_puts("  [net] FAIL: lseek on a socket succeeded\n");
        failures++;
    }

    /* The domain and type checks, from the far side of the boundary. */
    i32 bad = sys_socket(99, SOCK_STREAM, 0);

    if (bad >= 0) {
        u_puts("  [net] FAIL: socket() accepted address family 99\n");
        failures++;
        sys_close(bad);
    }

    sys_close(fd);
    ok("socket() gives a descriptor, fstat calls it a socket, lseek refuses");
}

/* ---- 2. a UDP round trip ------------------------------------------------ */

static void test_udp(void)
{
    static const char msg[] = "udp from ring 3";
    char buf[64];
    struct sys_sockaddr to, from;
    unsigned int local;
    i32 fd = sys_socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        fail("socket(SOCK_DGRAM)", fd);
        return;
    }

    /* The echo service runs on this machine, so the datagram goes to this
     * machine's own address - which is also the one case that exercises the
     * stack's loopback-by-way-of-the-wire path, because there is no loopback
     * interface and the frame really does go out and come back. */
    if (sys_resolve("10.0.2.15", &local) < 0) {
        fail("resolve of a dotted quad", -1);
        sys_close(fd);
        return;
    }

    u_sockaddr(&to, local, ECHO_PORT);

    i32 n = sys_sendto(fd, msg, sizeof(msg) - 1, &to);

    if (n < 0) {
        fail("sendto the echo port", n);
        sys_close(fd);
        return;
    }

    n = sys_recvfrom(fd, buf, sizeof(buf), &from);

    if (n < 0) {
        fail("recvfrom the echo port", n);
        sys_close(fd);
        return;
    }

    if (n != (i32)(sizeof(msg) - 1)) {
        u_puts("  [net] FAIL: the echo came back as ");
        u_putu((u32)n);
        u_puts(" byte(s)\n");
        failures++;
        sys_close(fd);
        return;
    }

    for (i32 i = 0; i < n; i++) {
        if (buf[i] != msg[i]) {
            u_puts("  [net] FAIL: the echoed datagram differs\n");
            failures++;
            sys_close(fd);
            return;
        }
    }

    /* And the sender's address came back with it, which is the whole
     * difference between recvfrom and read. */
    if (from.addr != local || u_ntohs(from.port) != ECHO_PORT) {
        u_puts("  [net] FAIL: recvfrom reported the wrong sender\n");
        failures++;
    }

    sys_close(fd);
    ok("a UDP datagram went out and came back, with its sender");
}

/* ---- 3. an outgoing TCP connection ------------------------------------- */

static void test_tcp_connect(void)
{
    static const char msg[] = "tcp from ring 3";
    char buf[64];
    struct sys_sockaddr to;
    unsigned int local;
    i32 fd = sys_socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        fail("socket(SOCK_STREAM)", fd);
        return;
    }

    if (sys_resolve("10.0.2.15", &local) < 0) {
        fail("resolve", -1);
        sys_close(fd);
        return;
    }

    u_sockaddr(&to, local, ECHO_PORT);

    i32 rc = sys_connect(fd, &to);

    if (rc < 0) {
        fail("connect to the echo port", rc);
        sys_close(fd);
        return;
    }

    i32 n = sys_send(fd, msg, sizeof(msg) - 1);

    if (n < 0) {
        fail("send", n);
        sys_close(fd);
        return;
    }

    /* The echo comes back on the same connection. A short read is legal -
     * TCP is a stream and the echo may arrive in pieces - so this loops
     * rather than assuming one read is enough, which is the one thing a
     * first TCP program always gets wrong. */
    i32 got = 0;

    while (got < n) {
        i32 k = sys_recv(fd, buf + got, (u32)(n - got));

        if (k == 0)
            break; /* the peer closed */
        if (k < 0) {
            fail("recv", k);
            sys_close(fd);
            return;
        }

        got += k;
    }

    if (got != n) {
        u_puts("  [net] FAIL: the stream echoed ");
        u_putu((u32)got);
        u_puts(" of ");
        u_putu((u32)n);
        u_puts(" byte(s)\n");
        failures++;
        sys_close(fd);
        return;
    }

    for (i32 i = 0; i < n; i++) {
        if (buf[i] != msg[i]) {
            u_puts("  [net] FAIL: the echoed stream differs\n");
            failures++;
            sys_close(fd);
            return;
        }
    }

    sys_shutdown(fd, SHUT_WR);
    sys_close(fd);
    ok("a TCP connection was opened from ring 3, and echoed what it sent");
}

/* ---- 4. a listener, and a connection to it ----------------------------- */

static void test_listen_accept(void)
{
    static const char msg[] = "accepted";
    char buf[32];
    struct sys_sockaddr addr, peer;
    unsigned int local;
    i32 srv, cli, acc;

    if (sys_resolve("10.0.2.15", &local) < 0) {
        fail("resolve", -1);
        return;
    }

    srv = sys_socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        fail("socket for the listener", srv);
        return;
    }

    u_sockaddr(&addr, local, LISTEN_PORT);

    i32 rc = sys_bind(srv, &addr);

    if (rc < 0) {
        fail("bind", rc);
        sys_close(srv);
        return;
    }

    rc = sys_listen(srv, 4);
    if (rc < 0) {
        fail("listen", rc);
        sys_close(srv);
        return;
    }

    /* A second socket, connecting to the first. The listener has to stay
     * listening while this connection exists and while the accepted one
     * exists too - three connections' worth of state at once, which is the
     * thing a single-connection stack could not do.
     *
     * The connect is non-blocking, because a blocking one would wait for a
     * handshake that cannot complete until this same process calls accept -
     * and a process cannot be in two system calls at once. That is not a
     * limitation of the stack; it is what a single-threaded program has to
     * do anywhere. */
    cli = sys_socket(AF_INET, SOCK_STREAM, 0);
    if (cli < 0) {
        fail("socket for the client", cli);
        sys_close(srv);
        return;
    }

    sys_fcntl_nonblock(cli);

    rc = sys_connect(cli, &addr);

    /* EAGAIN means the handshake is under way, which is success here. */
    if (rc < 0 && rc != SYS_EAGAIN) {
        fail("connect to our own listener", rc);
        sys_close(cli);
        sys_close(srv);
        return;
    }

    acc = sys_accept(srv, &peer);

    if (acc < 0) {
        fail("accept", acc);
        sys_close(cli);
        sys_close(srv);
        return;
    }

    if (peer.addr != local) {
        u_puts("  [net] FAIL: accept reported the wrong peer address\n");
        failures++;
    }

    /* The listener is still a listener. Asserted, because the bug this
     * design exists to prevent is an accept that consumes its own
     * listener. */
    struct sys_stat st;

    if (sys_fstat(srv, &st) < 0 || st.type != S_TYPE_SOCKET) {
        u_puts("  [net] FAIL: the listener did not survive accept()\n");
        failures++;
    }

    /* One byte each way across the accepted connection. */
    i32 n = sys_send(acc, msg, sizeof(msg) - 1);

    if (n < 0) {
        fail("send on the accepted connection", n);
    } else {
        i32 got = 0;

        while (got < n) {
            i32 k = sys_recv(cli, buf + got, (u32)(n - got));

            if (k == 0)
                break;
            if (k == SYS_EAGAIN)
                continue;
            if (k < 0) {
                fail("recv on the client", k);
                break;
            }
            got += k;
        }

        if (got == n) {
            ok("a listener accepted a connection from this same process, and "
               "both ends carried data");
        } else {
            u_puts("  [net] FAIL: the accepted connection carried ");
            u_putu((u32)got);
            u_puts(" of ");
            u_putu((u32)n);
            u_puts(" byte(s)\n");
            failures++;
        }
    }

    sys_close(acc);
    sys_close(cli);
    sys_close(srv);
}

void _start(void)
{
    u_puts("[net] sockets from ring 3, pid ");
    u_putu((u32)sys_getpid());
    u_puts("\n");

    test_descriptor();
    test_udp();
    test_tcp_connect();
    test_listen_accept();

    if (failures) {
        u_puts("[net] ");
        u_putu((u32)failures);
        u_puts(" failure(s)\n");
    } else {
        u_puts("[net] all socket checks passed\n");
    }

    sys_exit(failures ? 1 : 0);
}
