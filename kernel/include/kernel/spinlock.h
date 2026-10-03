/* StratumOS - spinlocks.
 *
 * Two things have to be excluded, and they are not the same thing:
 *
 *   another processor, which is excluded by spinning on a shared word until
 *   an atomic exchange wins it;
 *
 *   an interrupt handler on *this* processor, which is excluded by masking
 *   interrupts - and which spinning cannot help with, because a handler that
 *   interrupts a lock holder and then waits for that lock deadlocks a
 *   processor against itself.
 *
 * So every acquisition does both, in that order's mirror image: interrupts
 * off first, then the word. Taking the lock before masking leaves a window in
 * which an interrupt arrives while the lock is held, and the deadlock that
 * follows is timing-dependent and rare, which is the worst combination.
 *
 * This was an interrupt mask alone until SMP arrived, with a `locked` counter
 * maintained and checked purely so that recursive acquisition would be caught
 * before it could matter. It mattered; the check stayed.
 */
#ifndef _KERNEL_SPINLOCK_H
#define _KERNEL_SPINLOCK_H

#include <arch/io.h>

#include <kernel/types.h>

typedef struct {
    volatile u32 locked;
    bool saved_if;
    /* Which processor holds it, for the recursion check and for diagnosing a
     * deadlock. Only meaningful while `locked` is set. */
    volatile u32 holder;
    const char *name;
    /* Contention, in acquisitions that had to wait. A lock that is never
     * contended is a lock that could be something cheaper; one that always is
     * is a lock that wants splitting. Neither is visible without counting. */
    volatile u32 acquisitions;
    volatile u32 contended;
} spinlock_t;

#define SPINLOCK_INIT(nm)       \
    {                           \
        0, false, 0, (nm), 0, 0 \
    }

void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

/* Try once and report. For the paths that have something else to do rather
 * than wait - and for the test suite, which needs to observe a held lock
 * without deadlocking on it. */
bool spin_trylock(spinlock_t *lock);

bool spin_is_locked(const spinlock_t *lock);

#endif /* _KERNEL_SPINLOCK_H */
