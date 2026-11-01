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

/* A FAT date is packed as year-since-1980 in bits 15-9, month in 8-5 and day
 * in 4-0. Zero is therefore not "no date" but month 0 and day 0, which are
 * both out of range - and an external tool reading the image prints
 * "1980-00-00" for it. Found by listing a StratumOS-written image with
 * mtools, which is the point of checking the artefact with something other
 * than the kernel that produced it.
 *
 * 1980-01-01, the earliest date the format can express, rather than the real
 * time: there is an RTC, but a directory entry written with the current clock
 * would make every build of a test image differ from the last, and the fs
 * suite asserts on specific bytes in specific clusters. A fixed, *valid*
 * timestamp is the honest choice until something needs real ones. */
#define FAT_DATE_EPOCH    ((u16)((0 << 9) | (1 << 5) | 1))

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

/* Read a FAT entry, reporting failure separately from the value.
 *
 * fat_next() folds both into a single 0 return, which is fine for walking a
 * chain - a chain never contains cluster 0, so 0 always means stop - and is
 * wrong for the allocator, where 0 is the value that means "this cluster is
 * free, take it". An unreadable entry would then be read as free and handed
 * out, possibly while a file still owns it. Two bytes of ambiguity, one
 * corrupted filesystem. */
static bool fat_get(u16 cluster, u16 *out)
{
    if (cluster < 2 || cluster >= info.cluster_count + 2)
        return false;

    u32 byte = (u32)cluster * 2;
    u64 lba = info.fat_start + byte / BLOCK_SIZE;
    const u8 *sector;

    if (!read_sector(CACHE_FAT, lba, &sector))
        return false;

    memcpy(out, &sector[byte % BLOCK_SIZE], sizeof(*out));

    return true;
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
    /* Where the entry cursor_next() last returned physically lives. Recorded
     * during the walk because that is the only moment it is known for free -
     * reconstructing it afterwards would mean walking the directory again. */
    u64 last_lba;
    u32 last_index;
};

static void cursor_init(struct dir_cursor *c, u16 first_cluster)
{
    c->is_root = (first_cluster == 0);
    c->cluster = first_cluster;
    c->sector = 0;
    c->entry = 0;
    c->links = 0;
    c->last_lba = 0;
    c->last_index = 0;
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
        c->last_lba = lba;
        c->last_index = c->entry;
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
                        struct fat_dirent *out, u64 lba, u32 index)
{
    decode_name(raw->name, out->name);
    out->attr = raw->attr;
    out->is_dir = (raw->attr & FAT_ATTR_DIRECTORY) != 0;
    out->size = out->is_dir ? 0 : raw->size;
    out->first_cluster = raw->cluster_low;
    out->ent_lba = lba;
    out->ent_index = index;
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
            fill_dirent(&raw, out, c.last_lba, c.last_index);
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

        fill_dirent(&raw, out, c.last_lba, c.last_index);
        return true;
    }

    return false;
}

/* =========================================================================
 * Writing
 *
 * The ordering rules this section enforces, and the reasoning for them, are
 * in fs/fat16.h. The short version, which every function below obeys: the
 * name is published last and withdrawn first.
 * ========================================================================= */

/* Counters, so the shell and the tests can see what writing actually did. */
static u32 stat_writes;
static u32 stat_sectors_written;
static u32 stat_clusters_allocated;
static u32 stat_clusters_freed;
static u32 stat_dirent_updates;

/* Where to start looking for a free cluster. Not a cache of what is free -
 * the FAT on disk is always the truth - just a note of where the last search
 * ended, so that filling a filesystem is not quadratic. A stale hint costs
 * one extra scan and cannot cause a wrong answer, which is why it needs no
 * invalidation. */
static u16 alloc_hint = 2;

bool fat16_writable(void)
{
    return info.mounted && device != NULL;
}

/* Write one sector, and keep the read cache consistent with it.
 *
 * Refreshing rather than invalidating: the data is in hand, so there is no
 * reason to make the next read go to the disk for a sector we just wrote.
 * Both slots are checked because either could be holding this LBA - the FAT
 * slot and the data slot are divided by purpose, not by address, so nothing
 * stops a sector from having been read through the other one. Missing that
 * is how a cache serves pre-write bytes after a write, which is a bug that
 * looks like a disk that ignored you. */
static bool write_sector(u64 lba, const void *buf)
{
    if (!blockdev_write(device, lba, 1, buf))
        return false;

    stat_sectors_written++;

    for (u32 i = 0; i < CACHE_SLOTS; i++) {
        if (cache_valid[i] && cache_lba[i] == lba)
            memcpy(cache[i], buf, BLOCK_SIZE);
    }

    return true;
}

/* Read a sector into a caller's own buffer, bypassing the cache.
 *
 * Needed by every read-modify-write below. Going through read_sector() would
 * hand back a pointer into the cache, and modifying that would change what
 * the cache believes the disk says before the write had happened - so a
 * failed write would leave the cache holding bytes that are nowhere on the
 * disk. A private copy keeps the cache truthful at all times. */
static bool read_sector_raw(u64 lba, void *buf)
{
    if (!blockdev_read(device, lba, 1, buf))
        return false;

    info.reads++;
    return true;
}

/* ---- the allocation table, writing ------------------------------------ */

/* Set one FAT entry, in every copy of the table.
 *
 * Both copies, always. FAT16 keeps two by convention and the second is
 * supposed to be a usable replica; a driver that wrote only the first would
 * produce a filesystem that every other implementation considers damaged,
 * and the damage would be invisible until something consulted FAT 2.
 *
 * Read-modify-write of a whole sector, because a FAT entry is two bytes and
 * a disk writes 512 at a time. */
static bool fat_set(u16 cluster, u16 value)
{
    if (cluster < 2 || cluster >= info.cluster_count + 2) {
        pr_err("refusing to write FAT entry %u, outside 2-%u", cluster,
               info.cluster_count + 1);
        return false;
    }

    u32 byte = (u32)cluster * 2;
    u32 within = byte % BLOCK_SIZE;
    u32 sector_index = byte / BLOCK_SIZE;
    u8 sector[BLOCK_SIZE];

    for (u32 copy = 0; copy < info.num_fats; copy++) {
        u64 lba =
            info.fat_start + (u64)copy * info.sectors_per_fat + sector_index;

        if (!read_sector_raw(lba, sector))
            return false;

        memcpy(&sector[within], &value, sizeof(value));

        if (!write_sector(lba, sector))
            return false;
    }

    return true;
}

/* Find a free cluster and claim it as the end of a chain.
 *
 * Claimed immediately rather than returned as "free": a function that handed
 * back a free cluster number without marking it would be a race with itself
 * the moment two allocations happened without an intervening write, and the
 * symptom would be two files sharing a cluster. Returns 0 when the volume is
 * full. */
static u16 fat_alloc(void)
{
    u32 total = info.cluster_count;

    /* Two passes: from the hint to the end, then from the start to the hint.
     * One pass from cluster 2 every time is correct and quadratic over the
     * life of a filesystem that is being filled. */
    for (u32 attempt = 0; attempt < 2; attempt++) {
        u16 from = attempt == 0 ? alloc_hint : 2;
        u16 to = attempt == 0 ? (u16)(total + 2) : alloc_hint;

        for (u16 c = from; c < to; c++) {
            if (c < 2)
                continue;

            u16 entry;

            /* fat_get() rather than fat_next(), because here 0 is a value and
             * not a verdict - see fat_get(). A cluster whose entry could not
             * be read is skipped rather than claimed. */
            if (!fat_get(c, &entry))
                continue;

            if (entry != FAT_FREE)
                continue;

            if (!fat_set(c, FAT_EOC_FIRST | 0x7))
                return 0;

            alloc_hint = (u16)((u32)c + 1 < total + 2 ? c + 1 : 2);
            stat_clusters_allocated++;

            return c;
        }
    }

    pr_warn("the filesystem is full: no free cluster among %u", total);
    return 0;
}

/* Zero every sector of a cluster.
 *
 * Mandatory for a directory, because a FAT directory ends at the first entry
 * whose name begins with 0x00 - so a new directory cluster full of whatever
 * the last file left there would be read as a directory full of garbage
 * entries. Used for file data too, so that extending a file past its end
 * cannot expose the previous occupant's bytes to the new owner, which is an
 * information leak between processes and not merely untidy. */
static bool cluster_zero(u16 cluster)
{
    u8 zeros[BLOCK_SIZE];
    u64 base = cluster_lba(cluster);

    memset(zeros, 0, sizeof(zeros));

    for (u32 s = 0; s < info.sectors_per_cluster; s++) {
        if (!write_sector(base + s, zeros))
            return false;
    }

    return true;
}

/* Free a whole chain, following it as it goes.
 *
 * Each link is read before it is cleared, because clearing it destroys the
 * only record of where the chain continues. Getting that backwards orphans
 * the remainder of the file - which is worse than not freeing it, since the
 * clusters stay marked in use with nothing pointing at them. */
static bool fat_free_chain(u16 first)
{
    u16 cluster = first;
    u32 links = 0;

    while (cluster >= 2 && !is_end_of_chain(cluster)) {
        u16 next = fat_next(cluster);

        if (!fat_set(cluster, FAT_FREE))
            return false;

        stat_clusters_freed++;

        /* Freeing lowers the watermark, so a subsequent allocation should
         * look here rather than wherever the last one ended. */
        if (cluster < alloc_hint)
            alloc_hint = cluster;

        if (next == 0) {
            /* A broken link. The clusters up to here have been returned,
             * which is the best available outcome; the rest are leaked and
             * stay marked in use. Reported rather than silent, because a
             * chain that ends in a bad entry means the filesystem was already
             * damaged. */
            pr_err("chain from cluster %u ends in an unreadable entry; %u "
                   "cluster(s) leaked",
                   first, 1u);
            return false;
        }

        if (++links > CHAIN_LIMIT(&info)) {
            pr_err("chain from cluster %u loops; stopping", first);
            return false;
        }

        cluster = next;
    }

    return true;
}

/* The `index`-th cluster of a chain, growing the chain if asked.
 *
 * `*first` is updated when a chain that was empty gains its first cluster,
 * which is the one case where growing a file changes its directory entry's
 * cluster field as well as its size. */
static u16 chain_cluster_at(u16 *first, u32 index, bool grow)
{
    if (*first == 0) {
        if (!grow)
            return 0;

        u16 c = fat_alloc();

        if (!c)
            return 0;

        if (!cluster_zero(c)) {
            (void)fat_set(c, FAT_FREE);
            return 0;
        }

        *first = c;
    }

    u16 cluster = *first;

    for (u32 i = 0; i < index; i++) {
        u16 next = fat_next(cluster);

        if (next == 0)
            return 0;

        if (is_end_of_chain(next)) {
            if (!grow)
                return 0;

            u16 fresh = fat_alloc();

            if (!fresh)
                return 0;

            if (!cluster_zero(fresh)) {
                (void)fat_set(fresh, FAT_FREE);
                return 0;
            }

            /* The new cluster is allocated and zeroed before it is linked.
             * Linking first would publish a chain that reaches a cluster
             * still holding its previous contents. */
            if (!fat_set(cluster, fresh)) {
                (void)fat_set(fresh, FAT_FREE);
                return 0;
            }

            next = fresh;
        }

        if (i > CHAIN_LIMIT(&info)) {
            pr_err("cluster chain loops while seeking index %u", index);
            return 0;
        }

        cluster = next;
    }

    return cluster;
}

/* ---- directory entries, writing --------------------------------------- */

/* Encode "NAME.EXT" into the eleven-byte on-disk form.
 *
 * Returns false for anything 8.3 cannot express, and that refusal is the
 * feature: silently truncating "LONGFILENAME.TXT" to "LONGFILE.TXT" would
 * create a file under a name the caller did not ask for, and a second
 * create would then collide with the first for no visible reason.
 *
 * The characters FAT forbids are refused too. Several of them - '/', '\\',
 * ':' - are path punctuation, so accepting one would put a name on the disk
 * that no path could ever name again. */
static bool encode_name(const char *name, u8 out[11])
{
    static const char *forbidden = "\"*+,/:;<=>?[\\]|";
    u32 i = 0;

    memset(out, ' ', 11);

    if (!name || !*name || *name == '.')
        return false;

    /* The stem: up to eight characters, stopping at the dot. */
    for (; name[i] && name[i] != '.'; i++) {
        if (i >= 8)
            return false;
        if ((u8)name[i] < 0x20 || strchr(forbidden, name[i]))
            return false;
        out[i] = (u8)upper(name[i]);
    }

    if (!name[i])
        return true;

    /* The extension: up to three, and exactly one dot is allowed. */
    const char *ext = &name[i + 1];

    for (u32 j = 0; ext[j]; j++) {
        if (j >= 3)
            return false;
        if (ext[j] == '.')
            return false;
        if ((u8)ext[j] < 0x20 || strchr(forbidden, ext[j]))
            return false;
        out[8 + j] = (u8)upper(ext[j]);
    }

    return true;
}

/* Read, modify and write back the 32 bytes of one directory entry. */
static bool dirent_update(u64 lba, u32 index,
                          void (*edit)(struct fat_raw_dirent *, void *),
                          void *ctx)
{
    u8 sector[BLOCK_SIZE];
    struct fat_raw_dirent raw;

    if (lba == 0) {
        /* The root directory has no entry of its own. Nothing that updates a
         * size or a cluster should ever be aimed at it, so this is a bug
         * rather than an I/O error. */
        pr_err("attempt to update the root directory's own entry");
        return false;
    }

    if (index >= BLOCK_SIZE / DIRENT_SIZE)
        return false;

    if (!read_sector_raw(lba, sector))
        return false;

    memcpy(&raw, &sector[index * DIRENT_SIZE], DIRENT_SIZE);
    edit(&raw, ctx);
    memcpy(&sector[index * DIRENT_SIZE], &raw, DIRENT_SIZE);

    stat_dirent_updates++;

    return write_sector(lba, sector);
}

struct size_and_cluster {
    u32 size;
    u16 cluster;
};

static void edit_size_cluster(struct fat_raw_dirent *raw, void *ctx)
{
    const struct size_and_cluster *v = ctx;

    raw->size = v->size;
    raw->cluster_low = v->cluster;
}

static void edit_delete(struct fat_raw_dirent *raw, void *ctx)
{
    UNUSED(ctx);

    /* 0xE5 rather than 0x00. A zero first byte means "this entry and every
     * entry after it has never been used", so writing one would truncate the
     * directory and hide every file that happened to sit after this one. */
    raw->name[0] = DIRENT_DELETED;
}

static void edit_rename(struct fat_raw_dirent *raw, void *ctx)
{
    memcpy(raw->name, ctx, 11);
}

/* Persist a file's current size and first cluster. */
static bool dirent_commit(const struct fat_dirent *f)
{
    struct size_and_cluster v = {f->size, f->first_cluster};

    return dirent_update(f->ent_lba, f->ent_index, edit_size_cluster, &v);
}

/* Find a free slot in a directory, extending it if every slot is taken.
 *
 * A slot is free if it is deleted (0xE5) or never used (0x00). Reusing a
 * deleted slot in preference to extending keeps directories from growing
 * without bound as files come and go.
 */
static bool dirent_find_free(u16 dir_cluster, u64 *lba_out, u32 *index_out)
{
    u32 per_sector = BLOCK_SIZE / DIRENT_SIZE;
    u8 sector[BLOCK_SIZE];

    if (dir_cluster == 0) {
        /* The root is a fixed run of sectors and cannot be extended - its
         * size was fixed when the volume was formatted. A full root
         * directory is therefore ENOSPC even on an otherwise empty disk,
         * which is a real FAT16 limitation and not a shortcut here. */
        u32 sectors = ((u32)info.root_entries * DIRENT_SIZE + BLOCK_SIZE - 1) /
                      BLOCK_SIZE;

        for (u32 s = 0; s < sectors; s++) {
            u64 lba = info.root_start + s;

            if (!read_sector_raw(lba, sector))
                return false;

            for (u32 e = 0; e < per_sector; e++) {
                u8 first = sector[e * DIRENT_SIZE];

                if (first == DIRENT_FREE || first == DIRENT_DELETED) {
                    *lba_out = lba;
                    *index_out = e;
                    return true;
                }
            }
        }

        pr_warn("the root directory is full (%u entries)", info.root_entries);
        return false;
    }

    u16 cluster = dir_cluster;
    u32 links = 0;

    for (;;) {
        for (u32 s = 0; s < info.sectors_per_cluster; s++) {
            u64 lba = cluster_lba(cluster) + s;

            if (!read_sector_raw(lba, sector))
                return false;

            for (u32 e = 0; e < per_sector; e++) {
                u8 first = sector[e * DIRENT_SIZE];

                if (first == DIRENT_FREE || first == DIRENT_DELETED) {
                    *lba_out = lba;
                    *index_out = e;
                    return true;
                }
            }
        }

        u16 next = fat_next(cluster);

        if (next == 0)
            return false;

        if (is_end_of_chain(next)) {
            /* Extend the directory by one zeroed cluster. Zeroed is what
             * makes every entry in it read as "never used". */
            u16 fresh = fat_alloc();

            if (!fresh)
                return false;

            if (!cluster_zero(fresh) || !fat_set(cluster, fresh)) {
                (void)fat_set(fresh, FAT_FREE);
                return false;
            }

            *lba_out = cluster_lba(fresh);
            *index_out = 0;

            return true;
        }

        if (++links > CHAIN_LIMIT(&info)) {
            pr_err("directory chain loops while looking for a free slot");
            return false;
        }

        cluster = next;
    }
}

/* Split "/a/b/c" into the parent path "/a/b" and the final component "c".
 *
 * The leaf buffer the callers pass is deliberately a full path's worth rather
 * than FAT_NAME_MAX. A leaf that does not fit in thirteen bytes is an invalid
 * *name*, and encode_name() is the one place that decides that - if the split
 * rejected it first, the caller would be told its path was malformed when the
 * truth is that its filename is too long for 8.3. Those map to different
 * errnos and suggest different fixes. */
static bool split_path(const char *path, char *parent, size_t parent_len,
                       char *leaf, size_t leaf_len)
{
    const char *slash = strrchr(path, '/');

    if (!slash || path[0] != '/')
        return false;

    size_t plen = (size_t)(slash - path);

    if (plen == 0) {
        strlcpy(parent, "/", parent_len);
    } else {
        if (plen >= parent_len)
            return false;
        memcpy(parent, path, plen);
        parent[plen] = '\0';
    }

    if (strlcpy(leaf, slash + 1, leaf_len) >= leaf_len)
        return false;

    return leaf[0] != '\0';
}

enum fat_create_error fat16_create_ex(const char *path, bool directory,
                                      struct fat_dirent *out)
{
    struct fat_dirent parent_ent;
    struct fat_dirent existing;
    char parent[FAT_PATH_MAX];
    char leaf[FAT_PATH_MAX];
    u8 encoded[11];

    if (!fat16_writable() || !path)
        return FAT_CREATE_IO;

    if (!split_path(path, parent, sizeof(parent), leaf, sizeof(leaf))) {
        pr_warn("'%s' is not a path a file can be created at", path);
        return FAT_CREATE_BAD_PATH;
    }

    if (!encode_name(leaf, encoded)) {
        pr_warn("'%s' is not expressible as an 8.3 name", leaf);
        return FAT_CREATE_BAD_NAME;
    }

    if (!fat16_stat(parent, &parent_ent) || !parent_ent.is_dir) {
        pr_debug("'%s': the parent directory does not exist", path);
        return FAT_CREATE_NO_PARENT;
    }

    /* Refused rather than overwritten. An open-with-create that silently
     * replaced an existing file would make O_EXCL unimplementable and would
     * make a mkdir of an existing directory destroy its contents. */
    if (lookup_in(parent_ent.first_cluster, leaf, &existing)) {
        pr_debug("'%s' already exists", path);
        return FAT_CREATE_EXISTS;
    }

    u16 first_cluster = 0;

    /* A directory must have a cluster from the moment it exists, because its
     * "." and ".." entries live in it. A file starts with none: an empty file
     * occupies no data space at all, which is why first_cluster is 0 and not
     * a cluster full of nothing. */
    if (directory) {
        first_cluster = fat_alloc();

        if (!first_cluster)
            return FAT_CREATE_FULL;

        if (!cluster_zero(first_cluster)) {
            (void)fat_set(first_cluster, FAT_FREE);
            return FAT_CREATE_IO;
        }

        /* "." and "..", written before the directory is published. ".." of a
         * directory in the root is cluster 0, which is how FAT spells "the
         * root" - not a null pointer. */
        u8 sector[BLOCK_SIZE];
        struct fat_raw_dirent dot;
        struct fat_raw_dirent dotdot;

        memset(sector, 0, sizeof(sector));
        memset(&dot, 0, sizeof(dot));
        memset(&dotdot, 0, sizeof(dotdot));

        memset(dot.name, ' ', sizeof(dot.name));
        dot.name[0] = '.';
        dot.attr = FAT_ATTR_DIRECTORY;
        dot.cluster_low = first_cluster;
        dot.create_date = FAT_DATE_EPOCH;
        dot.write_date = FAT_DATE_EPOCH;

        memset(dotdot.name, ' ', sizeof(dotdot.name));
        dotdot.name[0] = '.';
        dotdot.name[1] = '.';
        dotdot.attr = FAT_ATTR_DIRECTORY;
        dotdot.cluster_low = parent_ent.first_cluster;
        dotdot.create_date = FAT_DATE_EPOCH;
        dotdot.write_date = FAT_DATE_EPOCH;

        memcpy(&sector[0], &dot, DIRENT_SIZE);
        memcpy(&sector[DIRENT_SIZE], &dotdot, DIRENT_SIZE);

        if (!write_sector(cluster_lba(first_cluster), sector)) {
            (void)fat_free_chain(first_cluster);
            return FAT_CREATE_IO;
        }
    }

    u64 lba = 0;
    u32 index = 0;

    if (!dirent_find_free(parent_ent.first_cluster, &lba, &index)) {
        if (first_cluster)
            (void)fat_free_chain(first_cluster);
        return FAT_CREATE_FULL;
    }

    /* The name goes in last. Everything it will point at - the cluster, the
     * "." and ".." entries - is already on the disk, so the instant this
     * entry becomes visible it is already correct. */
    u8 sector[BLOCK_SIZE];
    struct fat_raw_dirent raw;

    if (!read_sector_raw(lba, sector)) {
        if (first_cluster)
            (void)fat_free_chain(first_cluster);
        return FAT_CREATE_IO;
    }

    memset(&raw, 0, sizeof(raw));
    memcpy(raw.name, encoded, sizeof(encoded));
    raw.attr = directory ? FAT_ATTR_DIRECTORY : FAT_ATTR_ARCHIVE;
    raw.cluster_low = first_cluster;
    raw.size = 0;
    raw.create_date = FAT_DATE_EPOCH;
    raw.write_date = FAT_DATE_EPOCH;
    raw.access_date = FAT_DATE_EPOCH;

    memcpy(&sector[index * DIRENT_SIZE], &raw, DIRENT_SIZE);

    if (!write_sector(lba, sector)) {
        if (first_cluster)
            (void)fat_free_chain(first_cluster);
        return FAT_CREATE_IO;
    }

    stat_dirent_updates++;

    if (out)
        fill_dirent(&raw, out, lba, index);

    pr_debug("created %s '%s'", directory ? "directory" : "file", path);

    return FAT_CREATE_OK;
}

bool fat16_create(const char *path, bool directory, struct fat_dirent *out)
{
    return fat16_create_ex(path, directory, out) == FAT_CREATE_OK;
}

/* ---- writing file data ------------------------------------------------- */

i32 fat16_write(struct fat_dirent *file, u32 offset, const void *buf, u32 len)
{
    if (!fat16_writable() || !file || !buf)
        return -1;

    if (file->is_dir) {
        pr_debug("write to '%s', which is a directory", file->name);
        return -1;
    }

    if (len == 0)
        return 0;

    /* A write that would take the file past 4 GiB cannot be recorded: the
     * size field in a directory entry is 32 bits. Refused here rather than
     * silently wrapping. */
    if (offset + len < offset)
        return -1;

    const u8 *src = buf;
    u32 done = 0;
    u32 cluster_bytes = info.cluster_bytes;

    /* A hole. Writing past the end of a file has to leave readable zeros in
     * between, not whatever was in the newly allocated clusters - and
     * cluster_zero() in the growth path is what provides that, so there is
     * nothing extra to do here beyond letting the size cover the gap. */
    while (done < len) {
        u32 pos = offset + done;
        u32 index = pos / cluster_bytes;
        u32 within = pos % cluster_bytes;
        u32 chunk = cluster_bytes - within;

        if (chunk > len - done)
            chunk = len - done;

        u16 cluster = chain_cluster_at(&file->first_cluster, index, true);

        if (!cluster)
            break;

        /* Sector at a time inside the cluster, read-modify-write when the
         * write does not cover a whole sector. A partial sector that was
         * written without reading first would zero the bytes either side of
         * it, which is the classic way to lose data while appearing to
         * write correctly. */
        u32 sector_in_cluster = within / BLOCK_SIZE;
        u32 sector_offset = within % BLOCK_SIZE;
        u32 copied = 0;

        while (copied < chunk) {
            u32 n = BLOCK_SIZE - sector_offset;

            if (n > chunk - copied)
                n = chunk - copied;

            u64 lba = cluster_lba(cluster) + sector_in_cluster;
            u8 sector[BLOCK_SIZE];

            if (n == BLOCK_SIZE) {
                memcpy(sector, src + done + copied, BLOCK_SIZE);
            } else {
                if (!read_sector_raw(lba, sector))
                    goto out;
                memcpy(&sector[sector_offset], src + done + copied, n);
            }

            if (!write_sector(lba, sector))
                goto out;

            copied += n;
            sector_offset = 0;
            sector_in_cluster++;
        }

        done += copied;
    }

out:
    if (done == 0)
        return -1;

    stat_writes++;

    /* The directory entry last, and only if something changed. This is the
     * step that publishes the new bytes: until the size covers them, a reader
     * following the rules cannot see them, which is exactly the property the
     * ordering is for. */
    if (offset + done > file->size || file->first_cluster) {
        if (offset + done > file->size)
            file->size = offset + done;

        if (!dirent_commit(file))
            return -1;
    }

    return (i32)done;
}

bool fat16_truncate(struct fat_dirent *file, u32 length)
{
    if (!fat16_writable() || !file || file->is_dir)
        return false;

    if (length == file->size)
        return true;

    if (length > file->size) {
        /* Growing by truncate means a hole, and a hole has to read as zeros.
         * The simplest honest implementation is to write the zeros, because
         * FAT has no way to record a sparse region - a file's clusters are a
         * chain, and a chain has no gaps. */
        u8 zeros[BLOCK_SIZE];
        u32 remaining = length - file->size;
        u32 at = file->size;

        memset(zeros, 0, sizeof(zeros));

        while (remaining > 0) {
            u32 n = remaining > BLOCK_SIZE ? BLOCK_SIZE : remaining;
            i32 wrote = fat16_write(file, at, zeros, n);

            if (wrote <= 0)
                return false;

            at += (u32)wrote;
            remaining -= (u32)wrote;
        }

        return true;
    }

    /* Shrinking. The clusters past the new end are freed, and the size comes
     * down first: a reader that saw the old size with the clusters already
     * gone would read from freed space. Name withdrawn first, again. */
    u32 keep = (length + info.cluster_bytes - 1) / info.cluster_bytes;
    u32 old_size = file->size;
    u16 old_first = file->first_cluster;

    file->size = length;

    if (keep == 0)
        file->first_cluster = 0;

    if (!dirent_commit(file)) {
        file->size = old_size;
        file->first_cluster = old_first;
        return false;
    }

    if (keep == 0) {
        if (old_first)
            (void)fat_free_chain(old_first);
        return true;
    }

    /* Walk to the last cluster being kept, cut the chain there, then free the
     * tail. Cutting before freeing means the surviving part never points at
     * a cluster that has been returned to the allocator. */
    u16 cluster = old_first;

    for (u32 i = 1; i < keep && cluster; i++)
        cluster = fat_next(cluster);

    if (!cluster || is_end_of_chain(cluster))
        return true;

    u16 tail = fat_next(cluster);

    if (!fat_set(cluster, FAT_EOC_FIRST | 0x7))
        return false;

    if (tail && !is_end_of_chain(tail))
        (void)fat_free_chain(tail);

    return true;
}

/* ---- removing and renaming -------------------------------------------- */

static bool directory_is_empty(u16 cluster)
{
    struct dir_cursor c;
    struct fat_raw_dirent raw;
    bool done;

    cursor_init(&c, cluster);

    while (cursor_next(&c, &raw, &done)) {
        char name[FAT_NAME_MAX];

        decode_name(raw.name, name);

        /* "." and ".." are not contents. Every other entry is. */
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        return false;
    }

    return true;
}

static bool remove_entry(const char *path, bool want_dir)
{
    struct fat_dirent ent;

    if (!fat16_writable() || !path)
        return false;

    if (path[0] == '/' && path[1] == '\0') {
        pr_warn("refusing to remove the root directory");
        return false;
    }

    if (!fat16_stat(path, &ent))
        return false;

    if (ent.is_dir != want_dir) {
        pr_debug("'%s' is %s", path, ent.is_dir ? "a directory" : "a file");
        return false;
    }

    if (want_dir && !directory_is_empty(ent.first_cluster)) {
        pr_debug("'%s' is not empty", path);
        return false;
    }

    u16 chain = ent.first_cluster;

    /* The name goes first. Until this write lands the file is entirely
     * intact; after it lands the clusters are unreachable but still marked
     * in use, which leaks space and corrupts nothing. The other order would
     * free clusters that a still-readable name points at. */
    if (!dirent_update(ent.ent_lba, ent.ent_index, edit_delete, NULL))
        return false;

    if (chain)
        (void)fat_free_chain(chain);

    pr_debug("removed %s '%s'", want_dir ? "directory" : "file", path);

    return true;
}

bool fat16_unlink(const char *path)
{
    return remove_entry(path, false);
}

bool fat16_rmdir(const char *path)
{
    return remove_entry(path, true);
}

bool fat16_mkdir(const char *path)
{
    return fat16_create_ex(path, true, NULL) == FAT_CREATE_OK;
}

bool fat16_rename(const char *from, const char *to)
{
    struct fat_dirent src;
    struct fat_dirent dst_parent;
    struct fat_dirent clash;
    char from_parent[FAT_PATH_MAX];
    char from_leaf[FAT_PATH_MAX];
    char to_parent[FAT_PATH_MAX];
    char to_leaf[FAT_PATH_MAX];
    u8 encoded[11];

    if (!fat16_writable() || !from || !to)
        return false;

    if (!split_path(from, from_parent, sizeof(from_parent), from_leaf,
                    sizeof(from_leaf)) ||
        !split_path(to, to_parent, sizeof(to_parent), to_leaf, sizeof(to_leaf)))
        return false;

    if (!encode_name(to_leaf, encoded)) {
        pr_warn("'%s' is not expressible as an 8.3 name", to_leaf);
        return false;
    }

    if (!fat16_stat(from, &src))
        return false;

    if (!fat16_stat(to_parent, &dst_parent) || !dst_parent.is_dir)
        return false;

    if (lookup_in(dst_parent.first_cluster, to_leaf, &clash)) {
        pr_debug("'%s' already exists", to);
        return false;
    }

    /* Within one directory this is a name change and nothing else - the
     * entry stays where it is, so there is no window in which the file has
     * two names or none. Across directories it has to be a new entry plus a
     * deletion, and there is no way to do those two writes at once; the new
     * name is written first, so a crash leaves the file reachable under both
     * names rather than under neither. */
    if (strcmp(from_parent, to_parent) == 0)
        return dirent_update(src.ent_lba, src.ent_index, edit_rename, encoded);

    u64 lba = 0;
    u32 index = 0;

    if (!dirent_find_free(dst_parent.first_cluster, &lba, &index))
        return false;

    u8 sector[BLOCK_SIZE];
    struct fat_raw_dirent raw;

    if (!read_sector_raw(src.ent_lba, sector))
        return false;

    memcpy(&raw, &sector[src.ent_index * DIRENT_SIZE], DIRENT_SIZE);
    memcpy(raw.name, encoded, sizeof(encoded));

    if (!read_sector_raw(lba, sector))
        return false;

    memcpy(&sector[index * DIRENT_SIZE], &raw, DIRENT_SIZE);

    if (!write_sector(lba, sector))
        return false;

    stat_dirent_updates++;

    return dirent_update(src.ent_lba, src.ent_index, edit_delete, NULL);
}

bool fat16_sync(void)
{
    if (!fat16_writable())
        return false;

    return blockdev_flush(device);
}

bool fat16_space(u32 *free_clusters, u32 *total_clusters)
{
    if (!info.mounted)
        return false;

    if (total_clusters)
        *total_clusters = info.cluster_count;

    if (!free_clusters)
        return true;

    u32 free_count = 0;

    for (u16 c = 2; c < info.cluster_count + 2; c++) {
        u16 entry;

        /* Same reason as the allocator: an unreadable entry counted as free
         * would overstate the space available, and the caller would believe
         * a write could succeed that cannot. */
        if (fat_get(c, &entry) && entry == FAT_FREE)
            free_count++;
    }

    *free_clusters = free_count;

    return true;
}

void fat16_write_stats(u32 *writes, u32 *sectors, u32 *allocated, u32 *freed,
                       u32 *dirents)
{
    if (writes)
        *writes = stat_writes;
    if (sectors)
        *sectors = stat_sectors_written;
    if (allocated)
        *allocated = stat_clusters_allocated;
    if (freed)
        *freed = stat_clusters_freed;
    if (dirents)
        *dirents = stat_dirent_updates;
}
