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

#include <kernel/syscall_abi.h>
#include <kernel/types.h>

/* The call numbers and error values live in syscall_abi.h, which user
 * programs include verbatim. Two copies of an ABI is two versions of it. */

void syscall_init(void);
u32 syscall_count(void);

/* Validate that a userspace buffer is mapped and user-accessible before the
 * kernel dereferences it. Never trust a pointer that came from ring 3. */
bool user_range_ok(vaddr_t base, size_t len);

#endif /* _KERNEL_SYSCALL_H */
