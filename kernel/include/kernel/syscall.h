/* StratumOS - the ring 3 -> ring 0 boundary.
 *
 * Userspace issues `int 0x80` with the call number in EAX and up to three
 * arguments in EBX, ECX, EDX; the result comes back in EAX. The gate is
 * installed with DPL 3 so ring 3 may invoke it, and it is an *interrupt* gate,
 * so interrupts are masked on entry and the handler decides when to re-enable
 * them.
 */
#ifndef _KERNEL_SYSCALL_H
#define _KERNEL_SYSCALL_H

#include <arch/idt.h>
#include <kernel/types.h>

enum {
    SYS_EXIT   = 0,
    SYS_WRITE  = 1,
    SYS_GETPID = 2,
    SYS_YIELD  = 3,
    SYS_SLEEP  = 4,
    SYS_UPTIME = 5,
    SYS_GETKEY = 6,
    SYS_MAX
};

#define SYS_EBADCALL (-1)
#define SYS_EFAULT   (-2)
#define SYS_EINVAL   (-3)

void syscall_init(void);
u32  syscall_count(void);

/* Validate that a userspace buffer is mapped and user-accessible before the
 * kernel dereferences it. Never trust a pointer that came from ring 3. */
bool user_range_ok(vaddr_t base, size_t len);

#endif /* _KERNEL_SYSCALL_H */
