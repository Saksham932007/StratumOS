/* StratumOS - timer-driven sampling profiler.
 *
 * The timer interrupt already fires 100 times a second and already has the
 * interrupted EIP sitting in its register frame. Attributing that EIP to a
 * function and counting it costs one binary search over the symbol table, so
 * a statistical profiler is almost free - it just needs somewhere to put the
 * counts.
 *
 * This is how a real profiler works, at a cruder sample rate: `perf record`
 * samples on a performance-counter overflow rather than a timer tick, but the
 * principle - periodically ask "where is the instruction pointer?" - is the
 * same, and so is the bias. Samples land where time is spent, which is not
 * the same as where the work is.
 *
 * Limits worth stating: 100 Hz means ~100 samples per second, so a profile
 * needs seconds of runtime to say anything, and anything shorter than ~10 ms
 * is invisible. Samples taken inside the timer handler itself are excluded,
 * since otherwise the profiler is mostly a picture of itself.
 */
#ifndef _KERNEL_PROFILE_H
#define _KERNEL_PROFILE_H

#include <arch/idt.h>

#include <kernel/types.h>

bool profile_start(void);
void profile_stop(void);
void profile_reset(void);
bool profile_active(void);

/* Called from the timer interrupt with the interrupted frame. */
void profile_tick(const struct regs *r);

/* Print the hottest `top` functions, or all of them if `top` is 0. */
void profile_report(u32 top);

struct profile_stats {
    u32 samples;      /* attributed to a known function      */
    u32 unknown;      /* in .text but no symbol, or outside  */
    u32 user_samples; /* taken while the CPU was in ring 3   */
    u32 idle_samples; /* taken in the idle task              */
};
void profile_get_stats(struct profile_stats *out);

#endif /* _KERNEL_PROFILE_H */
