/* StratumOS - a read-only FAT16 driver.
 *
 * See fs/fat16.h for why FAT16, and why read-only.
 *
 * Shape of the thing: the boot sector (the BPB) gives the geometry, from
 * which four sector offsets follow - FAT, root directory, data - and
 * everything else is arithmetic on those. A file is a starting cluster plus a
 * length; the FAT turns the starting cluster into a chain by holding, at
 * index N, the number of the cluster that follows N.
 *
 * Every number that comes off the disk is checked before it is used as an
 * index or a length. That is not defensive style for its own sake: a FAT
 * image is user-supplied data, the cluster chain is a linked list whose
 * pointers live in that data, and a chain that loops back on itself is the
 * easiest way to hang a driver that trusts it.
 */
#define LOG_TAG "fat"

#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/string.h>

#include <mm/heap.h>

#include <fs/blockdev.h>
#include <fs/fat16.h>

/* Cluster numbers with a meaning rather than a location. */
#define FAT_FREE          0x0000
#define FAT_RESERVED      0x0001
#define FAT_BAD           0xFFF7
#define FAT_EOC_FIRST     0xFFF8 /* 0xFFF8-0xFFFF all mean end of chain */

#define DIRENT_SIZE       32
#define DIRENT_FREE       0x00 /* this entry and every one after is free */
#define DIRENT_DELETED    0xE5

/* How many links a chain may have before the driver calls it a loop. A
 * filesystem cannot legitimately have a chain longer than its cluster count,
 * so exceeding that is proof of corruption rather than a guess. */
#define CHAIN_LIMIT(info) ((info)->cluster_count + 2)

struct fat_bpb {
    u8 jump[3];
    char oem[8];
    u16 bytes_per_sector;
    u8 sectors_per_cluster;
    u16 reserved_sectors;
    u8 num_fats;
    u16 root_entries;
    u16 total_sectors_16;
    u8 media;
    u16 sectors_per_fat;
    u16 sectors_per_track;
    u16 heads;
    u32 hidden_sectors;
    u32 total_sectors_32;
    /* The extended BIOS parameter block, FAT12/16 flavour. */
    u8 drive_number;
    u8 reserved;
    u8 extended_signature;
    u32 volume_id;
    char label[11];
    char fs_type[8];
} PACKED;

_Static_assert(sizeof(struct fat_bpb) == 62, "the FAT16 BPB is 62 bytes");

struct fat_raw_dirent {
    u8 name[11];
    u8 attr;
    u8 nt_reserved;
    u8 create_tenths;
    u16 create_time;
    u16 create_date;
    u16 access_date;
    u16 cluster_high; /* FAT32 only; must be zero here */
    u16 write_time;
    u16 write_date;
    u16 cluster_low;
    u32 size;
} PACKED;

_Static_assert(sizeof(struct fat_raw_dirent) == DIRENT_SIZE,
               "a FAT directory entry is 32 bytes");

static struct fat_info info;
static const struct blockdev *device;
static char device_name[BLOCKDEV_NAME_MAX];

/* A two-sector cache: one slot for the allocation table, one for everything
 * else.
 *
 * The need for a cache at all comes from the access pattern. Walking a
 * directory reads 16 entries out of every sector; following a cluster chain
 * reads one 16-bit FAT entry out of a sector that holds 256 of them. Without
 * a cache, resolving /bin/INIT issues a disk read per directory entry
 * examined and per cluster followed.
 *
 * The *split* comes from a measurement. A single slot thrashed: reading a
 * file alternates between FAT sectors (to follow the chain) and data sectors
 * (to copy bytes out), so each one evicted the other and the hit rate
 * collapsed to nearly nothing. Reading a file in 64-byte chunks should touch
 * each 512-byte sector eight times and miss once; with one slot it missed
 * every time.
 *
 * Two slots, divided by *kind* rather than by recency, because the two
 * streams are independent and both are sequential. An LRU pair would work
 * too and would need a replacement policy; splitting by purpose needs none,
 * and makes it impossible for one stream to starve the other.
 *
 * A third slot would buy nothing. There is no third stream: a directory
 * entry and a file's contents are both "data", and only one is being read at
 * a time.
 */
enum cache_slot {
    CACHE_FAT = 0,  /* sectors of the allocation table */
    CACHE_DATA = 1, /* directory entries and file contents */
    CACHE_SLOTS = 2,
};

static u8 cache[CACHE_SLOTS][BLOCK_SIZE];
static u64 cache_lba[CACHE_SLOTS];
static bool cache_valid[CACHE_SLOTS];

static void cache_flush(void)
{
    for (u32 i = 0; i < CACHE_SLOTS; i++)
        cache_valid[i] = false;
}

static bool read_sector(enum cache_slot slot, u64 lba, const u8 **out)
{
    if (cache_valid[slot] && cache_lba[slot] == lba) {
        info.cache_hits++;
        *out = cache[slot];
        return true;
    }

    if (!blockdev_read(device, lba, 1, cache[slot])) {
        cache_valid[slot] = false;
        return false;
    }

    cache_lba[slot] = lba;
    cache_valid[slot] = true;
    info.reads++;
    *out = cache[slot];
    return true;
}

bool fat16_mounted(void)
{
    return info.mounted;
}

const struct fat_info *fat16_get_info(void)
{
    return &info;
}

const char *fat16_device_name(void)
{
    return info.mounted ? device_name : "(none)";
}

/* ---- mounting ---------------------------------------------------------- */

bool fat16_mount(const struct blockdev *dev)
{
    if (!dev) {
        pr_err("mount: no device");
        return false;
    }

    memset(&info, 0, sizeof(info));
    device = dev;
    cache_flush();
    strlcpy(device_name, dev->name, sizeof(device_name));

    u8 sector[BLOCK_SIZE];

    if (!blockdev_read(dev, 0, 1, sector)) {
        pr_err("mount %s: cannot read the boot sector", dev->name);
        return false;
    }

    struct fat_bpb bpb;

    memcpy(&bpb, sector, sizeof(bpb));

    /* Validate before computing anything from these numbers. Every one of
     * them is a divisor or a multiplier below, so a zero is a crash and a
     * large value is an out-of-range read. */
    if (bpb.bytes_per_sector != BLOCK_SIZE) {
        pr_err("mount %s: %u-byte sectors; this driver assumes %u", dev->name,
               bpb.bytes_per_sector, (unsigned)BLOCK_SIZE);
        return false;
    }

    if (bpb.sectors_per_cluster == 0 ||
        (bpb.sectors_per_cluster & (bpb.sectors_per_cluster - 1)) != 0 ||
        bpb.sectors_per_cluster > 128) {
        pr_err("mount %s: %u sectors per cluster is not a power of two in "
               "1-128",
               dev->name, bpb.sectors_per_cluster);
        return false;
    }

    if (bpb.reserved_sectors == 0) {
        pr_err("mount %s: zero reserved sectors; the boot sector itself is "
               "one",
               dev->name);
        return false;
    }

    if (bpb.num_fats == 0 || bpb.num_fats > 2) {
        pr_err("mount %s: %u file allocation tables", dev->name, bpb.num_fats);
        return false;
    }

    if (bpb.root_entries == 0) {
        /* A zero here is FAT32's marker: it has no fixed root directory. */
        pr_err("mount %s: zero root entries - this looks like FAT32, which "
               "this driver does not read",
               dev->name);
        return false;
    }

    if (bpb.sectors_per_fat == 0) {
        pr_err("mount %s: zero sectors per FAT (FAT32 again?)", dev->name);
        return false;
    }

    u32 total =
        bpb.total_sectors_16 ? bpb.total_sectors_16 : bpb.total_sectors_32;

    if (total == 0 || total > dev->sectors) {
        pr_err("mount %s: the BPB claims %u sectors; the device has %llu",
               dev->name, total, dev->sectors);
        return false;
    }

    info.bytes_per_sector = bpb.bytes_per_sector;
    info.sectors_per_cluster = bpb.sectors_per_cluster;
    info.reserved_sectors = bpb.reserved_sectors;
    info.num_fats = bpb.num_fats;
    info.root_entries = bpb.root_entries;
    info.sectors_per_fat = bpb.sectors_per_fat;
    info.total_sectors = total;

    info.fat_start = bpb.reserved_sectors;
    info.root_start = info.fat_start + (u32)bpb.num_fats * bpb.sectors_per_fat;

    u32 root_sectors =
        ((u32)bpb.root_entries * DIRENT_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE;

    info.data_start = info.root_start + root_sectors;

    if (info.data_start >= total) {
        pr_err("mount %s: the metadata (%u sectors) does not fit in %u",
               dev->name, info.data_start, total);
        return false;
    }

    info.cluster_count = (total - info.data_start) / bpb.sectors_per_cluster;
    info.cluster_bytes = (u32)bpb.sectors_per_cluster * BLOCK_SIZE;

    /* The cluster count is what decides FAT12 from FAT16 from FAT32 - not
     * the fs_type string, which is a comment and is routinely wrong. Below
     * 4085 clusters the entries are 12 bits wide and this driver would read
     * every one of them incorrectly, so refuse rather than misread. */
    if (info.cluster_count < 4085) {
        pr_err("mount %s: %u clusters means FAT12 (12-bit entries), which "
               "this driver does not decode",
               dev->name, info.cluster_count);
        return false;
    }

    if (info.cluster_count > 65524) {
        pr_err("mount %s: %u clusters is beyond FAT16", dev->name,
               info.cluster_count);
        return false;
    }

    /* The FAT has to be big enough to hold an entry per cluster. A table
     * shorter than its own cluster count means following a chain would read
     * off the end of it. */
    u32 fat_bytes_needed = (info.cluster_count + 2) * 2;

    if ((u32)bpb.sectors_per_fat * BLOCK_SIZE < fat_bytes_needed) {
        pr_err("mount %s: the FAT is %u bytes but %u clusters need %u",
               dev->name, (u32)bpb.sectors_per_fat * BLOCK_SIZE,
               info.cluster_count, fat_bytes_needed);
        return false;
    }

    memcpy(info.label, bpb.label, 11);
    info.label[11] = '\0';
    for (int i = 10; i >= 0 && info.label[i] == ' '; i--)
        info.label[i] = '\0';

    info.mounted = true;

    pr_info("mounted %s: FAT16 \"%s\", %u KiB, %u clusters of %u KiB",
            dev->name, info.label, total / 2, info.cluster_count,
            info.cluster_bytes / KIB);
    pr_debug("%s: FAT at +%u (%u x %u sectors), root at +%u (%u entries), "
             "data at +%u",
             dev->name, info.fat_start, info.num_fats, info.sectors_per_fat,
             info.root_start, info.root_entries, info.data_start);

    return true;
}

/* ---- the allocation table --------------------------------------------- */

/* The next cluster in a chain, or 0 on any error. 0 is safe to use as the
 * error value because cluster 0 is reserved and can never appear in a
 * chain. */
static u16 fat_next(u16 cluster)
{
    if (cluster < 2 || cluster >= info.cluster_count + 2) {
        pr_err("cluster %u is outside the filesystem's 2-%u", cluster,
               info.cluster_count + 1);
        return 0;
    }

    u32 byte = (u32)cluster * 2;
    u64 lba = info.fat_start + byte / BLOCK_SIZE;
    const u8 *sector;

    if (!read_sector(CACHE_FAT, lba, &sector))
        return 0;

    u16 next;

    memcpy(&next, &sector[byte % BLOCK_SIZE], sizeof(next));

    return next;
}

static bool is_end_of_chain(u16 entry)
{
    return entry >= FAT_EOC_FIRST;
}

static u64 cluster_lba(u16 cluster)
{
    return info.data_start + (u64)(cluster - 2) * info.sectors_per_cluster;
}

/* ---- names ------------------------------------------------------------- */

/* Decode an on-disk 8.3 name into "NAME.EXT".
 *
 * The on-disk form is eleven bytes with no dot: eight of stem and three of
 * extension, each space-padded. The dot is punctuation that exists only in
 * the presentation, which is why "README.TXT" and "README  TXT" are the same
 * name and why comparing the undecoded bytes is the wrong way to look a file
 * up. */
static void decode_name(const u8 *raw, char *out)
{
    u32 n = 0;

    for (u32 i = 0; i < 8 && raw[i] != ' '; i++)
        out[n++] = (char)raw[i];

    if (raw[8] != ' ') {
        out[n++] = '.';
        for (u32 i = 8; i < 11 && raw[i] != ' '; i++)
            out[n++] = (char)raw[i];
    }

    out[n] = '\0';
}

static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* Case-insensitive, because FAT is. `/bin/init` has to find `INIT`. */
static bool name_matches(const char *a, const char *b)
{
    while (*a && *b) {
        if (upper(*a) != upper(*b))
            return false;
        a++;
        b++;
    }
    return *a == *b;
}

/* ---- directories ------------------------------------------------------ */

/* Everything a directory walk needs to know, whether it is the fixed root or
 * a cluster chain in the data area. Having one shape for both is what lets
 * one loop handle them. */
struct dir_cursor {
    bool is_root;
    u16 cluster; /* for a non-root directory         */
    u32 sector;  /* within the root, or the cluster  */
    u32 entry;   /* within the sector                */
    u32 links;   /* chain links followed, for the loop check */
};

static void cursor_init(struct dir_cursor *c, u16 first_cluster)
{
    c->is_root = (first_cluster == 0);
    c->cluster = first_cluster;
    c->sector = 0;
    c->entry = 0;
    c->links = 0;
}

/* Fetch the next raw entry, advancing the cursor.
 *
 * Returns false at the end of the directory or on any error - the caller
 * cannot tell them apart, and does not need to: both mean "stop". `done` is
 * set when the end was reached cleanly, so a caller that cares can
 * distinguish a missing file from a broken filesystem.
 */
static bool cursor_next(struct dir_cursor *c, struct fat_raw_dirent *out,
                        bool *done)
{
    *done = false;

    for (;;) {
        u64 lba;
        u32 sectors_here;

        if (c->is_root) {
            sectors_here =
                ((u32)info.root_entries * DIRENT_SIZE + BLOCK_SIZE - 1) /
                BLOCK_SIZE;

            if (c->sector >= sectors_here) {
                *done = true;
                return false;
            }

            lba = info.root_start + c->sector;
        } else {
            sectors_here = info.sectors_per_cluster;

            if (c->sector >= sectors_here) {
                u16 next = fat_next(c->cluster);

                if (next == 0)
                    return false;
                if (is_end_of_chain(next)) {
                    *done = true;
                    return false;
                }
                if (++c->links > CHAIN_LIMIT(&info)) {
                    pr_err("directory cluster chain loops");
                    return false;
                }

                c->cluster = next;
                c->sector = 0;
                continue;
            }

            lba = cluster_lba(c->cluster) + c->sector;
        }

        const u8 *sector;

        if (!read_sector(CACHE_DATA, lba, &sector))
            return false;

        u32 per_sector = BLOCK_SIZE / DIRENT_SIZE;

        if (c->entry >= per_sector) {
            c->entry = 0;
            c->sector++;
            continue;
        }

        memcpy(out, &sector[c->entry * DIRENT_SIZE], DIRENT_SIZE);
        c->entry++;

        /* A zero first byte means this entry has never been used, and
         * neither has any entry after it. That is the end of the
         * directory. */
        if (out->name[0] == DIRENT_FREE) {
            *done = true;
            return false;
        }

        if (out->name[0] == DIRENT_DELETED)
            continue;

        /* Long-filename fragments and the volume label are not files. */
        if ((out->attr & FAT_ATTR_LFN) == FAT_ATTR_LFN)
            continue;
        if (out->attr & FAT_ATTR_VOLUME_ID)
            continue;

        return true;
    }
}

static void fill_dirent(const struct fat_raw_dirent *raw,
                        struct fat_dirent *out)
{
    decode_name(raw->name, out->name);
    out->attr = raw->attr;
    out->is_dir = (raw->attr & FAT_ATTR_DIRECTORY) != 0;
    out->size = out->is_dir ? 0 : raw->size;
    out->first_cluster = raw->cluster_low;
}

/* Find one component inside the directory starting at `dir_cluster`
 * (0 meaning the root). */
static bool lookup_in(u16 dir_cluster, const char *name, struct fat_dirent *out)
{
    struct dir_cursor c;
    struct fat_raw_dirent raw;
    bool done;

    cursor_init(&c, dir_cluster);
    info.lookups++;

    while (cursor_next(&c, &raw, &done)) {
        char decoded[FAT_NAME_MAX];

        decode_name(raw.name, decoded);

        if (name_matches(decoded, name)) {
            fill_dirent(&raw, out);
            return true;
        }
    }

    return false;
}

/* ---- paths ------------------------------------------------------------- */

/* Resolve an absolute path to its directory entry.
 *
 * The root is a special case throughout FAT16 - it is at a fixed sector and
 * has cluster number 0 rather than a real one - so it gets a synthetic entry
 * here, and everything downstream can treat it like any other directory.
 */
bool fat16_stat(const char *path, struct fat_dirent *out)
{
    if (!info.mounted || !path || !out)
        return false;

    if (path[0] != '/') {
        pr_warn("'%s' is not an absolute path; there is no working "
                "directory",
                path);
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->is_dir = true;
    out->attr = FAT_ATTR_DIRECTORY;
    out->first_cluster = 0;
    strlcpy(out->name, "/", sizeof(out->name));

    const char *p = path;

    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;

        /* One component, bounded by the 8.3 name length. A longer one cannot
         * match anything on the disk, so refusing it here is both correct and
         * the only way to keep the buffer honest. */
        char component[FAT_NAME_MAX];
        u32 n = 0;

        while (*p && *p != '/') {
            if (n >= sizeof(component) - 1) {
                pr_warn("'%s' has a component longer than %u characters", path,
                        (unsigned)(sizeof(component) - 1));
                return false;
            }
            component[n++] = *p++;
        }
        component[n] = '\0';

        if (!out->is_dir) {
            pr_debug("'%s': '%s' is a file, not a directory", path, out->name);
            return false;
        }

        if (!lookup_in(out->first_cluster, component, out))
            return false;
    }

    return true;
}

/* ---- reading ----------------------------------------------------------- */

i32 fat16_read(const struct fat_dirent *file, u32 offset, void *buf, u32 len)
{
    if (!info.mounted || !file || !buf)
        return -1;

    if (file->is_dir) {
        pr_debug("read from '%s', which is a directory", file->name);
        return -1;
    }

    if (offset >= file->size)
        return 0;

    if (len > file->size - offset)
        len = file->size - offset;

    if (len == 0)
        return 0;

    /* Walk the chain to the cluster holding `offset`. O(offset) in cluster
     * hops, which is what a FAT costs: there is no index, the chain *is* the
     * index. Sequential reading is therefore cheap and seeking is not, and
     * the single-sector cache makes the walk one read per FAT sector rather
     * than one per hop. */
    u16 cluster = file->first_cluster;
    u32 skip = offset / info.cluster_bytes;
    u32 links = 0;

    while (skip-- > 0) {
        u16 next = fat_next(cluster);

        if (next == 0 || is_end_of_chain(next)) {
            pr_err("'%s': the chain ends before offset %u", file->name, offset);
            return -1;
        }
        if (++links > CHAIN_LIMIT(&info)) {
            pr_err("'%s': the cluster chain loops", file->name);
            return -1;
        }
        cluster = next;
    }

    u8 *out = buf;
    u32 done = 0;
    u32 within = offset % info.cluster_bytes;

    while (done < len) {
        if (cluster < 2 || cluster >= info.cluster_count + 2) {
            pr_err("'%s': cluster %u is outside the filesystem", file->name,
                   cluster);
            return -1;
        }

        u32 sector_in_cluster = within / BLOCK_SIZE;
        u32 within_sector = within % BLOCK_SIZE;
        u32 chunk = BLOCK_SIZE - within_sector;

        if (chunk > len - done)
            chunk = len - done;

        const u8 *sector;

        if (!read_sector(CACHE_DATA, cluster_lba(cluster) + sector_in_cluster,
                         &sector))
            return -1;

        memcpy(out + done, sector + within_sector, chunk);
        done += chunk;
        within += chunk;

        if (within >= info.cluster_bytes && done < len) {
            u16 next = fat_next(cluster);

            if (next == 0 || is_end_of_chain(next)) {
                /* The directory entry said the file was longer than its chain
                 * actually is. Report what was read rather than inventing
                 * zeroes: a short read is a fact, and padding would hide a
                 * corrupt filesystem. */
                pr_warn("'%s': chain ended %u bytes short of its stated size",
                        file->name, len - done);
                return (i32)done;
            }
            if (++links > CHAIN_LIMIT(&info)) {
                pr_err("'%s': the cluster chain loops", file->name);
                return -1;
            }

            cluster = next;
            within = 0;
        }
    }

    return (i32)done;
}

void *fat16_read_whole(const char *path, u32 max, u32 *size_out)
{
    struct fat_dirent entry;

    if (size_out)
        *size_out = 0;

    if (!fat16_stat(path, &entry))
        return NULL;

    if (entry.is_dir) {
        pr_debug("'%s' is a directory", path);
        return NULL;
    }

    if (entry.size == 0) {
        pr_debug("'%s' is empty", path);
        return NULL;
    }

    /* The size came off the disk, so it is bounded before it becomes an
     * allocation. Without this, a directory entry claiming 4 GiB is a
     * kmalloc of 4 GiB. */
    if (entry.size > max) {
        pr_warn("'%s' is %u bytes; the caller allowed %u", path, entry.size,
                max);
        return NULL;
    }

    void *buf = kmalloc(entry.size);

    if (!buf) {
        pr_err("no memory for '%s' (%u bytes)", path, entry.size);
        return NULL;
    }

    i32 got = fat16_read(&entry, 0, buf, entry.size);

    if (got < 0 || (u32)got != entry.size) {
        pr_err("'%s': read %d of %u bytes", path, got, entry.size);
        kfree(buf);
        return NULL;
    }

    info.opens++;

    if (size_out)
        *size_out = entry.size;

    return buf;
}

bool fat16_readdir(const char *path, u32 index, struct fat_dirent *out)
{
    struct fat_dirent dir;

    if (!info.mounted || !out)
        return false;

    if (!fat16_stat(path, &dir))
        return false;

    if (!dir.is_dir) {
        pr_debug("'%s' is not a directory", path);
        return false;
    }

    struct dir_cursor c;
    struct fat_raw_dirent raw;
    bool done;
    u32 n = 0;

    cursor_init(&c, dir.first_cluster);

    while (cursor_next(&c, &raw, &done)) {
        if (n++ != index)
            continue;

        fill_dirent(&raw, out);
        return true;
    }

    return false;
}
