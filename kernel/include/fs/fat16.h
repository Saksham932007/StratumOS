/* StratumOS - a read-only FAT16 driver.
 *
 * FAT16 rather than FAT12 or FAT32, and read-only rather than read-write.
 * Both choices are about where the interesting work is.
 *
 * FAT12's entries are twelve bits, so every other one straddles a byte
 * boundary - a fiddly decoder that teaches nothing the 16-bit version does
 * not. FAT32 moves the root directory into the data area, adds an FSInfo
 * sector and reserves the top four bits of every entry; more bookkeeping,
 * same ideas. FAT16 is the smallest version that has all of them: a boot
 * sector describing the geometry, an allocation table that is a linked list
 * by array index, a fixed root directory, and 8.3 names.
 *
 * Read-only because writing is a different problem. A correct write has to
 * allocate from the FAT, update both copies of it, extend a directory entry's
 * size and cluster chain, and survive being interrupted between any two of
 * those - which is a journalling discussion, not a filesystem-format one.
 * Reading is what `exec` needs.
 *
 * Long filenames are not supported. They are a chain of pseudo-entries with
 * a checksum over the short name, tagged with an attribute byte that older
 * systems skip; a well-defined feature, and not one needed to find /bin/init.
 */
#ifndef _FS_FAT16_H
#define _FS_FAT16_H

#include <kernel/types.h>

#include <fs/blockdev.h>

#define FAT_NAME_MAX       13 /* "FILENAME.EXT" plus a terminator */
#define FAT_PATH_MAX       128

/* Directory entry attributes. */
#define FAT_ATTR_READ_ONLY 0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
/* A long-filename fragment sets all four of the first bits, which is how a
 * driver that does not understand them knows to skip them. */
#define FAT_ATTR_LFN       0x0F

struct fat_dirent {
    char name[FAT_NAME_MAX]; /* decoded from 8.3, dot included */
    u8 attr;
    u32 size; /* zero for a directory           */
    u16 first_cluster;
    bool is_dir;
};

struct fat_info {
    bool mounted;
    char label[12];
    u16 bytes_per_sector;
    u8 sectors_per_cluster;
    u16 reserved_sectors;
    u8 num_fats;
    u16 root_entries;
    u16 sectors_per_fat;
    u32 total_sectors;
    u32 fat_start;  /* all four in sectors from the start of   */
    u32 root_start; /* the partition                           */
    u32 data_start;
    u32 cluster_count;
    u32 cluster_bytes;
    /* Counters, so the shell and the tests can see the work done. */
    u32 reads; /* sector reads issued through the cache   */
    u32 cache_hits;
    u32 opens;
    u32 lookups;
};

/* Mount the filesystem in `dev`. One filesystem at a time; mounting a second
 * replaces the first. Returns false, with a reason logged, if the boot sector
 * does not describe a FAT16 filesystem this driver can read. */
bool fat16_mount(const struct blockdev *dev);
bool fat16_mounted(void);
const struct fat_info *fat16_get_info(void);
const char *fat16_device_name(void);

/* Resolve an absolute path. Returns false if any component is missing, or if
 * a component that has to be a directory is not one. */
bool fat16_stat(const char *path, struct fat_dirent *out);

/* Read up to `len` bytes from `offset` within a file. Returns the number of
 * bytes read, or -1 on error. Short reads happen only at end of file. */
i32 fat16_read(const struct fat_dirent *file, u32 offset, void *buf, u32 len);

/* Read a whole file into freshly kmalloc'd memory. The caller owns it and
 * must kfree it. `size_out` receives the length. Bounded by `max`, so a
 * caller cannot be made to allocate a gigabyte by a crafted directory
 * entry. */
void *fat16_read_whole(const char *path, u32 max, u32 *size_out);

/* Walk a directory. `index` counts only the entries this driver reports -
 * skipping deleted entries, long-filename fragments and the volume label -
 * so it is stable for a given filesystem but is not an on-disk offset. */
bool fat16_readdir(const char *path, u32 index, struct fat_dirent *out);

#endif /* _FS_FAT16_H */
