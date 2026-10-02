/* StratumOS - interrupt-safe locks.
 *
 * The kernel is uniprocessor, so a "lock" only needs to keep an interrupt
 * handler from observing a half-updated structure. That makes it an
 * interrupt-state save/restore rather than a spin on a shared word - which is
 * both correct here and honest about what it does. The spin field is kept and
 * checked so that recursive acquisition is caught now rather than when SMP
 * support eventually makes this a real lock.
 */
#ifndef _KERNEL_SPINLOCK_H
#define _KERNEL_SPINLOCK_H

#include <arch/io.h>

#include <kernel/types.h>

typedef struct {
    volatile u32 locked;
    bool saved_if;
    const char *name;
} spinlock_t;

#define SPINLOCK_INIT(nm) \
    {                     \
        0, false, (nm)    \
    }

void spin_lock(spinlock_t *lock);
void spin_unlock(spinlock_t *lock);

#endif /* _KERNEL_SPINLOCK_H */
