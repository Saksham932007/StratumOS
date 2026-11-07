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
 *
 * The hierarchy
 * -------------
 * Two locks taken in opposite orders by two processors deadlock, and nothing
 * about either acquisition looks wrong at the point it is written - which is
 * why a documented lock order is not enough. Every lock that guards a
 * subsystem carries a *level*, and a processor may only acquire a lock whose
 * level is strictly greater than every level it already holds. A violation
 * panics at the second acquisition, naming both locks, instead of hanging
 * later on a machine with more processors than the author had.
 *
 * The levels are the order the subsystems call each other in, which is a
 * tree and not a coincidence: the scheduler allocates memory, the heap grows
 * through the VMM, the VMM takes frames from the PMM, and anything at all
 * may want to print.
 *
 *     1  sched     tasks, states, run queues
 *     2  fs        descriptors, the VFS, FAT, the block layer
 *     3  net       the stack and the controller
 *     4  heap      kmalloc's arena
 *     5  vmm       page tables and address spaces
 *     6  pmm       the frame bitmap and its reference counts
 *     7  console   the last thing anyone takes
 *
 * Level 0 means "not in the hierarchy": a lock that no other lock is ever
 * held across. The TLB-shootdown lock, the benchmark's and the test suite's
 * are 0, and they are neither checked nor tracked.
 */
#ifndef _KERNEL_SPINLOCK_H
#define _KERNEL_SPINLOCK_H

#include <arch/io.h>

#include <kernel/types.h>

/* The hierarchy. See the file header. */
#define LOCK_LEVEL_NONE    0
#define LOCK_LEVEL_SCHED   1
#define LOCK_LEVEL_FS      2
#define LOCK_LEVEL_NET     3
#define LOCK_LEVEL_HEAP    4
#define LOCK_LEVEL_VMM     5
#define LOCK_LEVEL_PMM     6
#define LOCK_LEVEL_CONSOLE 7

typedef struct {
    volatile u32 locked;
    bool saved_if;
    /* This lock's place in the hierarchy, and the level this processor was at
     * before it was taken - so that unlock can restore it, which works
     * because locks nest last-in-first-out. */
    u8 level;
    u8 prev_level;
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

/* A lock outside the hierarchy: nothing else is ever held across it. */
#define SPINLOCK_INIT(nm)        {0, false, LOCK_LEVEL_NONE, 0, 0, (nm), 0, 0}

/* A lock inside it. `lv` is one of LOCK_LEVEL_*. */
#define SPINLOCK_INIT_AT(nm, lv) {0, false, (lv), 0, 0, (nm), 0, 0}

void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

/* Try once and report. For the paths that have something else to do rather
 * than wait - and for the test suite, which needs to observe a held lock
 * without deadlocking on it. */
bool spin_trylock(spinlock_t *lock);

bool spin_is_locked(const spinlock_t *lock);

/* True if *this* processor is the one holding the lock.
 *
 * It exists for exactly one caller: the console. A panic prints, and the
 * panicking processor may well be holding the console lock already - it is
 * the lock most likely to be held when something goes wrong, because every
 * other subsystem may take it. Spinning on it there would turn a diagnosable
 * fault into a silent hang, and that is the failure mode this kernel keeps
 * refusing. Anything else that wants this is asking for a recursive lock and
 * should restructure instead. */
bool spin_held_by_self(const spinlock_t *lock);

/* The highest hierarchy level this processor currently holds, or 0. For the
 * test suite, and for an assertion in code that must not be entered with a
 * lock held. */
u8 spin_level_held(void);

#endif /* _KERNEL_SPINLOCK_H */
