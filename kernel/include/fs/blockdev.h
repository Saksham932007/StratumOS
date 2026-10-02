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

/* The first partition holding a filesystem this kernel can mount, or NULL. */
const struct blockdev *blockdev_first_fs(void);

#endif /* _FS_BLOCKDEV_H */
