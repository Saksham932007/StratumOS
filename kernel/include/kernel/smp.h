/* StratumOS - symmetric multiprocessing: per-CPU state, and getting there.
 *
 * What "SMP" means here, precisely, because the phrase covers a lot of
 * ground:
 *
 *   - every processor the firmware reports is taken out of reset, brought
 *     through real mode into the higher half, given its own GDT entry, TSS,
 *     stack and local APIC configuration, and runs kernel code;
 *   - `spinlock_t` is a real spinlock rather than an interrupt mask;
 *   - processors can interrupt each other, and TLB shootdown works;
 *   - the *scheduler* still runs only on the boot processor. Application
 *     processors park in their own idle loop and service interrupts.
 *
 * That last line is the honest boundary of this phase. Per-CPU run queues are
 * a separate change with its own set of ways to be subtly wrong, and claiming
 * them before they exist would be the kind of overstatement this project was
 * started to correct. See docs/SMP.md.
 *
 * Identifying the current CPU
 * ---------------------------
 * By reading the task register, which is already per-CPU and already holds a
 * distinct value on every processor: each one executed `ltr SEL_TSS(cpu)`
 * with its own selector, because each needs its own TSS for ss0/esp0. So the
 * task register is a per-CPU identity the kernel was obliged to set up
 * anyway, and `str` reads it back in one instruction that touches no memory.
 *
 * The first version read the local APIC's id register instead. That is the
 * obvious thing to do, and it is 125 times more expensive, because it is an
 * uncached access to a device:
 *
 *     cpu-index    453 cycles  ->    3.6
 *     spinlock     580 cycles  ->   68      (two cpu-index calls each)
 *     ctxsw        886 cycles  ->  580      (tss_set_kernel_stack)
 *
 * Medians under emulation, so the absolute figures are the emulator's - but
 * the ratio is the real finding, and it is on the context-switch path,
 * because tss_set_kernel_stack() has to write *this* processor's TSS. The
 * `cpu-index` and `spinlock` benchmarks exist because the cost was a design
 * decision worth measuring rather than asserting.
 *
 * The textbook alternative is a per-CPU GDT descriptor whose base points at
 * the `struct cpu`, reached as `%gs:offset`. That is what Linux does and it
 * is better still, because it gets the whole structure rather than an index.
 * It also needs the interrupt stubs to load a per-CPU GS on every kernel
 * entry, which needs the processor already identified - and `str` is how you
 * would break that circle. It is not needed yet.
 */
#ifndef _KERNEL_SMP_H
#define _KERNEL_SMP_H

#include <arch/gdt.h>

#include <kernel/types.h>

/* Per-CPU state. One of these per processor, in a plain array: an array
 * indexed by a dense id is addressable from any CPU, which matters for the
 * IPI paths, and there are at most SMP_MAX_CPUS of them. */
struct cpu {
    bool present; /* the firmware reported it                    */
    bool online;  /* it reached C and said so                    */
    bool is_bsp;
    u32 index; /* dense, 0..n-1; the BSP is not always 0      */
    u8 apic_id;
    u32 stack_top; /* its own kernel stack                        */

    /* Counters, all written only by the CPU they describe except where
     * noted, so they need no lock. */
    volatile u32 ipi_ping;   /* ping IPIs serviced                 */
    volatile u32 ipi_tlb;    /* TLB shootdowns serviced            */
    volatile u32 interrupts; /* interrupts serviced                */
    volatile u64 idle_loops; /* times round the idle loop          */
};

/* Discover the processors, start them, and wait for them to come online.
 * Needs ACPI, the local APIC, the VMM and the GDT. Safe to call on a machine
 * with one processor, or no ACPI at all, in which case it reports that and
 * leaves the kernel exactly as it was. */
void smp_init(void);

bool smp_enabled(void);     /* more than one processor is online  */
u32 smp_cpu_count(void);    /* online, including the BSP          */
u32 smp_cpus_present(void); /* reported by firmware               */

/* This CPU's dense index, from the task register. See the file header.
 *
 * Valid before smp_init(), and before `ltr` has ever been executed, where it
 * answers 0 - which is correct rather than merely harmless, because until the
 * second processor exists there is only one answer it could give. gdt_init()
 * itself calls this, so the early answer has to be right. */
static inline u32 smp_cpu_index(void)
{
    u16 tr;

    /* STR stores the visible selector half of the task register. Unlike LTR
     * it is not privileged, and unlike the local APIC's id register it is not
     * a memory access at all. */
    __asm__("str %0" : "=rm"(tr));

    u32 index = (u32)(tr >> 3);

    /* Before any `ltr`, TR is the null selector. After one, it names a TSS
     * descriptor at GDT_TSS_FIRST + cpu. Anything else means the task
     * register holds something this kernel did not put there, and answering
     * 0 is the only safe reading. */
    if (index < GDT_TSS_FIRST || index >= GDT_TSS_FIRST + SMP_MAX_CPUS)
        return 0;

    return index - GDT_TSS_FIRST;
}
struct cpu *smp_this_cpu(void);
const struct cpu *smp_cpu(u32 index);

/* The entry point an application processor jumps to. Not called from C;
 * declared so that the trampoline's address can be taken. */
void ap_main(void);

/* ---- inter-processor interrupts ---------------------------------------- */

/* Make every other processor take an interrupt and bump its own counter.
 * Exists to prove the path works, which is worth having explicitly rather
 * than inferring it from TLB shootdown appearing to behave. */
void smp_ping_others(void);

/* Invalidate one page on every other processor.
 *
 * A TLB is per-CPU, so unmapping a page on this processor leaves every other
 * processor's cached translation for it intact - and still usable. Any change
 * that removes or narrows a mapping another CPU could be using has to tell
 * them, and this is how. */
void smp_tlb_shootdown(vaddr_t va);

/* Stop every other processor. Used by panic(): a kernel that has decided it
 * cannot continue should not leave three other CPUs running in the state that
 * made it decide that. */
void smp_halt_others(void);

struct smp_stats {
    u32 pings_sent;
    u32 shootdowns_sent;
    u32 shootdowns_served;
    u32 start_failures;
};
void smp_get_stats(struct smp_stats *out);

#endif /* _KERNEL_SMP_H */
