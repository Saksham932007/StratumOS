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

static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("sti" ::: "memory"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }

static inline u32 read_eflags(void)
{
    u32 f;
    __asm__ volatile("pushfl; popl %0" : "=r"(f));
    return f;
}

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
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

#endif /* _ARCH_IO_H */
