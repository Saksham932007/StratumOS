/* StratumOS - block devices and MBR partitions.
 *
 * One thin layer between the ATA driver and the filesystem, for one reason:
 * a filesystem lives in a *partition*, and a partition is an offset. Without
 * this, every read in the FAT driver would have to remember to add the
 * partition's starting LBA, and forgetting once gives you a filesystem that
 * reads the wrong sectors and blames its own metadata.
 *
 * It is also where a bounds check belongs. A block device knows its own
 * length, so a filesystem that follows a corrupt pointer is stopped here with
 * a message naming the request, rather than at the drive, which can only say
 * "sector not found".
 */
#ifndef _FS_BLOCKDEV_H
#define _FS_BLOCKDEV_H

#include <kernel/types.h>

#define BLOCK_SIZE         512
#define BLOCKDEV_MAX       8
#define BLOCKDEV_NAME_MAX  12

/* MBR partition types this kernel recognises as something to look inside. */
#define MBR_TYPE_FAT16     0x06
#define MBR_TYPE_FAT16_LBA 0x0E
#define MBR_TYPE_FAT12     0x01
#define MBR_TYPE_FAT16_SML 0x04

struct blockdev {
    bool present;
    char name[BLOCKDEV_NAME_MAX]; /* "hd0", "hd0p1"                    */
    u32 drive;                    /* which ATA drive                   */
    u64 first_lba;                /* where this device starts on it    */
    u64 sectors;                  /* how long it is                    */
    u8 partition_type;            /* 0 for a whole disk                */
};

/* Probe every ATA drive for an MBR and register a block device for the disk
 * itself plus one per partition it describes. */
void blockdev_init(void);

u32 blockdev_count(void);
const struct blockdev *blockdev_get(u32 index);
const struct blockdev *blockdev_find(const char *name);

/* Read `count` blocks from `offset` *within this device*. Bounds-checked
 * against the device's own length, so a filesystem cannot read past its
 * partition into whatever is next on the disk. */
bool blockdev_read(const struct blockdev *dev, u64 offset, u32 count,
                   void *buf);

/* The same read, for callers already inside the filesystem subsystem and
 * holding its lock - FAT16's sector cache, which reaches the disk on behalf
 * of a vfs_read() that took the lock at the boundary. See
 * kernel/include/fs/fslock.h. */
bool blockdev_read_locked(const struct blockdev *dev, u64 offset, u32 count,
                          void *buf);

/* Write `count` blocks at `offset` within this device, bounds-checked the
 * same way - which matters more on this side, because a write that escapes a
 * partition destroys what is next on the disk rather than merely reading it.
 * Durable when it returns: ata_write() ends with a cache flush. */
bool blockdev_write(const struct blockdev *dev, u64 offset, u32 count,
                    const void *buf);

/* Commit the underlying drive's write cache. For a caller that has just
 * finished a multi-step update and wants one flush at the end of it. */
bool blockdev_flush(const struct blockdev *dev);

/* The first partition holding a filesystem this kernel can mount, or NULL. */
const struct blockdev *blockdev_first_fs(void);

#endif /* _FS_BLOCKDEV_H */
