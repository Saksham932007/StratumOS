/* StratumOS - in-kernel microbenchmarks.
 *
 * Measures the kernel's own hot paths with the time-stamp counter: syscall
 * round trips, context switches, allocator throughput, page-table operations.
 *
 * Two things make the numbers trustworthy rather than decorative:
 *
 *   * The harness measures its own overhead with an empty workload and
 *     subtracts it, so what is reported is the operation and not the loop.
 *   * Every figure is a median of many samples, not a single timing. A single
 *     timing on a machine with interrupts enabled measures whatever happened
 *     to interrupt it.
 *
 * And one caveat that is printed with every run: under emulation these are
 * the emulator's costs, not the silicon's. The harness detects that case and
 * says so, because a benchmark that quietly reports QEMU's timings as hardware
 * numbers is worse than no benchmark.
 */
#ifndef _KERNEL_BENCH_H
#define _KERNEL_BENCH_H

#include <kernel/types.h>

#define BENCH_SAMPLES 24

typedef void (*bench_fn)(u32 iterations);

struct bench {
    const char *name;
    const char *description;
    bench_fn fn;
    u32 iterations;
    /* Some workloads cannot be measured with interrupts masked - a context
     * switch to a partner task needs the scheduler alive. Those accept more
     * noise, which the median then absorbs. */
    bool needs_interrupts;
    /* Divisor applied to the result, for workloads where one call performs
     * several of the operation being measured (a yield is two switches). */
    u32 ops_per_iteration;
};

/* Derive the TSC frequency from the PIT. Takes ~200 ms, so it is done on
 * first use rather than at boot. */
void bench_calibrate(void);
u64 bench_tsc_hz(void);
u64 bench_cycles_to_ps(u64 cycles);

void bench_list(void);
u32 bench_count(void);
unsigned bench_run_all(void);
unsigned bench_run_one(const char *name);

#endif /* _KERNEL_BENCH_H */
