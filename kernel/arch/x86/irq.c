/* StratumOS - 8259A PIC programming and hardware interrupt routing.
 *
 * Two cascaded 8259s give 15 usable interrupt lines. Out of reset the BIOS
 * maps them onto vectors 8-15 and 0x70-0x77, which collides with the CPU's
 * own exception vectors: a timer tick would arrive as #DF and a keypress as
 * #GP. Remapping them to 32-47 is therefore not an optimisation, it is a
 * correctness requirement.
 *
 * Spurious interrupts are handled properly. If IRQ 7 or IRQ 15 fires but the
 * In-Service register says otherwise, the PIC is reporting line noise and the
 * interrupt must NOT be acknowledged - acknowledging it would lose a real
 * interrupt later.
 */
#define LOG_TAG "irq"

#include <arch/idt.h>
#include <arch/io.h>
#include <arch/irq.h>
#include <kernel/log.h>
#include <kernel/string.h>

struct irq_slot {
    irq_handler_t handler;
    const char *name;
    u32 count;
};

static struct irq_slot slots[IRQ_COUNT];
static u32 spurious;

/* Cached interrupt mask. Reading it back from the PIC works, but keeping it
 * here makes mask/unmask a pure function of our own state and avoids a
 * surprise if firmware fiddles with it. */
static u16 irq_mask_cache = 0xFFFF;

static void pic_write_masks(void)
{
    outb(PIC1_DATA, (u8)(irq_mask_cache & 0xFF));
    outb(PIC2_DATA, (u8)((irq_mask_cache >> 8) & 0xFF));
}

static void pic_remap(void)
{
    /* ICW1: begin initialisation, expect ICW4. */
    outb(PIC1_CMD, 0x11);
    io_wait();
    outb(PIC2_CMD, 0x11);
    io_wait();

    /* ICW2: vector offset for each chip. */
    outb(PIC1_DATA, IRQ_BASE);
    io_wait();
    outb(PIC2_DATA, IRQ_BASE + 8);
    io_wait();

    /* ICW3: master has a slave on line 2; slave's cascade identity is 2. */
    outb(PIC1_DATA, 1 << IRQ_CASCADE);
    io_wait();
    outb(PIC2_DATA, IRQ_CASCADE);
    io_wait();

    /* ICW4: 8086 mode, normal EOI. */
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    /* Start with everything masked except the cascade line, without which
     * the slave PIC can never deliver anything. Drivers unmask their own
     * line when they are ready to receive - so an interrupt cannot arrive
     * before its handler is installed. */
    irq_mask_cache = (u16)~(1u << IRQ_CASCADE);
    pic_write_masks();
}

void irq_init(void)
{
    memset(slots, 0, sizeof(slots));
    spurious = 0;
    pic_remap();
    pr_debug("PIC remapped to vectors %u-%u, all lines masked",
             (unsigned)IRQ_BASE, (unsigned)(IRQ_BASE + IRQ_COUNT - 1));
}

void irq_install_handler(unsigned irq, irq_handler_t handler, const char *name)
{
    if (irq >= IRQ_COUNT) {
        pr_err("refusing to install handler for invalid IRQ %u", irq);
        return;
    }

    bool irqs = irq_save();
    slots[irq].handler = handler;
    slots[irq].name = name;
    irq_restore(irqs);

    irq_unmask(irq);
    pr_debug("IRQ %u -> %s", irq, name ? name : "(unnamed)");
}

void irq_uninstall_handler(unsigned irq)
{
    if (irq >= IRQ_COUNT)
        return;

    irq_mask(irq);

    bool irqs = irq_save();
    slots[irq].handler = NULL;
    slots[irq].name = NULL;
    irq_restore(irqs);
}

void irq_mask(unsigned irq)
{
    if (irq >= IRQ_COUNT)
        return;
    irq_mask_cache |= (u16)(1u << irq);
    pic_write_masks();
}

void irq_unmask(unsigned irq)
{
    if (irq >= IRQ_COUNT)
        return;
    irq_mask_cache &= (u16)~(1u << irq);
    pic_write_masks();
}

/* Ask a PIC whether it really has an interrupt in service. */
static bool pic_isr_bit(u16 cmd_port, u8 line)
{
    outb(cmd_port, PIC_READ_ISR);
    return (inb(cmd_port) & (1u << line)) != 0;
}

void irq_dispatch(struct regs *r)
{
    unsigned irq = r->int_no - IRQ_BASE;

    if (irq >= IRQ_COUNT)
        return;

    /* --- spurious interrupt detection ---------------------------------- */
    if (irq == 7 && !pic_isr_bit(PIC1_CMD, 7)) {
        /* Master reported line 7 but has nothing in service: noise.
         * Send no EOI at all. */
        spurious++;
        return;
    }

    if (irq == 15 && !pic_isr_bit(PIC2_CMD, 7)) {
        /* Slave-side spurious. The master *did* genuinely accept the cascade,
         * so it still needs its EOI - but the slave must not get one. */
        spurious++;
        outb(PIC1_CMD, PIC_EOI);
        return;
    }

    slots[irq].count++;

    if (slots[irq].handler) {
        slots[irq].handler(r);
    } else {
        /* Mask it so an unclaimed, level-triggered line cannot wedge the
         * machine by re-asserting forever. */
        pr_warn("unhandled IRQ %u - masking the line", irq);
        irq_mask(irq);
    }

    /* End of interrupt: the slave first, then the master, because the master
     * is what is actually holding the CPU's INTR line for a cascaded IRQ. */
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

u32 irq_count(unsigned irq)
{
    return (irq < IRQ_COUNT) ? slots[irq].count : 0;
}

const char *irq_name(unsigned irq)
{
    if (irq >= IRQ_COUNT || !slots[irq].name)
        return "(none)";
    return slots[irq].name;
}

u32 irq_spurious_count(void)
{
    return spurious;
}
