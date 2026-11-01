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
