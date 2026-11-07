/* StratumOS - the network subsystem's lock.
 *
 * See kernel/include/net/netlock.h for what it covers, and why this is the
 * one lock in the kernel that counts its depth.
 */
#include <kernel/smp.h>
#include <kernel/spinlock.h>

#include <net/netlock.h>

static spinlock_t lock = SPINLOCK_INIT_AT("net", LOCK_LEVEL_NET);

/* How deep this processor is inside the stack. Written only by the processor
 * it describes, and only while that processor holds the lock - which means
 * with interrupts masked - so it needs no protection of its own. */
static u32 depth[SMP_MAX_CPUS];

void net_lock(void)
{
    /* The check is safe without the lock. If this processor does hold it,
     * interrupts are masked and nothing can change underneath; if it does
     * not, the answer is false and spin_lock() settles the race. */
    if (spin_held_by_self(&lock)) {
        depth[smp_cpu_index() % SMP_MAX_CPUS]++;
        return;
    }

    spin_lock(&lock);
    depth[smp_cpu_index() % SMP_MAX_CPUS] = 1;
}

void net_unlock(void)
{
    u32 self = smp_cpu_index() % SMP_MAX_CPUS;

    if (depth[self] > 1) {
        depth[self]--;
        return;
    }

    depth[self] = 0;
    spin_unlock(&lock);
}

bool net_lock_held(void)
{
    return spin_held_by_self(&lock);
}

u32 net_sleep_begin(void)
{
    if (!spin_held_by_self(&lock))
        return 0;

    u32 self = smp_cpu_index() % SMP_MAX_CPUS;
    u32 held = depth[self];

    depth[self] = 0;
    spin_unlock(&lock);
    return held;
}

void net_sleep_end(u32 held)
{
    if (!held)
        return;

    spin_lock(&lock);
    depth[smp_cpu_index() % SMP_MAX_CPUS] = held;
}
