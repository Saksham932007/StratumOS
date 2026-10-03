/* StratumOS - in-kernel microbenchmarks.
 *
 * See kernel/include/kernel/bench.h for the measurement methodology and its
 * one important caveat (emulated cycle counts are the emulator's, not the
 * CPU's).
 */
#define LOG_TAG "bench"

#include <arch/cpu.h>
#include <arch/io.h>

#include <drivers/timer.h>

#include <kernel/bench.h>
#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/smp.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/syscall.h>

#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

/* How many timer ticks to measure the TSC across. 20 ticks at 100 Hz is
 * 200 ms - long enough that the ±1 tick quantisation is under 1% error. */
#define CALIB_TICKS 20

static u64 tsc_hz;
static u64 harness_overhead; /* cycles for an empty sample, subtracted */

u64 bench_tsc_hz(void)
{
    return tsc_hz;
}

void bench_calibrate(void)
{
    if (tsc_hz)
        return;

    const struct cpu_info *ci = cpu_get_info();

    if (!ci->has_tsc) {
        pr_err("this CPU has no time-stamp counter; cannot benchmark");
        return;
    }

    /* Align to a tick boundary first. Starting mid-tick would put a whole
     * tick of uncertainty into a 20-tick measurement. */
    bool irqs_were_on = irq_enabled();
    sti();

    u64 edge = timer_ticks();
    while (timer_ticks() == edge)
        ;

    u64 t0 = timer_ticks();
    u64 c0 = rdtsc();

    while (timer_ticks() - t0 < CALIB_TICKS)
        ;

    u64 elapsed_ticks = timer_ticks() - t0;
    u64 elapsed_cycles = rdtsc() - c0;

    if (!irqs_were_on)
        cli();

    /* elapsed_seconds = ticks * 1000 / actual_hz_milli, so
     * tsc_hz = cycles * actual_hz_milli / (ticks * 1000). */
    u32 hz_milli = timer_actual_hz_milli();
    if (!elapsed_ticks || !hz_milli) {
        pr_err("TSC calibration failed (no timer progress)");
        return;
    }

    tsc_hz = (elapsed_cycles * hz_milli) / (elapsed_ticks * 1000);

    pr_info("TSC calibrated at %llu.%03llu MHz over %llu ticks",
            tsc_hz / 1000000, (tsc_hz / 1000) % 1000, elapsed_ticks);
}

/* Picoseconds, not nanoseconds: a single cycle on a 3 GHz part is 333 ps, and
 * integer nanoseconds would round sub-nanosecond operations to zero. */
u64 bench_cycles_to_ps(u64 cycles)
{
    if (!tsc_hz)
        return 0;
    return (cycles * 1000000000000ull) / tsc_hz;
}

/* ---- sample collection ------------------------------------------------- */

static void sort_u64(u64 *a, u32 n)
{
    /* Insertion sort: n is BENCH_SAMPLES, so anything cleverer is noise. */
    for (u32 i = 1; i < n; i++) {
        u64 key = a[i];
        u32 j = i;
        while (j > 0 && a[j - 1] > key) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

static void bench_empty(u32 iterations)
{
    for (u32 i = 0; i < iterations; i++)
        __asm__ volatile("" ::: "memory");
}

struct sample_set {
    u64 min, median, mean;
};

static void collect(const struct bench *b, struct sample_set *out)
{
    u64 samples[BENCH_SAMPLES];

    for (u32 s = 0; s < BENCH_SAMPLES; s++) {
        bool irqs = false;

        if (!b->needs_interrupts)
            irqs = irq_save();

        /* Serialise before and after so the TSC read is not reordered across
         * the workload. CPUID is the architecturally defined way to do this
         * on a CPU without RDTSCP. */
        u32 a, bb, c, d;
        cpuid_raw(0, 0, &a, &bb, &c, &d);
        u64 start = rdtsc();

        b->fn(b->iterations);

        cpuid_raw(0, 0, &a, &bb, &c, &d);
        u64 end = rdtsc();

        if (!b->needs_interrupts)
            irq_restore(irqs);

        samples[s] = (end > start) ? (end - start) : 0;
    }

    sort_u64(samples, BENCH_SAMPLES);

    u64 total = 0;
    for (u32 s = 0; s < BENCH_SAMPLES; s++)
        total += samples[s];

    u32 ops = b->iterations * (b->ops_per_iteration ? b->ops_per_iteration : 1);

    /* Subtract the harness's own cost, then divide by the work done. Clamp at
     * zero: an operation cheaper than the measurement overhead reports as 0
     * rather than as an enormous unsigned number. */
    u64 min = samples[0];
    u64 med = samples[BENCH_SAMPLES / 2];
    u64 mean = total / BENCH_SAMPLES;

    min = (min > harness_overhead) ? min - harness_overhead : 0;
    med = (med > harness_overhead) ? med - harness_overhead : 0;
    mean = (mean > harness_overhead) ? mean - harness_overhead : 0;

    /* Scale by 1000 so the per-operation figure keeps three fractional
     * digits without floating point. */
    out->min = (min * 1000) / ops;
    out->median = (med * 1000) / ops;
    out->mean = (mean * 1000) / ops;
}

/* ---- the workloads ----------------------------------------------------- */

static void bench_syscall(u32 iterations)
{
    /* int 0x80 from ring 0 takes exactly the same path as from ring 3, minus
     * the stack switch, so this measures the gate, the stub, the dispatch and
     * the iret. */
    for (u32 i = 0; i < iterations; i++) {
        i32 ret;
        __asm__ volatile("int $0x80"
                         : "=a"(ret)
                         : "a"(SYS_GETPID), "b"(0), "c"(0), "d"(0)
                         : "memory");
    }
}

static volatile bool pingpong_stop;
static volatile u32 pingpong_rounds;

static void pingpong_partner(void *arg)
{
    UNUSED(arg);

    /* Exists only to be switched to. Every yield here hands the CPU straight
     * back to the benchmark task. */
    while (!pingpong_stop) {
        pingpong_rounds++;
        sched_yield();
    }
}

static void bench_ctxsw(u32 iterations)
{
    /* One sched_yield() switches away and back: two context switches, which
     * is what ops_per_iteration accounts for. */
    for (u32 i = 0; i < iterations; i++)
        sched_yield();
}

static void bench_kmalloc_fixed(u32 iterations)
{
    for (u32 i = 0; i < iterations; i++) {
        void *p = kmalloc(64);
        kfree(p);
    }
}

static void bench_kmalloc_mixed(u32 iterations)
{
    /* Sizes that straddle the split threshold, so the allocator actually has
     * to split and coalesce rather than reusing one block forever. */
    static const u32 sizes[8] = {24, 160, 48, 900, 32, 512, 96, 1400};

    for (u32 i = 0; i < iterations; i++) {
        void *p = kmalloc(sizes[i & 7]);
        kfree(p);
    }
}

static void bench_pmm(u32 iterations)
{
    for (u32 i = 0; i < iterations; i++) {
        paddr_t f = pmm_alloc_frame();
        if (f != PMM_NO_FRAME)
            pmm_free_frame(f);
    }
}

static void bench_vmm_map(u32 iterations)
{
    const vaddr_t scratch = 0xC6000000u;
    paddr_t frame = pmm_alloc_frame();

    if (frame == PMM_NO_FRAME)
        return;

    for (u32 i = 0; i < iterations; i++) {
        vmm_map(scratch, frame, PTE_PRESENT | PTE_WRITE);
        vmm_unmap(scratch);
    }

    pmm_free_frame(frame);
}

static void bench_vmm_translate(u32 iterations)
{
    paddr_t out;

    for (u32 i = 0; i < iterations; i++)
        vmm_translate((vaddr_t)__kernel_start + (i & 0xFFF), &out);
}

static u8 bench_buf_a[4096];
static u8 bench_buf_b[4096];

static void bench_memcpy(u32 iterations)
{
    for (u32 i = 0; i < iterations; i++)
        memcpy(bench_buf_a, bench_buf_b, sizeof(bench_buf_a));
}

static void bench_memset(u32 iterations)
{
    for (u32 i = 0; i < iterations; i++)
        memset(bench_buf_a, (int)i, sizeof(bench_buf_a));
}

static void bench_printf(u32 iterations)
{
    char buf[64];

    /* Formatting only - no console I/O, which would measure the UART. */
    for (u32 i = 0; i < iterations; i++)
        ksnprintf(buf, sizeof(buf), "%s %d %08x %llu", "x", (int)i, i,
                  (u64)i * 1000003ull);
}

static void bench_heap_check(u32 iterations)
{
    for (u32 i = 0; i < iterations; i++)
        (void)heap_check();
}

/* Identifying the current processor.
 *
 * This is here because the cost is a design decision, not an accident.
 * smp_cpu_index() reads the local APIC's id register, which is an uncached
 * memory access to a device - and it is on the context-switch path, because
 * tss_set_kernel_stack() has to write *this* processor's TSS.
 *
 * The alternative is a per-CPU GDT descriptor whose base points at the
 * `struct cpu`, reached as `%gs:offset` in one instruction. That is what a
 * production kernel does, and it needs the interrupt stubs to load a per-CPU
 * GS on every kernel entry, which needs the processor already identified.
 * Breaking that circle is worth doing; knowing what it would buy means
 * measuring what the simple version costs rather than asserting it is fine.
 */
/* A volatile sink, so the optimiser cannot delete the thing being measured.
 * Without it GCC observes that the result is unused and removes the call. */
static volatile u32 bench_sink;

static void bench_cpu_index(u32 iterations)
{
    for (u32 i = 0; i < iterations; i++)
        bench_sink = smp_cpu_index();
}

/* An uncontended lock, which is what nearly every acquisition in this kernel
 * is. The interesting number is how much the atomic compare-exchange and the
 * interrupt save/restore cost when nobody is competing - because that is the
 * price paid on every acquisition for the one in a thousand that contends. */
static void bench_spinlock(u32 iterations)
{
    static spinlock_t bench_lock = SPINLOCK_INIT("bench");

    for (u32 i = 0; i < iterations; i++) {
        spin_lock(&bench_lock);
        spin_unlock(&bench_lock);
    }
}

/* A TLB shootdown: an IPI broadcast plus the wait for every other processor
 * to acknowledge. On one processor this measures the local invalidation and
 * the early return, which is itself worth knowing - it is the cost a
 * uniprocessor pays for code written to be correct on many. */
static void bench_shootdown(u32 iterations)
{
    for (u32 i = 0; i < iterations; i++)
        smp_tlb_shootdown(KERNEL_VIRT_BASE + ((i & 0xFF) << PAGE_SHIFT));
}

/* ---- registry ---------------------------------------------------------- */

static const struct bench benches[] = {
    {"syscall", "int 0x80 round trip (SYS_GETPID)", bench_syscall, 2000, false,
     1},
    {"ctxsw", "context switch via sched_yield", bench_ctxsw, 1000, true, 2},
    {"kmalloc", "kmalloc(64) + kfree", bench_kmalloc_fixed, 2000, false, 1},
    {"kmalloc-mx", "kmalloc/kfree, 8 mixed sizes", bench_kmalloc_mixed, 2000,
     false, 1},
    {"pmm", "physical frame alloc + free", bench_pmm, 2000, false, 1},
    {"vmm-map", "map + unmap one 4 KiB page", bench_vmm_map, 1000, false, 2},
    {"vmm-xlate", "virtual to physical translation", bench_vmm_translate, 5000,
     false, 1},
    {"memcpy-4k", "memcpy of 4 KiB", bench_memcpy, 500, false, 1},
    {"memset-4k", "memset of 4 KiB", bench_memset, 500, false, 1},
    {"ksnprintf", "format 4 conversions to a buffer", bench_printf, 2000, false,
     1},
    {"heapcheck", "full heap integrity walk", bench_heap_check, 200, false, 1},
    {"cpu-index", "identify the current processor (an APIC register read)",
     bench_cpu_index, 2000, false, 1},
    {"spinlock", "uncontended spin_lock + spin_unlock", bench_spinlock, 2000,
     false, 2},
    {"shootdown", "TLB shootdown, including every ack", bench_shootdown, 200,
     true, 1},
};

u32 bench_count(void)
{
    return ARRAY_SIZE(benches);
}

void bench_list(void)
{
    kprintf("%-12s %-34s %8s\n", "BENCHMARK", "WHAT IT MEASURES", "ITERS");
    for (size_t i = 0; i < ARRAY_SIZE(benches); i++)
        kprintf("%-12s %-34s %8u\n", benches[i].name, benches[i].description,
                benches[i].iterations);
}

static void print_platform(void)
{
    const struct cpu_info *ci = cpu_get_info();

    kprintf("bench: tsc %llu.%03llu MHz   cpu \"%s\"\n", tsc_hz / 1000000,
            (tsc_hz / 1000) % 1000, ci->brand[0] ? ci->brand : ci->vendor);

    if (ci->has_hypervisor) {
        /* Saying this plainly is the difference between a benchmark and a
         * misleading number. Under TCG these are the emulator's costs. */
        kprintf("bench: NOTE running under a hypervisor/emulator - these are "
                "emulated costs,\n");
        kprintf("bench: NOTE not silicon timings. Use KVM or real hardware "
                "for absolute figures;\n");
        kprintf("bench: NOTE the relative ordering is still informative.\n");
    } else {
        kprintf("bench: bare metal - figures are real silicon timings\n");
    }
}

/* Cycles are reported scaled by 1000; render as a fixed-point figure. */
static void print_scaled(u64 scaled, const char *suffix)
{
    kprintf("%llu.%03llu%s", scaled / 1000, scaled % 1000, suffix);
}

static void run_one(const struct bench *b)
{
    struct sample_set r;
    struct task *partner = NULL;

    /* The context-switch benchmark needs something to switch to. */
    if (b->fn == bench_ctxsw) {
        pingpong_stop = false;
        pingpong_rounds = 0;
        partner = task_create("bench-pong", pingpong_partner, NULL);
        if (!partner) {
            kprintf("bench: %-12s SKIP (could not create a partner task)\n",
                    b->name);
            return;
        }
        /* Let it reach its first yield so the first measured switch is not
         * also the task's first-ever schedule. */
        sched_yield();
    }

    collect(b, &r);

    if (partner) {
        pingpong_stop = true;
        /* Let it observe the flag and exit. */
        for (int i = 0; i < 20; i++)
            sched_yield();
    }

    kprintf("bench: %-12s ", b->name);
    kprintf("min=");
    print_scaled(r.min, "c");
    kprintf(" med=");
    print_scaled(r.median, "c");
    kprintf(" mean=");
    print_scaled(r.mean, "c");

    if (tsc_hz) {
        u64 ps = bench_cycles_to_ps(r.median) / 1000; /* median is x1000 */
        kprintf("  = %llu.%03llu ns", ps / 1000, ps % 1000);
    }

    kprintf("\n");
}

static void measure_overhead(void)
{
    struct bench empty = {"overhead", "", bench_empty, 2000, false, 1};
    struct sample_set r;

    harness_overhead = 0;
    collect(&empty, &r);

    /* collect() divided by the iteration count and scaled by 1000; undo both
     * to recover the absolute cost of one sample. */
    harness_overhead = (r.min * 2000) / 1000;

    kprintf("bench: harness overhead %llu cycles per sample (subtracted)\n",
            harness_overhead);
}

unsigned bench_run_all(void)
{
    bench_calibrate();
    if (!tsc_hz) {
        kprintf("bench: cannot run without a calibrated TSC\n");
        return 1;
    }

    print_platform();
    measure_overhead();
    kprintf("bench: %u benchmarks, median of %u samples each\n",
            (unsigned)ARRAY_SIZE(benches), (unsigned)BENCH_SAMPLES);

    for (size_t i = 0; i < ARRAY_SIZE(benches); i++)
        run_one(&benches[i]);

    kprintf("bench: complete\n");
    return 0;
}

unsigned bench_run_one(const char *name)
{
    bench_calibrate();
    if (!tsc_hz) {
        kprintf("bench: cannot run without a calibrated TSC\n");
        return 1;
    }

    for (size_t i = 0; i < ARRAY_SIZE(benches); i++) {
        if (strcmp(benches[i].name, name) == 0) {
            print_platform();
            measure_overhead();
            run_one(&benches[i]);
            return 0;
        }
    }

    kprintf("bench: no benchmark named '%s'\n", name);
    return 1;
}
