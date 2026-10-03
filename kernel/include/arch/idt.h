/* StratumOS - interrupt descriptor table and the trap frame.
 *
 * struct regs mirrors, field for field, what isr.asm and irq.asm push before
 * calling into C. Getting this layout wrong is the classic way to lose a
 * weekend: the original version of this project never passed the frame
 * pointer at all, so every handler received a stale segment selector as its
 * `ctx` argument and quietly read garbage.
 *
 * Stack at the moment the C handler runs (low address first):
 *
 *     gs fs es ds                      <- pushed by the stub
 *     edi esi ebp esp ebx edx ecx eax  <- pushed by `pusha`
 *     int_no err_code                  <- pushed by the stub
 *     eip cs eflags                    <- pushed by the CPU
 *     user_esp ss                      <- CPU, only on a privilege change
 */
#ifndef _ARCH_IDT_H
#define _ARCH_IDT_H

#include <kernel/types.h>

struct regs {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    u32 int_no, err_code;
    u32 eip, cs, eflags;
    u32 user_esp, ss;
};

struct idt_entry {
    u16 base_low;
    u16 selector;
    u8 always0;
    u8 flags;
    u16 base_high;
} PACKED;

struct idt_ptr {
    u16 limit;
    u32 base;
} PACKED;

#define IDT_PRESENT     0x80
#define IDT_RING0       0x00
#define IDT_RING3       0x60
#define IDT_GATE_INT32  0x0E
#define IDT_GATE_TRAP32 0x0F

#define IDT_ENTRIES     256
/* INT_SYSCALL lives in kernel/syscall_abi.h, shared with user programs. */

typedef void (*isr_handler_t)(struct regs *r);

void idt_init(void);

/* Load the IDT register on this processor. idt_init() builds the table and
 * loads it on the boot processor; an application processor only needs the
 * load, because the table is shared - an interrupt descriptor table has no
 * per-CPU content, unlike the GDT, whose TSS descriptors do. */
void idt_load(void);
void idt_set_gate(u8 num, u32 base, u16 selector, u8 flags);

/* Install a handler for a raw interrupt vector (0-255). */
void isr_install_handler(u8 vector, isr_handler_t handler);

/* Called from the assembly stubs. */
void interrupt_dispatch(struct regs *r);

const char *exception_name(u32 vector);
void regs_dump(const struct regs *r);

/* Decode a page-fault error code and CR2 into readable text. Shared between
 * the default exception path and the VMM's own handler. */
void page_fault_describe(const struct regs *r);

#endif /* _ARCH_IDT_H */
