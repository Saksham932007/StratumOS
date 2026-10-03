/* StratumOS - x86 port I/O and small CPU primitives. */
#ifndef _ARCH_IO_H
#define _ARCH_IO_H

#include <kernel/types.h>

static inline void outb(u16 port, u8 val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw(u16 port, u16 val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outl(u16 port, u32 val)
{
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline u8 inb(u16 port)
{
    u8 v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline u16 inw(u16 port)
{
    u16 v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline u32 inl(u16 port)
{
    u32 v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* Write to an unused port to burn ~1 microsecond. Needed between back-to-back
 * writes to slow legacy devices such as the 8259 PIC and 8042 controller. */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

/* The privileged instructions, and the one accommodation this header makes
 * for being compiled on a host.
 *
 * The fuzz targets in tests/fuzz compile real kernel sources - heap.c takes
 * interrupt-safe locks, which means `cli`, which in a user process is an
 * immediate SIGSEGV. The choice is between stubbing them here, behind a macro
 * the kernel build never defines, and maintaining a parallel copy of every
 * file that touches them. The second is how a test suite stops testing the
 * code that ships.
 *
 * Interrupt state is modelled rather than ignored, so that irq_save() and
 * irq_restore() still nest correctly and a lock that forgets to restore is
 * still a bug the fuzzer can find. */
#ifdef STRATUM_FUZZING

extern int stratum_fuzz_interrupts_enabled;

static inline void cli(void)
{
    stratum_fuzz_interrupts_enabled = 0;
}
static inline void sti(void)
{
    stratum_fuzz_interrupts_enabled = 1;
}
static inline void hlt(void)
{
}

#else

static inline void cli(void)
{
    __asm__ volatile("cli" ::: "memory");
}
static inline void sti(void)
{
    __asm__ volatile("sti" ::: "memory");
}
static inline void hlt(void)
{
    __asm__ volatile("hlt");
}

#endif /* STRATUM_FUZZING */

#ifdef STRATUM_FUZZING

static inline u32 read_eflags(void)
{
    return stratum_fuzz_interrupts_enabled ? 0x200u : 0u;
}

#else

static inline u32 read_eflags(void)
{
    u32 f;
    __asm__ volatile("pushfl; popl %0" : "=r"(f));
    return f;
}

#endif

#define EFLAGS_IF 0x200u

static inline bool irq_enabled(void)
{
    return (read_eflags() & EFLAGS_IF) != 0;
}

/* Save-and-disable / restore pair for short critical sections. Nesting-safe,
 * unlike a bare cli/sti, because it restores the *previous* state. */
static inline bool irq_save(void)
{
    bool was = irq_enabled();
    cli();
    return was;
}

static inline void irq_restore(bool was_enabled)
{
    if (was_enabled)
        sti();
}

/* A compiler barrier: nothing more than "do not move memory accesses across
 * this point". It emits no instruction.
 *
 * Needed wherever a store changes the *meaning* of a later access rather than
 * its value - writing a page table entry being the example that matters here.
 * The compiler sees a store to one address and a load from an unrelated one,
 * and is entitled to reorder them; the hardware sees the first store change
 * where the second access goes. Only a barrier connects the two. */
static inline void barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static inline void cpu_relax(void)
{
    __asm__ volatile("pause" ::: "memory");
}

static inline u32 read_cr0(void)
{
    u32 v;
    __asm__ volatile("movl %%cr0, %0" : "=r"(v));
    return v;
}

static inline void write_cr0(u32 v)
{
    __asm__ volatile("movl %0, %%cr0" : : "r"(v) : "memory");
}

static inline u32 read_cr2(void)
{
    u32 v;
    __asm__ volatile("movl %%cr2, %0" : "=r"(v));
    return v;
}

static inline u32 read_cr3(void)
{
    u32 v;
    __asm__ volatile("movl %%cr3, %0" : "=r"(v));
    return v;
}

static inline void write_cr3(u32 v)
{
    __asm__ volatile("movl %0, %%cr3" : : "r"(v) : "memory");
}

static inline u32 read_cr4(void)
{
    u32 v;
    __asm__ volatile("movl %%cr4, %0" : "=r"(v));
    return v;
}

static inline void write_cr4(u32 v)
{
    __asm__ volatile("movl %0, %%cr4" : : "r"(v) : "memory");
}

static inline void invlpg(vaddr_t va)
{
#ifdef STRATUM_FUZZING
    (void)va;
    barrier();
#else
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
#endif
}

#endif /* _ARCH_IO_H */
