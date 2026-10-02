/* StratumOS - the system call ABI.
 *
 * Shared verbatim between the kernel and user programs, which is the point:
 * the call numbers and error values are a contract, and a contract with two
 * copies is a contract with two versions. Nothing in here may depend on
 * kernel-internal headers, because user code compiles against it.
 *
 * Calling convention: `int 0x80` with the call number in EAX and up to three
 * arguments in EBX, ECX and EDX. The result comes back in EAX. Negative
 * values are errors.
 */
#ifndef _KERNEL_SYSCALL_ABI_H
#define _KERNEL_SYSCALL_ABI_H

enum {
    SYS_EXIT = 0,
    SYS_WRITE = 1,
    SYS_GETPID = 2,
    SYS_YIELD = 3,
    SYS_SLEEP = 4,
    SYS_UPTIME = 5,
    SYS_GETKEY = 6,
    SYS_MAX
};

#define SYS_EBADCALL (-1)
#define SYS_EFAULT   (-2)
#define SYS_EINVAL   (-3)

/* The vector the gate is installed on. */
#define INT_SYSCALL  0x80

#endif /* _KERNEL_SYSCALL_ABI_H */
