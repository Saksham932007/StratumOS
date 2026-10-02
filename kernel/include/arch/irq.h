/* StratumOS - 8259A PIC and hardware interrupt routing. */
#ifndef _ARCH_IRQ_H
#define _ARCH_IRQ_H

#include <arch/idt.h>
#include <kernel/types.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20
#define PIC_READ_ISR 0x0B

/* After remapping, hardware IRQ n arrives as vector IRQ_BASE + n. The default
 * BIOS mapping puts IRQ 0-7 at vectors 8-15, which collides head-on with the
 * CPU's own exception vectors (8 is #DF, 13 is #GP). */
#define IRQ_BASE   32
#define IRQ_COUNT  16

#define IRQ_TIMER    0
#define IRQ_KEYBOARD 1
#define IRQ_CASCADE  2
#define IRQ_COM1     4
#define IRQ_RTC      8
#define IRQ_MOUSE    12

typedef void (*irq_handler_t)(struct regs *r);

void irq_init(void);
void irq_install_handler(unsigned irq, irq_handler_t handler, const char *name);
void irq_uninstall_handler(unsigned irq);
void irq_mask(unsigned irq);
void irq_unmask(unsigned irq);
void irq_dispatch(struct regs *r);

u32 irq_count(unsigned irq);
const char *irq_name(unsigned irq);
u32 irq_spurious_count(void);

#endif /* _ARCH_IRQ_H */
