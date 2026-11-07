/* StratumOS - userspace system call stubs.
 *
 * Everything a ring-3 program can do, it does through here. There is no libc:
 * the kernel's pages are not user-accessible, so a call into one would fault
 * immediately, and that is the correct behaviour rather than a limitation.
 */
#ifndef _USER_SYSCALL_H
#define _USER_SYSCALL_H

#include <kernel/syscall_abi.h>

typedef unsigned int u32;
typedef int i32;

static inline i32 syscall3(u32 nr, u32 a, u32 b, u32 c)
{
    i32 ret;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return ret;
}

/* ---- descriptors --------------------------------------------------------
 *
 * write() takes a descriptor now. It did not before: there was one possible
 * destination, the console, and the call took only a buffer and a length.
 * Every program here was updated in the same commit, and that is the whole
 * migration - there is no binary compatibility to keep, which is exactly
 * what makes changing the shape of call number 1 a reasonable thing to do
 * rather than a breaking change to apologise for.
 */
static inline i32 sys_write(int fd, const void *buf, u32 len)
{
    return syscall3(SYS_WRITE, (u32)fd, (u32)buf, len);
}

static inline i32 sys_read(int fd, void *buf, u32 len)
{
    return syscall3(SYS_READ, (u32)fd, (u32)buf, len);
}

static inline i32 sys_open(const char *path, u32 flags)
{
    return syscall3(SYS_OPEN, (u32)path, flags, 0);
}

static inline i32 sys_close(int fd)
{
    return syscall3(SYS_CLOSE, (u32)fd, 0, 0);
}

static inline i32 sys_lseek(int fd, i32 offset, int whence)
{
    return syscall3(SYS_LSEEK, (u32)fd, (u32)offset, (u32)whence);
}

static inline i32 sys_stat(const char *path, struct sys_stat *st)
{
    return syscall3(SYS_STAT, (u32)path, (u32)st, 0);
}

static inline i32 sys_fstat(int fd, struct sys_stat *st)
{
    return syscall3(SYS_FSTAT, (u32)fd, (u32)st, 0);
}

/* Read one directory entry by index. Returns 1 when `out` was filled, 0 at
 * the end of the directory, or a negative error. An index rather than an
 * opaque cookie, because the underlying driver's own iteration is by index -
 * see fs/fat16.h on why that is stable for a given filesystem. */
static inline i32 sys_getdent(int fd, u32 index, struct sys_dirent *out)
{
    return syscall3(SYS_GETDENT, (u32)fd, index, (u32)out);
}

static inline i32 sys_dup(int fd)
{
    return syscall3(SYS_DUP, (u32)fd, 0, 0);
}

static inline i32 sys_dup2(int oldfd, int newfd)
{
    return syscall3(SYS_DUP2, (u32)oldfd, (u32)newfd, 0);
}

static inline i32 sys_ioctl(int fd, u32 request, u32 arg)
{
    return syscall3(SYS_IOCTL, (u32)fd, request, arg);
}

static inline i32 sys_chdir(const char *path)
{
    return syscall3(SYS_CHDIR, (u32)path, 0, 0);
}

static inline i32 sys_getcwd(char *buf, u32 len)
{
    return syscall3(SYS_GETCWD, (u32)buf, len, 0);
}

static inline i32 sys_unlink(const char *path)
{
    return syscall3(SYS_UNLINK, (u32)path, 0, 0);
}

static inline i32 sys_mkdir(const char *path)
{
    return syscall3(SYS_MKDIR, (u32)path, 0, 0);
}

static inline i32 sys_rmdir(const char *path)
{
    return syscall3(SYS_RMDIR, (u32)path, 0, 0);
}

static inline i32 sys_rename(const char *from, const char *to)
{
    return syscall3(SYS_RENAME, (u32)from, (u32)to, 0);
}

static inline i32 sys_truncate(int fd, u32 length)
{
    return syscall3(SYS_TRUNCATE, (u32)fd, length, 0);
}

static inline i32 sys_sync(void)
{
    return syscall3(SYS_SYNC, 0, 0, 0);
}

/* ---- sockets -----------------------------------------------------------
 *
 * The Berkeley calls, with two adaptations to a three-register convention.
 * sendto and recvfrom pass a `struct sys_msg` because they want five
 * arguments; setsockopt and getsockopt fold the level into the high half of
 * the option name, because the level only ever holds SOL_SOCKET and a fourth
 * register for a constant would be a strange trade.
 */

static inline i32 sys_socket(int domain, int type, int protocol)
{
    return syscall3(SYS_SOCKET, (u32)domain, (u32)type, (u32)protocol);
}

static inline i32 sys_bind(int fd, const struct sys_sockaddr *addr)
{
    return syscall3(SYS_BIND, (u32)fd, (u32)addr, 0);
}

static inline i32 sys_listen(int fd, int backlog)
{
    return syscall3(SYS_LISTEN, (u32)fd, (u32)backlog, 0);
}

/* `peer` may be null, in which case the caller does not learn who connected.
 * Returns a new descriptor. */
static inline i32 sys_accept(int fd, struct sys_sockaddr *peer)
{
    return syscall3(SYS_ACCEPT, (u32)fd, (u32)peer, 0);
}

static inline i32 sys_connect(int fd, const struct sys_sockaddr *addr)
{
    return syscall3(SYS_CONNECT, (u32)fd, (u32)addr, 0);
}

static inline i32 sys_send(int fd, const void *buf, u32 len)
{
    return syscall3(SYS_SEND, (u32)fd, (u32)buf, len);
}

static inline i32 sys_recv(int fd, void *buf, u32 len)
{
    return syscall3(SYS_RECV, (u32)fd, (u32)buf, len);
}

static inline i32 sys_sendto(int fd, const void *buf, u32 len,
                             const struct sys_sockaddr *to)
{
    struct sys_msg m;

    m.buf = (u32)buf;
    m.len = len;
    m.addr = (u32)to;
    m.flags = 0;
    return syscall3(SYS_SENDTO, (u32)fd, (u32)&m, 0);
}

static inline i32 sys_recvfrom(int fd, void *buf, u32 len,
                               struct sys_sockaddr *from)
{
    struct sys_msg m;

    m.buf = (u32)buf;
    m.len = len;
    m.addr = (u32)from;
    m.flags = 0;
    return syscall3(SYS_RECVFROM, (u32)fd, (u32)&m, 0);
}

/* Make a descriptor non-blocking. Named for what it does rather than for the
 * ioctl it uses, because a program should not have to know that this kernel
 * has no fcntl. */
static inline i32 sys_fcntl_nonblock(int fd)
{
    return sys_ioctl(fd, FIONBIO, 1);
}

static inline i32 sys_shutdown(int fd, int how)
{
    return syscall3(SYS_SHUTDOWN, (u32)fd, (u32)how, 0);
}

static inline i32 sys_setsockopt(int fd, int level, int name, u32 value)
{
    return syscall3(SYS_SETSOCKOPT, (u32)fd,
                    ((u32)level << 16) | ((u32)name & 0xFFFFu), value);
}

static inline i32 sys_getsockopt(int fd, int level, int name, u32 *out)
{
    return syscall3(SYS_GETSOCKOPT, (u32)fd,
                    ((u32)level << 16) | ((u32)name & 0xFFFFu), (u32)out);
}

/* Turn a name into an IPv4 address, in network byte order. A dotted quad
 * resolves to itself without a query. */
static inline i32 sys_resolve(const char *name, unsigned int *addr_out)
{
    return syscall3(SYS_RESOLVE, (u32)name, (u32)addr_out, 0);
}

/* Byte order, for user code. Written out rather than taken from the kernel's
 * headers, because this file is the whole of what a program compiles
 * against. */
static inline unsigned short u_htons(unsigned short v)
{
    return (unsigned short)((v << 8) | (v >> 8));
}

static inline unsigned short u_ntohs(unsigned short v)
{
    return u_htons(v);
}

/* Fill in a sockaddr. `addr` is already in network order - which is what
 * sys_resolve returns - and `port` is in host order, because a port is a
 * number a programmer writes. */
static inline void u_sockaddr(struct sys_sockaddr *sa, unsigned int addr,
                              unsigned short port)
{
    unsigned int i;
    unsigned char *p = (unsigned char *)sa;

    for (i = 0; i < sizeof(*sa); i++)
        p[i] = 0;

    sa->family = AF_INET;
    sa->port = u_htons(port);
    sa->addr = addr;
}

/* "10.0.2.2" -> an address in network byte order. Returns 0 on anything that
 * is not four decimal octets, which is also why 0.0.0.0 cannot be expressed:
 * a program that wants the any-address does not need to parse it. */
static inline unsigned int u_inet_addr(const char *s)
{
    unsigned int octet[4] = {0, 0, 0, 0};
    int i = 0;
    int digits = 0;

    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            octet[i] = octet[i] * 10u + (unsigned int)(*s - '0');
            if (octet[i] > 255)
                return 0;
            digits++;
            continue;
        }

        if (*s != '.' || !digits || i == 3)
            return 0;

        i++;
        digits = 0;
    }

    if (i != 3 || !digits)
        return 0;

    return octet[0] | (octet[1] << 8) | (octet[2] << 16) | (octet[3] << 24);
}

static inline i32 sys_getpid(void)
{
    return syscall3(SYS_GETPID, 0, 0, 0);
}

static inline i32 sys_uptime(void)
{
    return syscall3(SYS_UPTIME, 0, 0, 0);
}

static inline void sys_yield(void)
{
    (void)syscall3(SYS_YIELD, 0, 0, 0);
}

static inline void sys_sleep(u32 ms)
{
    (void)syscall3(SYS_SLEEP, ms, 0, 0);
}

static inline i32 sys_getkey(void)
{
    return syscall3(SYS_GETKEY, 0, 0, 0);
}

/* Duplicate this process. Returns the child's pid in the parent and 0 in the
 * child - the same call, returning twice. */
static inline i32 sys_fork(void)
{
    return syscall3(SYS_FORK, 0, 0, 0);
}

/* Collect a dead child; blocks while one is still running. Returns its pid,
 * or -1 if there are no children. */
static inline i32 sys_wait(int *status)
{
    return syscall3(SYS_WAIT, (u32)status, 0, 0);
}

static inline i32 sys_getppid(void)
{
    return syscall3(SYS_GETPPID, 0, 0, 0);
}

/* Replace this process's image with another, keeping the pid. On success it
 * does not return - there is nothing left to return to, because the code that
 * made the call was in the image that was just unmapped. */
static inline i32 sys_exec(const char *name)
{
    return syscall3(SYS_EXEC, (u32)name, 0, 0);
}

__attribute__((noreturn)) static inline void sys_exit(int code)
{
    (void)syscall3(SYS_EXIT, (u32)code, 0, 0);
    __builtin_unreachable();
}

/* ---- the little bit of runtime a program needs ----------------------- */

static inline u32 u_strlen(const char *s)
{
    u32 n = 0;
    while (s[n])
        n++;
    return n;
}

static inline void u_puts(const char *s)
{
    (void)sys_write(STDOUT_FD, s, u_strlen(s));
}

static inline void u_eputs(const char *s)
{
    (void)sys_write(STDERR_FD, s, u_strlen(s));
}

/* Write all of a buffer, looping over short writes. The kernel caps one
 * transfer at a kilobyte, so anything larger comes back short - and a program
 * that ignored that would silently truncate its own output. */
static inline i32 u_write_all(int fd, const void *buf, u32 len)
{
    const char *p = buf;
    u32 done = 0;

    while (done < len) {
        i32 n = sys_write(fd, p + done, len - done);

        if (n <= 0)
            return n < 0 ? n : (i32)done;

        done += (u32)n;
    }

    return (i32)done;
}

static inline void u_putu(u32 v)
{
    char buf[12];
    int i = (int)sizeof(buf);

    buf[--i] = '\0';
    if (v == 0)
        buf[--i] = '0';
    while (v && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }

    u_puts(&buf[i]);
}

/* ---- a line, written in one call --------------------------------------
 *
 * Three calls to u_puts() for one line of output was fine when one processor
 * ran one task at a time: nothing could get between them. Two processes on
 * two processors interleave at the syscall boundary, and a line assembled
 * from several writes comes out spliced into somebody else's - which is not
 * a kernel bug, it is what concurrent writers to one descriptor means.
 *
 * So a program that wants a whole line has to write a whole line. These
 * build it in the caller's own memory and spend one write() on it. Anything
 * that does not fit is dropped rather than truncated mid-number: a partial
 * line is the thing being avoided.
 */
#define U_LINE_MAX 200

struct u_line {
    char buf[U_LINE_MAX];
    u32 len;
};

static inline void u_line_begin(struct u_line *l)
{
    l->len = 0;
}

static inline void u_line_str(struct u_line *l, const char *s)
{
    while (*s && l->len < U_LINE_MAX)
        l->buf[l->len++] = *s++;
}

static inline void u_line_u(struct u_line *l, u32 v)
{
    char tmp[12];
    int i = (int)sizeof(tmp);

    tmp[--i] = '\0';
    if (v == 0)
        tmp[--i] = '0';
    while (v && i > 0) {
        tmp[--i] = (char)('0' + (v % 10));
        v /= 10;
    }

    u_line_str(l, &tmp[i]);
}

static inline void u_line_hex(struct u_line *l, u32 v)
{
    static const char hex[] = "0123456789abcdef";
    char tmp[11];
    int i = (int)sizeof(tmp);

    tmp[--i] = '\0';
    if (v == 0)
        tmp[--i] = '0';
    while (v && i > 2) {
        tmp[--i] = hex[v & 0xF];
        v >>= 4;
    }
    tmp[--i] = 'x';
    tmp[--i] = '0';

    u_line_str(l, &tmp[i]);
}

/* One write for the whole line, through u_write_all so a short write does not
 * split it after all. */
static inline void u_line_end(struct u_line *l)
{
    u_line_str(l, "\n");
    (void)u_write_all(STDOUT_FD, l->buf, l->len);
    l->len = 0;
}

static inline void u_puthex(u32 v)
{
    static const char digits[] = "0123456789abcdef";
    char buf[11];
    int i = (int)sizeof(buf);

    buf[--i] = '\0';
    if (v == 0)
        buf[--i] = '0';
    while (v && i > 2) {
        buf[--i] = digits[v & 0xF];
        v >>= 4;
    }
    buf[--i] = 'x';
    buf[--i] = '0';

    u_puts(&buf[i]);
}

#endif /* _USER_SYSCALL_H */
