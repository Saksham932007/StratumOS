/* StratumOS - interrupt-safe critical sections.
 *
 * On a uniprocessor kernel the only concurrent execution context is an
 * interrupt handler, so mutual exclusion means masking interrupts - not
 * spinning on a word that nothing else can be holding.
 *
 * This is named spin_lock because that is what it will become, and the
 * `locked` counter is maintained and checked so that a recursive acquisition
 * is caught today rather than discovered the day real SMP locking is added.
 */
#include <kernel/panic.h>
#include <kernel/spinlock.h>

void spin_lock(spinlock_t *lock)
{
    bool was_enabled = irq_save();

    if (lock->locked) {
        /* Re-entering a lock we already hold would deadlock a real spinlock.
         * Fail now, where the stack trace points at the culprit. */
        irq_restore(was_enabled);
        panic("recursive acquisition of lock '%s'",
              lock->name ? lock->name : "(unnamed)");
    }

    lock->locked = 1;
    lock->saved_if = was_enabled;
}

void spin_unlock(spinlock_t *lock)
{
    bool was_enabled = lock->saved_if;

    if (!lock->locked)
        panic("release of lock '%s' that is not held",
              lock->name ? lock->name : "(unnamed)");

    lock->locked = 0;
    lock->saved_if = false;
    irq_restore(was_enabled);
}
