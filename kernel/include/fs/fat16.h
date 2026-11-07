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
    /* Where this entry physically lives, so that a write can update the size
     * and first cluster in place rather than looking the path up again.
     *
     * This is the one piece of state that makes an open file on a writable
     * FAT different from one on a read-only FAT. Reading needs the first
     * cluster and the size; writing has to *change* them, and changing them
     * means knowing which 32 bytes of which sector to rewrite. Carrying the
     * location is what makes an open descriptor independent of the path it
     * was opened by - which is also why a file renamed under an open
     * descriptor keeps working, as it does on any Unix.
     *
     * Zero in `ent_lba` means "synthesised, not on disk", which is true of
     * exactly one entry: the root directory, which FAT16 does not give a
     * directory entry of its own. */
    u64 ent_lba;
    u32 ent_index; /* which 32-byte slot within that sector */
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

/* ---- writing ------------------------------------------------------------
 *
 * WHAT MAKES THIS HARDER THAN READING
 *
 * A read touches one structure. A write touches three, and they have to agree
 * with each other afterwards: the allocation table (twice, because there are
 * two copies of it), the data clusters, and the directory entry that records
 * the size and the first cluster. There is no way to update three structures
 * atomically on a disk that only promises single-sector writes, so the
 * question is not "how do we make this atomic" - it cannot be - but "which
 * order leaves the least damage when the power fails in the middle".
 *
 * THE ORDERING RULES, AND WHY THEY ARE THESE
 *
 * Growing a file: allocate the clusters and write the FAT first, then write
 * the data, then update the directory entry last. A crash before the last
 * step leaks clusters - they are marked in use and no file claims them -
 * which wastes space and is repairable by a scan. The reverse order would
 * publish a size and a cluster chain before the clusters were allocated,
 * so the file would claim blocks the allocator still thinks are free, and
 * the next file created would be handed the same ones. Leaked space is a
 * disk that is smaller than it should be; double-allocated space is two
 * files silently sharing bytes.
 *
 * Deleting a file: clear the directory entry first, then free the chain. A
 * crash in between leaks the chain, again repairable. The reverse order
 * would leave a name pointing at clusters that are already free, so the next
 * allocation would hand them out while a readable file still claimed them.
 *
 * Both rules are the same rule: *the name is published last and withdrawn
 * first*. Anything else trades a space leak for corruption.
 *
 * WHAT IS STILL MISSING, AND IT IS NOT SMALL
 *
 * This is crash-*ordered*, not crash-safe. A power failure in the middle of
 * a multi-cluster write leaves a file whose size is right and whose last
 * cluster holds whatever was there before. Nothing detects the leaked
 * clusters either, because there is no fsck. Both want a journal or a
 * copy-on-write layout, which is a different filesystem rather than a
 * missing function - see docs/STORAGE.md.
 *
 * Every metadata write is write-through and synchronous: the FAT is not
 * buffered, so a file written in 64-byte pieces rewrites its directory entry
 * 64 bytes at a time. That is slow and it is deliberate - a write-back
 * metadata cache is where the ordering rules above stop being enforceable by
 * reading the code.
 */

/* Is the mounted filesystem writable? False when nothing is mounted, and
 * false when the volume was mounted from a device that refused a write. */
bool fat16_writable(void);

/* Write `len` bytes at `offset`, extending the file if needed. Returns the
 * number of bytes written, or -1. `file` is updated in place - its size and
 * first cluster can both change - and the directory entry on disk is updated
 * to match before this returns. */
i32 fat16_write(struct fat_dirent *file, u32 offset, const void *buf, u32 len);

/* Why a create failed.
 *
 * A bool return was not enough. The VFS has to turn a failure into an errno,
 * and "could not create" collapses four situations a caller would respond to
 * differently: a parent that does not exist (ENOENT - create it first), a
 * name 8.3 cannot hold (ENAMETOOLONG - no retry will help), a name already
 * taken (EEXIST), and a full volume or directory (ENOSPC). Mapping all of
 * them to ENOSPC told a caller its disk was full when the real problem was a
 * typo in a directory name. */
enum fat_create_error {
    FAT_CREATE_OK = 0,
    FAT_CREATE_BAD_PATH,
    FAT_CREATE_BAD_NAME, /* not expressible as 8.3        */
    FAT_CREATE_NO_PARENT,
    FAT_CREATE_EXISTS,
    FAT_CREATE_FULL, /* no free cluster, or a full root dir */
    FAT_CREATE_IO,
};

/* Create an empty file, or a directory with its "." and ".." entries. */
enum fat_create_error fat16_create_ex(const char *path, bool directory,
                                      struct fat_dirent *out);

/* The same, for callers that only need to know whether it worked. */
bool fat16_create(const char *path, bool directory, struct fat_dirent *out);

/* Set a file's length, freeing or zero-filling as needed. */
bool fat16_truncate(struct fat_dirent *file, u32 length);

bool fat16_unlink(const char *path);
bool fat16_mkdir(const char *path);
/* Refuses a directory that still has anything in it besides "." and "..". */
bool fat16_rmdir(const char *path);
/* Within one filesystem, and only a rename: the data does not move, because
 * only the name in the directory entry changes. */
bool fat16_rename(const char *from, const char *to);

/* Push the drive's write cache to the medium. Every call above is already
 * synchronous, so this is a belt-and-braces flush for a caller that has
 * finished a batch. */
bool fat16_sync(void);

/* The same four calls, for callers already inside the filesystem subsystem
 * and holding its lock - which in practice means fatfs, the VFS adapter that
 * every path through a descriptor goes down. The public forms above take the
 * lock, so calling one of those from in here would take it twice and panic.
 * See kernel/include/fs/fslock.h. */
bool fat16_stat_locked(const char *path, struct fat_dirent *out);
i32 fat16_read_locked(const struct fat_dirent *file, u32 offset, void *buf,
                      u32 len);
bool fat16_readdir_locked(const char *path, u32 index, struct fat_dirent *out);
bool fat16_sync_locked(void);

/* How many clusters are free, and how many there are. Counted by scanning
 * the allocation table, so it is a real answer rather than a running total
 * that could drift away from the disk. */
bool fat16_space(u32 *free_clusters, u32 *total_clusters);

/* Write-side counters, for `mount` in the shell and for the test suite. */
void fat16_write_stats(u32 *writes, u32 *sectors, u32 *clusters_allocated,
                       u32 *clusters_freed, u32 *dirent_updates);

#endif /* _FS_FAT16_H */
