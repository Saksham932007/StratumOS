/* StratumOS - RISC-V interrupt masking, for <kernel/irqflags.h>.
 *
 * sstatus.SIE is the supervisor interrupt enable. Reading and clearing it is
 * one CSR instruction each, and `csrrc` does both atomically - returning the
 * old value while clearing the bits, which is exactly the save-and-disable
 * primitive portable code wants and which x86 needs two instructions and a
 * stack push to express.
 *
 * Only supervisor interrupts. This kernel's machine-mode code is the timer
 * handler and nothing else, and it does not take locks - so masking
 * mstatus.MIE here would disable the clock for the duration of every
 * critical section, which is a worse trade than the one it buys.
 */
#ifndef _ARCH_RISCV64_IRQFLAGS_H
#define _ARCH_RISCV64_IRQFLAGS_H

#include <kernel/types.h>

#define SSTATUS_SIE (1ul << 1)

static inline bool irq_enabled(void)
{
    u64 s;

    __asm__ volatile("csrr %0, sstatus" : "=r"(s));
    return (s & SSTATUS_SIE) != 0;
}

static inline bool irq_save(void)
{
    u64 old;

    /* csrrc: read the CSR into rd and clear the bits of rs1 in it, as one
     * instruction. The x86 equivalent is pushf/pop/cli, and the difference
     * is not speed - it is that there is no window between the read and the
     * clear for an interrupt to arrive in. */
    __asm__ volatile("csrrc %0, sstatus, %1"
                     : "=r"(old)
                     : "r"(SSTATUS_SIE)
                     : "memory");
    return (old & SSTATUS_SIE) != 0;
}

static inline void irq_restore(bool was_enabled)
{
    if (was_enabled)
        __asm__ volatile("csrs sstatus, %0" ::"r"(SSTATUS_SIE) : "memory");
}

static inline void barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static inline void cpu_relax(void)
{
    /* The Zihintpause extension's `pause`, which rv64imac does not include -
     * so this is a compiler barrier and a comment rather than a pretence.
     * x86's PAUSE has no mandatory counterpart here. */
    barrier();
}

#endif /* _ARCH_RISCV64_IRQFLAGS_H */
