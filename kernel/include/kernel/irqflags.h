/* StratumOS - masking interrupts, without naming an architecture.
 *
 * Portable kernel code needs exactly one thing from the processor that is not
 * arithmetic: a way to make a short sequence atomic against interrupts. The
 * heap needs it, the frame allocator needs it, every spinlock needs it.
 *
 * Before this header existed, those files got it by including <arch/io.h> -
 * the x86 port I/O header, which also declares `inb`, `outb` and the
 * privileged instructions. That worked for as long as there was one
 * architecture, and the RISC-V port's first build of kernel/mm/heap.c failed
 * on the include line: there is no port I/O on RISC-V, so there is no
 * arch/io.h to find.
 *
 * It is the only portability failure the port found that was not a cast of
 * the wrong width - and it is the more interesting one, because it is
 * structural. Portable code was reaching into the architecture layer through
 * a door labelled with one architecture's name. The fix is to give the door
 * a neutral label and let each architecture answer it.
 *
 * What an implementation has to provide:
 *
 *   bool irq_save(void)         mask interrupts, return the previous state
 *   void irq_restore(bool)      put the previous state back
 *   bool irq_enabled(void)      are they on now?
 *   void barrier(void)          a compiler barrier, no instruction
 *   void cpu_relax(void)        a hint for a spin loop
 *
 * Nesting-safe by construction, because irq_restore takes the state rather
 * than unconditionally enabling - which is the whole reason this is a pair of
 * functions and not `cli`/`sti`.
 */
#ifndef _KERNEL_IRQFLAGS_H
#define _KERNEL_IRQFLAGS_H

#if defined(__i386__) || defined(__x86_64__)
/* x86 keeps these alongside the port I/O primitives, which is where they
 * were when this header was written and is not worth moving. */
#include <arch/io.h>
#elif defined(__riscv)
#include <arch/riscv64/irqflags.h>
#else
#error "StratumOS: no interrupt-flag implementation for this architecture"
#endif

#endif /* _KERNEL_IRQFLAGS_H */
