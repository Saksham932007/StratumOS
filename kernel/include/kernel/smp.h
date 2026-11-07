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
 *   - every subsystem that holds shared state holds a lock to do it, at a
 *     level the machine checks on every acquisition;
 *   - the *scheduler* runs on every processor. Each has its own idle task,
 *     its own `current`, its own timer and its own preemption, and they take
 *     work from one shared run queue.
 *
 * What that last line is *not* is per-processor run queues. The queue is
 * shared and there is no affinity, which is a choice with a reason - see
 * docs/SMP.md - and calling it anything grander would be the kind of
 * overstatement this project was started to correct.
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
struct task; /* kernel/sched.h - only ever pointed at from here */

struct cpu {
    bool present; /* the firmware reported it                    */
    bool online;  /* it reached C and said so                    */
    bool is_bsp;
    u32 index; /* dense, 0..n-1; the BSP is not always 0      */
    u8 apic_id;
    u32 stack_top; /* its own kernel stack                        */

    /* ---- what this processor is running ---------------------------------
     *
     * `current` and `idle` used to be two file-scope variables in sched.c,
     * which is exactly as many as a kernel that schedules on one processor
     * needs. They live here now because they are per-processor facts: two
     * processors run two different tasks at the same instant, and an idle
     * task is a processor's own - it owns that processor's boot stack and
     * cannot be run anywhere else.
     *
     * Written by the processor they describe, under the scheduler's lock,
     * and read by others only for reporting. */
    struct task *current;
    struct task *idle;

    /* The task this processor has just switched away from, which the task it
     * switched *to* is responsible for releasing. The outgoing task stays
     * RUNNING until its stack pointer has actually been saved, because the
     * moment it becomes READY another processor may pick it up - and picking
     * up a task whose context is still being written is the one race a
     * context switch cannot survive. See sched_handoff(). */
    struct task *prev;

    /* Set by this processor's timer tick, or by a reschedule IPI from
     * another processor that has made something runnable. Read at the tail
     * of the interrupt path. */
    volatile bool need_resched;

    /* Context switches performed by this processor. The sum is
     * sched_switch_count(); the split is the evidence that more than one
     * processor is scheduling, which a sum cannot show. */
    volatile u32 switches;

    /* Counters, all written only by the CPU they describe except where
     * noted, so they need no lock. */
    volatile u32 ipi_ping;   /* ping IPIs serviced                 */
    volatile u32 ipi_tlb;    /* TLB shootdowns serviced            */
    volatile u32 interrupts; /* interrupts serviced                */
    volatile u64 idle_loops; /* times round the idle loop          */
    /* Ticks from this processor's *own* local APIC timer. The number
     * existing is the evidence that preemption is per-processor rather than
     * the boot processor's alone: on a machine where it is not, every entry
     * but one stays at zero. */
    volatile u32 timer_ticks;
    volatile u32 ipi_resched; /* reschedule IPIs serviced           */
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

/* The same slot, writable. It exists for the scheduler, which owns the
 * `current`, `idle`, `prev` and `need_resched` fields above and has to be
 * able to set them for a processor other than the one it is running on -
 * sched_init() builds an idle task for every processor that is already
 * waiting. Returns NULL for an index no processor occupies. */
struct cpu *smp_cpu_slot(u32 index);

/* The entry point an application processor jumps to. Not called from C;
 * declared so that the trampoline's address can be taken. */
void ap_main(void);

/* ---- inter-processor interrupts ---------------------------------------- */

/* Make every other processor take an interrupt and bump its own counter.
 * Exists to prove the path works, which is worth having explicitly rather
 * than inferring it from TLB shootdown appearing to behave. */
void smp_ping_others(void);

/* Tell every other processor that the run queue has something in it.
 *
 * Sent whenever a task becomes runnable on a machine where another processor
 * might be asleep in HLT. The receiver only sets its need_resched flag; the
 * switch happens at the tail of its interrupt path, like any other
 * preemption. A no-op on a machine with one processor. */
void smp_reschedule_others(void);

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
    u32 reschedules_sent;
    u32 shootdowns_sent;
    u32 shootdowns_served;
    u32 start_failures;
};
void smp_get_stats(struct smp_stats *out);

#endif /* _KERNEL_SMP_H */
