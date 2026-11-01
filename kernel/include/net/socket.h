/* StratumOS - sockets, as descriptors.
 *
 * WHY THIS IS A SMALL FILE
 *
 * This is the layer the descriptor table was built for. The note at the top
 * of fs/vfs.h says it: there was no sockets API because there was no `open`,
 * so there was nothing a descriptor could be, and adding sockets first would
 * have meant a second parallel set of calls that happened to return small
 * integers - a descriptor table, built twice, badly.
 *
 * With the VFS in place and TCP able to hold several connections, a socket is
 * one more `struct vfs_ops`. `read` is a receive, `write` is a send, `close`
 * is a shutdown, and `poll_read` is what makes a blocking receive work. The
 * three calls that have no file equivalent - bind, listen, accept - are the
 * only ones that need anything new, and accept is the interesting one: it
 * produces a *descriptor*, which is why fd_install() exists.
 *
 * That is the whole argument for having built it in this order. A socket
 * that was not a descriptor would have needed its own table, its own
 * close-on-exit path, its own fork semantics and its own `read`. Being a
 * descriptor, it inherits all four and this file is what is left over.
 *
 * WHAT A SOCKET HERE CANNOT DO
 *
 *   - no select or poll across several descriptors. The `poll_read` and
 *     `poll_write` ops exist and are used by the blocking paths, so the
 *     mechanism is there; what is missing is a call that waits on a set,
 *     which needs a wait queue the scheduler does not have yet.
 *   - no out-of-band data, no socket-level options beyond timeouts and
 *     SO_ERROR, no SO_LINGER.
 *   - no Unix-domain sockets, no raw sockets, and one address family.
 *   - a datagram socket is connectionless only: sendto and recvfrom work,
 *     and connect() on a SOCK_DGRAM records a default peer so that send and
 *     recv work too, which is what Berkeley sockets do.
 *   - blocking is implemented by polling the network and sleeping, not by a
 *     wait queue, so a blocked receive costs a timer tick of latency. It is
 *     the same compromise `ping` and `arp` make in the shell, for the same
 *     reason: there is no wait queue, and inventing one for this would be
 *     inventing it in the wrong place.
 */
#ifndef _NET_SOCKET_H
#define _NET_SOCKET_H

#include <kernel/syscall_abi.h>
#include <kernel/types.h>

#include <net/net.h>

/* How many sockets exist at once, across every process. Fixed, like the
 * open-file table and for the same reason: a ring-3 loop calling socket()
 * must hit a bound rather than an allocator. */
#define SOCKET_MAX 16

void socket_init(void);

/* Each of these is the kernel side of one system call. They take and return
 * descriptors, and every pointer they are given has already been copied out
 * of user memory by the syscall layer - nothing here dereferences a ring-3
 * address, which is the one rule that keeps the checks in one place. */
i32 socket_create(int domain, int type, int protocol);
i32 socket_bind(i32 fd, const struct sys_sockaddr *addr);
i32 socket_listen(i32 fd, int backlog);
i32 socket_accept(i32 fd, struct sys_sockaddr *peer);
i32 socket_connect(i32 fd, const struct sys_sockaddr *addr);
i32 socket_send(i32 fd, const void *buf, u32 len, u32 flags);
i32 socket_recv(i32 fd, void *buf, u32 len, u32 flags);
i32 socket_sendto(i32 fd, const void *buf, u32 len,
                  const struct sys_sockaddr *to);
i32 socket_recvfrom(i32 fd, void *buf, u32 len, struct sys_sockaddr *from);
i32 socket_shutdown(i32 fd, int how);
i32 socket_setsockopt(i32 fd, int level, int name, u32 value);
i32 socket_getsockopt(i32 fd, int level, int name, u32 *out);

/* Counters, for `sockets` in the shell and for the test suite. */
struct socket_stats {
    u32 created, closed, connects, accepts, binds;
    u32 bytes_sent, bytes_received;
    u32 blocking_waits; /* times a call had to wait for the network */
};

const struct socket_stats *socket_get_stats(void);
u32 socket_count(void);
/* A one-line description of socket `index`, for the shell. Returns false
 * past the end. */
bool socket_describe(u32 index, char *out, size_t outlen);

#endif /* _NET_SOCKET_H */
