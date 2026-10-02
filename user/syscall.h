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

static inline i32 sys_write(const char *s, u32 len)
{
    return syscall3(SYS_WRITE, (u32)s, len, 0);
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
    (void)sys_write(s, u_strlen(s));
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

#endif /* _USER_SYSCALL_H */
