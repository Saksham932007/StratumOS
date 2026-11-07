/* StratumOS - the filesystem subsystem's lock.
 *
 * See kernel/include/fs/fslock.h for what it covers and why it is one lock
 * rather than five.
 */
#include <kernel/spinlock.h>

#include <fs/fslock.h>

static spinlock_t lock = SPINLOCK_INIT_AT("fs", LOCK_LEVEL_FS);

void fs_lock(void)
{
    spin_lock(&lock);
}

void fs_unlock(void)
{
    spin_unlock(&lock);
}

bool fs_lock_held(void)
{
    return spin_held_by_self(&lock);
}

bool fs_sleep_begin(void)
{
    if (!spin_held_by_self(&lock))
        return false;

    spin_unlock(&lock);
    return true;
}

void fs_sleep_end(bool released)
{
    if (released)
        spin_lock(&lock);
}
