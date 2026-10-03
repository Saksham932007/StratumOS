/* StratumOS - the local APIC.
 *
 * The 8259 pair this kernel has been using since boot is a 1981 part with
 * two jobs it can no longer do: it has no concept of a second processor, and
 * it cannot send an interrupt from one CPU to another. The local APIC is one
 * per CPU, built into the processor, and it can do both - which is why SMP
 * starts here rather than with the trampoline.
 *
 * Registers are memory-mapped at a 4 KiB page the MADT names, usually
 * 0xFEE00000, and every one is 32 bits aligned to 16. The mapping must be
 * uncached: a cached read of the interrupt command register can return a
 * value the APIC has already changed.
 *
 * The 8259s are not removed. They stay masked after this, because the MADT's
 * PCAT_COMPAT flag says they exist and an unmasked legacy PIC will keep
 * delivering interrupts on vectors the kernel has reassigned.
 */
#ifndef _ARCH_APIC_H
#define _ARCH_APIC_H

#include <kernel/types.h>

/* Register offsets, in bytes from the APIC base. */
#define APIC_ID              0x020 /* this CPU's APIC id, in bits 24-31      */
#define APIC_VERSION         0x030
#define APIC_TPR             0x080 /* task priority: 0 accepts everything    */
#define APIC_EOI             0x0B0 /* write anything to acknowledge          */
#define APIC_LDR             0x0D0
#define APIC_DFR             0x0E0
#define APIC_SPURIOUS        0x0F0 /* bit 8 is the software enable           */
#define APIC_ESR             0x280 /* error status, write-to-clear           */
#define APIC_ICR_LOW         0x300 /* writing this sends the IPI             */
#define APIC_ICR_HIGH        0x310 /* destination APIC id, in bits 24-31     */
#define APIC_LVT_TIMER       0x320
#define APIC_LVT_THERMAL     0x330
#define APIC_LVT_PERF        0x340
#define APIC_LVT_LINT0       0x350
#define APIC_LVT_LINT1       0x360
#define APIC_LVT_ERROR       0x370
#define APIC_TIMER_ICR       0x380 /* initial count                          */
#define APIC_TIMER_CCR       0x390 /* current count                          */
#define APIC_TIMER_DIV       0x3E0

/* APIC_SPURIOUS */
#define APIC_SW_ENABLE       0x100

/* Local vector table entries */
#define APIC_LVT_MASKED      0x10000
#define APIC_LVT_PERIODIC    0x20000

/* LVT delivery modes, in bits 8-10. Two of them matter for LINT0/LINT1:
 *
 *   ExtINT  "whatever is on this pin came from an external interrupt
 *           controller; take the vector from it, not from this register".
 *           This is how a legacy 8259 keeps working once the local APIC is
 *           enabled - a wiring arrangement the firmware sets up and calls
 *           virtual wire mode. Masking LINT0 instead cuts the 8259 off
 *           entirely, which on this kernel means the timer stops.
 *
 *   NMI     what LINT1 is wired to on every PC.
 */
#define APIC_LVT_EXTINT      0x700
#define APIC_LVT_NMI         0x400

/* APIC_ICR_LOW: delivery modes */
#define APIC_ICR_FIXED       0x00000
#define APIC_ICR_INIT        0x00500
#define APIC_ICR_STARTUP     0x00600
/* ...and the rest of the command word */
#define APIC_ICR_PHYSICAL    0x00000
#define APIC_ICR_ASSERT      0x04000
#define APIC_ICR_DEASSERT    0x00000
#define APIC_ICR_LEVEL       0x08000
#define APIC_ICR_EDGE        0x00000
#define APIC_ICR_PENDING     0x01000 /* read-only: delivery in progress   */
#define APIC_ICR_ALL_BUT_ME  0xC0000

/* Vectors this kernel uses above the 8259 range (32-47). */
#define APIC_VECTOR_SPURIOUS 0xFF
#define APIC_VECTOR_ERROR    0xFE
#define APIC_VECTOR_IPI_PING 0xF0 /* "are you there", for the tests      */
#define APIC_VECTOR_IPI_TLB  0xF1 /* TLB shootdown                       */
#define APIC_VECTOR_IPI_HALT 0xF2 /* stop, for panic                     */

/* Map and enable this CPU's local APIC. Called once on the boot processor,
 * which maps the registers, and then once on each application processor,
 * which reuses the mapping. Returns false if there is no usable APIC. */
bool apic_init_bsp(void);
void apic_init_ap(void);

bool apic_available(void);
u32 apic_id(void);
u32 apic_version(void);

void apic_write(u32 reg, u32 value);
u32 apic_read(u32 reg);

/* Acknowledge an interrupt that arrived through the APIC. An interrupt that
 * came from the 8259s must be acknowledged there instead - sending an EOI to
 * the wrong controller leaves a line asserted forever. */
void apic_eoi(void);

/* Send an interrupt to one CPU by APIC id, or to every CPU except this one.
 * Both wait for the delivery to be accepted before returning, because the
 * command register holds one IPI at a time. */
void apic_send_ipi(u8 apic_id, u32 vector);
void apic_broadcast_ipi(u32 vector);

/* The INIT-SIPI-SIPI sequence that takes a processor out of reset. `vector`
 * is the page of physical memory it begins executing in real mode, so the
 * trampoline has to be below 1 MiB and page aligned. */
void apic_start_ap(u8 apic_id, u8 vector);

struct apic_stats {
    u32 ipis_sent;
    u32 eois;
    u32 spurious;
    u32 errors;
};
void apic_get_stats(struct apic_stats *out);

#endif /* _ARCH_APIC_H */
