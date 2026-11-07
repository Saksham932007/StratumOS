/* StratumOS - the filesystem subsystem's lock.
 *
 * One lock covers the whole stack: the descriptor tables, the VFS open-file
 * table, FAT16 and its sector cache, the block layer and the ATA driver.
 * They are not separable in any useful way - a read through a descriptor
 * walks all five - and a lock per layer would buy nothing but four more
 * orderings to get wrong.
 *
 * It sits at LOCK_LEVEL_FS, above the scheduler's and below the heap's, which
 * is the order the stack already uses: opening a file allocates, allocating
 * grows the heap, and none of the layers below call back into the
 * filesystem. See kernel/spinlock.h and docs/SMP.md.
 *
 * The boundary is where the lock is taken. Everything inside the subsystem -
 * fatfs calling fat16, fat16 calling the block layer - runs with it already
 * held and must not take it again; where such a function is also called from
 * outside, it exists twice, as a public name that takes the lock and a
 * `_locked` name that does not.
 */
#ifndef STRATUM_FS_FSLOCK_H
#define STRATUM_FS_FSLOCK_H

#include <kernel/types.h>

#ifdef STRATUM_FUZZING
/* The fuzz targets link one file of the filesystem against a host shim, with
 * no scheduler and one thread, so the lock has nothing to exclude and
 * kernel/fs/fslock.c is not in their source list. */
static inline void fs_lock(void)
{
}
static inline void fs_unlock(void)
{
}
static inline bool fs_lock_held(void)
{
    return true;
}
static inline bool fs_sleep_begin(void)
{
    return false;
}
static inline void fs_sleep_end(bool released)
{
    (void)released;
}
#else
void fs_lock(void);
void fs_unlock(void);

/* True if this processor holds the filesystem lock. For assertions, and for
 * the sleep window below. */
bool fs_lock_held(void);

/* Release the lock around something that blocks, and take it again after.
 *
 * A blocking console read yields the processor from inside vfs_read(), with
 * the lock held. Yielding takes the scheduler's lock, which is *below* this
 * one, so the hierarchy check would catch it - and even if it did not, the
 * filesystem would be unavailable to every other processor for as long as
 * the reader waited for a keystroke, which could be forever.
 *
 * So the one path that blocks drops the lock first. The caller must treat
 * everything it read from shared filesystem state before the window as
 * stale afterwards. fs_sleep_begin() returns whether it actually released -
 * it does not if the caller did not hold the lock - and that value is what
 * fs_sleep_end() takes back.
 */
bool fs_sleep_begin(void);
void fs_sleep_end(bool released);
#endif /* STRATUM_FUZZING */

#endif /* STRATUM_FS_FSLOCK_H */
