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

/* The same, for a buffer the kernel is about to *write* through.
 *
 * Present and user-accessible is not enough to write to, and the difference
 * is not theoretical: a program's own .text is mapped read-only by W^X, so a
 * pointer into it passes user_range_ok() and then faults in ring 0 when the
 * kernel stores to it. CR0.WP makes that fault a kernel fault, and a kernel
 * fault is a panic - so a ring-3 program could halt the machine by passing
 * the address of its own code to any call that returns data through a
 * pointer.
 *
 * Found by the ring-3 fuzzer the first time it was pointed at read(). The bug
 * predates the descriptor table: wait()'s optional status pointer has had it
 * since processes landed, and was simply never called with a read-only
 * address by anything that was not trying to. See docs/FUZZING.md. */
bool user_range_writable(vaddr_t base, size_t len);

/* The name of a SYS_E* value, for a log line or a shell message. Returns a
 * usable string for anything, including a value that is not an error code at
 * all, because its callers are error paths and an error path that itself
 * fails is the worst place to find a missing case. */
const char *sys_strerror(i32 err);

#endif /* _KERNEL_SYSCALL_H */
