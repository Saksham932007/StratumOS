/* StratumOS - spinlocks.
 *
 * See kernel/spinlock.h for why an acquisition masks interrupts *and* spins,
 * and why the order matters.
 *
 * The atomic primitives are GCC's __atomic builtins rather than hand-written
 * `lock xchg`. On x86 they compile to exactly that - `lock cmpxchg` for the
 * acquire, a plain store with a compiler barrier for the release, because x86
 * does not reorder stores past stores - and using the builtins means the
 * compiler knows what they do. Inline assembly with a "memory" clobber tells
 * it only that something happened.
 */
#include <arch/cpu.h>
#include <arch/io.h>

#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/smp.h>
#include <kernel/spinlock.h>

/* How long to spin before concluding this is a deadlock rather than
 * contention. Generous: a lock held across a disk read under emulation can be
 * held for a long time. The alternative to a bound is a kernel that hangs
 * with no output, which is the failure mode this project keeps refusing. */
#define SPIN_LIMIT 200000000u

/* The highest hierarchy level each processor currently holds, so that an
 * acquisition out of order can be caught at the point it happens. Written
 * only by the processor it describes, and only with interrupts masked, so it
 * needs no lock of its own - which it could not have. */
static u8 held_level[SMP_MAX_CPUS];

/* Record an acquisition and refuse one that would invert the order.
 *
 * Called with the lock already won and interrupts already masked, which is
 * what makes reading and writing held_level[self] safe. Panicking here rather
 * than returning an error is deliberate: an inverted acquisition is a bug in
 * the kernel's structure, and the stack trace at the second lock is the whole
 * diagnosis. */
static void level_enter(spinlock_t *lock, u32 self)
{
    if (lock->level == LOCK_LEVEL_NONE)
        return;

    u8 current = self < SMP_MAX_CPUS ? held_level[self] : 0;

    if (lock->level <= current) {
        /* Release first, so the panic path can take the console lock. */
        __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
        panic("cpu %u tried to acquire lock '%s' at level %u while already "
              "holding level %u; that inverts the lock hierarchy and would "
              "deadlock against a processor doing it the other way round - "
              "see kernel/spinlock.h",
              self, lock->name ? lock->name : "(unnamed)", lock->level,
              current);
    }

    lock->prev_level = current;
    if (self < SMP_MAX_CPUS)
        held_level[self] = lock->level;
}

static void level_leave(const spinlock_t *lock, u32 self)
{
    if (lock->level == LOCK_LEVEL_NONE)
        return;
    if (self < SMP_MAX_CPUS)
        held_level[self] = lock->prev_level;
}

u8 spin_level_held(void)
{
    u32 self = smp_cpu_index();

    return self < SMP_MAX_CPUS ? held_level[self] : 0;
}

bool spin_is_locked(const spinlock_t *lock)
{
    return __atomic_load_n(&lock->locked, __ATOMIC_ACQUIRE) != 0;
}

bool spin_held_by_self(const spinlock_t *lock)
{
    return __atomic_load_n(&lock->locked, __ATOMIC_ACQUIRE) != 0 &&
           __atomic_load_n(&lock->holder, __ATOMIC_RELAXED) == smp_cpu_index();
}

bool spin_trylock(spinlock_t *lock)
{
    bool was_enabled = irq_save();
    u32 expected = 0;

    if (__atomic_compare_exchange_n(&lock->locked, &expected, 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        u32 self = smp_cpu_index();

        __atomic_store_n(&lock->holder, self, __ATOMIC_RELAXED);
        lock->saved_if = was_enabled;
        __atomic_add_fetch(&lock->acquisitions, 1, __ATOMIC_RELAXED);
        level_enter(lock, self);
        return true;
    }

    /* Failed, so this processor is not entering a critical section and must
     * not be left with interrupts masked. */
    irq_restore(was_enabled);
    return false;
}

void spin_lock(spinlock_t *lock)
{
    /* Interrupts off before the word: a handler that interrupts the holder
     * and then wants the same lock would spin forever against itself, on a
     * processor that is the only one able to release it. */
    bool was_enabled = irq_save();
    u32 self = smp_cpu_index();

    u32 expected = 0;

    if (__atomic_compare_exchange_n(&lock->locked, &expected, 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        __atomic_store_n(&lock->holder, self, __ATOMIC_RELAXED);
        lock->saved_if = was_enabled;
        __atomic_add_fetch(&lock->acquisitions, 1, __ATOMIC_RELAXED);
        level_enter(lock, self);
        return;
    }

    /* Contended. Before waiting, rule out the one case where waiting is
     * hopeless: this processor already holds it. With interrupts masked the
     * only way to reach here holding it is a genuine nesting bug in the
     * kernel, and failing now puts the stack trace at the culprit rather than
     * at a hang. */
    if (__atomic_load_n(&lock->holder, __ATOMIC_RELAXED) == self) {
        irq_restore(was_enabled);
        panic("cpu %u recursively acquired lock '%s'", self,
              lock->name ? lock->name : "(unnamed)");
    }

    __atomic_add_fetch(&lock->contended, 1, __ATOMIC_RELAXED);

    for (u32 spin = 0; spin < SPIN_LIMIT; spin++) {
        /* Read before trying to write. An atomic read-modify-write on a
         * contended line bounces the cache line between processors on every
         * attempt; reading until the lock looks free and only then attempting
         * the exchange leaves the line shared while waiting. This is the
         * difference between a test-and-set lock and a test-and-test-and-set
         * one, and on more than two processors it is large. */
        if (!__atomic_load_n(&lock->locked, __ATOMIC_RELAXED)) {
            expected = 0;
            if (__atomic_compare_exchange_n(&lock->locked, &expected, 1, false,
                                            __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED)) {
                __atomic_store_n(&lock->holder, self, __ATOMIC_RELAXED);
                lock->saved_if = was_enabled;
                __atomic_add_fetch(&lock->acquisitions, 1, __ATOMIC_RELAXED);
                level_enter(lock, self);
                return;
            }
        }

        /* PAUSE. It tells the processor this is a spin loop, which on a real
         * machine avoids a memory-order violation penalty when the lock is
         * finally released, and on a hyperthreaded one yields the pipeline to
         * the sibling - which may well be the holder. */
        cpu_relax();
    }

    irq_restore(was_enabled);
    panic("cpu %u spun %u times on lock '%s', held by cpu %u - this is a "
          "deadlock, not contention",
          self, (unsigned)SPIN_LIMIT, lock->name ? lock->name : "(unnamed)",
          lock->holder);
}

void spin_unlock(spinlock_t *lock)
{
    if (!__atomic_load_n(&lock->locked, __ATOMIC_RELAXED))
        panic("release of lock '%s' that is not held",
              lock->name ? lock->name : "(unnamed)");

    u32 self = smp_cpu_index();

    if (__atomic_load_n(&lock->holder, __ATOMIC_RELAXED) != self)
        panic("cpu %u released lock '%s', which cpu %u holds", self,
              lock->name ? lock->name : "(unnamed)", lock->holder);

    bool was_enabled = lock->saved_if;

    lock->saved_if = false;
    level_leave(lock, self);

    /* Before the lock word, not after: once `locked` is clear another
     * processor may win the exchange, and a stale holder written over its
     * fresh one would make this lock look like ours again. */
    __atomic_store_n(&lock->holder, SPIN_NO_HOLDER, __ATOMIC_RELAXED);

    /* The release has to be the last thing. Once the word is clear another
     * processor may enter the critical section, so every write inside it must
     * already be visible - which is what the release ordering guarantees, and
     * is why `saved_if` is read into a local first. */
    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);

    irq_restore(was_enabled);
}
