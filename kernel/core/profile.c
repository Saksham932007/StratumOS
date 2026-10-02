/* StratumOS - timer-driven sampling profiler.
 *
 * See kernel/include/kernel/profile.h for the method and its biases.
 */
#define LOG_TAG "profile"

#include <arch/io.h>

#include <drivers/timer.h>

#include <kernel/ksyms.h>
#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/profile.h>
#include <kernel/sched.h>
#include <kernel/string.h>

#include <mm/heap.h>

static u32 *buckets; /* one counter per symbol */
static u32 bucket_count;
static volatile bool active;
static struct profile_stats stats;
static u64 started_at_tick;
static u64 stopped_at_tick;

bool profile_active(void)
{
    return active;
}

bool profile_start(void)
{
    if (!ksyms_available()) {
        pr_err("no symbol table, so samples cannot be attributed");
        return false;
    }

    if (!buckets) {
        bucket_count = ksym_count;
        buckets = kcalloc(bucket_count, sizeof(u32));
        if (!buckets) {
            pr_err("cannot allocate %u sample counters", bucket_count);
            return false;
        }
    }

    profile_reset();
    started_at_tick = timer_ticks();
    active = true;
    return true;
}

void profile_stop(void)
{
    stopped_at_tick = timer_ticks();
    active = false;
}

void profile_reset(void)
{
    bool irqs = irq_save();

    if (buckets)
        memset(buckets, 0, bucket_count * sizeof(u32));
    memset(&stats, 0, sizeof(stats));
    started_at_tick = timer_ticks();
    stopped_at_tick = 0;

    irq_restore(irqs);
}

void profile_tick(const struct regs *r)
{
    if (!active || !buckets)
        return;

    /* A trap from ring 3 cannot be attributed to a kernel symbol, and
     * pretending otherwise would blame whatever kernel function happens to
     * sit at that address. */
    if ((r->cs & 3) != 0) {
        stats.user_samples++;
        return;
    }

    struct task *cur = task_current();
    if (cur && cur->pid == 0) {
        /* Time in the idle task is time the machine had nothing to do. It is
         * worth knowing, but it is not a hotspot. */
        stats.idle_samples++;
        return;
    }

    int idx = ksym_index(r->eip);
    if (idx < 0) {
        stats.unknown++;
        return;
    }

    buckets[idx]++;
    stats.samples++;
}

void profile_get_stats(struct profile_stats *out)
{
    if (out)
        *out = stats;
}

void profile_report(u32 top)
{
    if (!buckets) {
        kprintf("profiler has never been started\n");
        return;
    }

    u64 end = stopped_at_tick ? stopped_at_tick : timer_ticks();
    u64 elapsed_ticks = (end > started_at_tick) ? end - started_at_tick : 0;
    u32 hz = timer_hz();
    u32 total = stats.samples;

    kprintf("Sampling profile%s\n", active ? " (still running)" : "");
    kprintf("  sample rate   : %u Hz (the timer tick)\n", hz);
    kprintf("  duration      : %llu ticks", elapsed_ticks);
    if (hz)
        kprintf(" (~%llu ms)", (elapsed_ticks * 1000) / hz);
    kprintf("\n");
    kprintf("  kernel samples: %u attributed, %u unattributed\n", total,
            stats.unknown);
    kprintf("  ring 3        : %u\n", stats.user_samples);
    kprintf("  idle          : %u\n", stats.idle_samples);

    if (total == 0) {
        kprintf(
            "\n  No kernel samples. Either nothing ran while profiling, or\n");
        kprintf("  the machine was idle the whole time - try running a\n");
        kprintf("  workload between 'profile start' and 'profile stop'.\n");
        return;
    }

    if (top == 0 || top > bucket_count)
        top = bucket_count;

    kprintf("\n  %7s  %6s  %s\n", "SAMPLES", "SHARE", "FUNCTION");

    /* Selection of the top N rather than a full sort: N is small, the table
     * is a few hundred entries, and this needs no scratch allocation. */
    u32 shown = 0;
    u32 ceiling = 0xFFFFFFFFu;

    while (shown < top) {
        u32 best_count = 0;
        int best = -1;

        for (u32 i = 0; i < bucket_count; i++) {
            if (buckets[i] == 0 || buckets[i] > ceiling)
                continue;
            if (buckets[i] > best_count) {
                best_count = buckets[i];
                best = (int)i;
            }
        }

        if (best < 0)
            break;

        /* Print every symbol tied at this count before lowering the ceiling,
         * so ties are not silently dropped. */
        for (u32 i = 0; i < bucket_count && shown < top; i++) {
            if (buckets[i] != best_count)
                continue;

            u32 permille = (u32)(((u64)buckets[i] * 1000) / total);
            kprintf("  %7u  %3u.%01u%%  %s\n", buckets[i], permille / 10,
                    permille % 10, ksym_table[i].name);
            shown++;
        }

        if (best_count == 0)
            break;
        ceiling = best_count - 1;
    }
}
