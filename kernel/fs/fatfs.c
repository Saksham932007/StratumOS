/* StratumOS - the FAT16 filesystem behind the VFS.
 *
 * The thin half of the work. fs/fat16.c knows about boot sectors, allocation
 * tables and 8.3 names; this file knows about `struct vfs_ops` and nothing
 * else. Keeping them apart is what makes the FAT driver testable without a
 * descriptor table and the VFS testable without a disk - the two in-kernel
 * suites for them do exactly that.
 *
 * An open file holds a copy of its directory entry rather than a path. A path
 * would have to be re-resolved on every read, which is a disk lookup per
 * read; the entry has the first cluster and the size, which is everything a
 * read needs. The cost is that a file renamed under an open descriptor keeps
 * being read from where it was, which is also what every Unix does, and for
 * the same reason.
 *
 * Nothing here writes yet. The VFS turns a missing ->write into SYS_EROFS,
 * so a program that tries gets a truthful refusal rather than a crash, and
 * the refusal names the real reason: fat16.c cannot write. That arrives in
 * its own commit, because allocating from a FAT and updating both copies of
 * it is a correctness problem with a recovery story, not a plumbing one.
 */
#define LOG_TAG "fatfs"

#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/string.h>

#include <mm/heap.h>

#include <fs/fat16.h>
#include <fs/vfs.h>

/* The per-open state: the directory entry, copied. Small enough that the
 * allocation is the only reason this is not stored inline in struct vfs_file,
 * and it is kept out of there so that struct vfs_file does not grow a member
 * that only one backend uses. */
struct fatfs_open {
    struct fat_dirent ent;
};

static u32 opens;
static u32 reads;
static u32 bytes_read;

static i32 fatfs_read(struct vfs_file *f, void *buf, u32 len)
{
    struct fatfs_open *o = f->priv;

    if (!o)
        return SYS_EBADF;

    /* At or past the end is end of file, not an error. The distinction
     * matters to a caller looping until zero. */
    if (f->offset >= o->ent.size)
        return 0;

    u32 left = o->ent.size - f->offset;

    if (len > left)
        len = left;

    i32 got = fat16_read(&o->ent, f->offset, buf, len);

    if (got < 0)
        return SYS_EIO;

    f->offset += (u32)got;
    reads++;
    bytes_read += (u32)got;

    return got;
}

static i32 fatfs_seek(struct vfs_file *f, i32 offset, int whence)
{
    struct fatfs_open *o = f->priv;
    i64 base;

    if (!o)
        return SYS_EBADF;

    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = (i64)f->offset;
        break;
    case SEEK_END:
        base = (i64)o->ent.size;
        break;
    default:
        return SYS_EINVAL;
    }

    /* Computed in 64 bits and then checked, so that a negative or enormous
     * offset cannot wrap into a plausible-looking position. A seek past the
     * end is legal on a real filesystem and would create a hole on write; it
     * is rejected here because this driver cannot write, and allowing a
     * position no read could ever use would only hide the error. */
    i64 want = base + (i64)offset;

    if (want < 0 || want > (i64)o->ent.size)
        return SYS_EINVAL;

    f->offset = (u32)want;

    return (i32)want;
}

static i32 fatfs_stat_file(struct vfs_file *f, struct sys_stat *out)
{
    struct fatfs_open *o = f->priv;
    const struct fat_info *info = fat16_get_info();

    if (!o)
        return SYS_EBADF;

    out->size = o->ent.size;
    out->type = o->ent.is_dir ? S_TYPE_DIR : S_TYPE_FILE;
    out->blocks =
        info && info->cluster_bytes
            ? (o->ent.size + info->cluster_bytes - 1) / info->cluster_bytes
            : 0;

    return 0;
}

static i32 fatfs_getdent(struct vfs_file *f, u32 index, struct sys_dirent *out)
{
    struct fat_dirent ent;

    if (!fat16_readdir(f->path, index, &ent))
        return 0; /* past the last entry */

    strlcpy(out->name, ent.name, sizeof(out->name));
    out->type = ent.is_dir ? S_TYPE_DIR : S_TYPE_FILE;
    out->size = ent.size;

    return 1;
}

static void fatfs_close(struct vfs_file *f)
{
    kfree(f->priv);
    f->priv = NULL;
}

static const struct vfs_ops fatfs_file_ops = {
    .name = "fat16",
    .read = fatfs_read,
    .seek = fatfs_seek,
    .stat = fatfs_stat_file,
    .close = fatfs_close,
};

static const struct vfs_ops fatfs_dir_ops = {
    .name = "fat16dir",
    .stat = fatfs_stat_file,
    .getdent = fatfs_getdent,
    .close = fatfs_close,
};

static i32 fatfs_open(const char *path, u32 flags, struct vfs_file *f)
{
    struct fat_dirent ent;

    if (!fat16_mounted())
        return SYS_ENOENT;

    /* Read-only, and the check goes *before* the lookup deliberately.
     *
     * Doing it after meant that open("/new.txt", O_WRONLY | O_CREAT) failed
     * the lookup first and returned ENOENT - which tells the caller the file
     * is missing and invites it to create one, which is the thing that
     * cannot work. EROFS says the filesystem will never accept a write, so
     * retrying is pointless. The suite asserts this ordering because the
     * first version of this file had it the wrong way round.
     *
     * O_TRUNC is included for the same reason: it modifies the file. */
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC)))
        return SYS_EROFS;

    /* The root directory has no directory entry of its own to find, so it is
     * synthesised. Without this, opendir("/") - the first thing any `ls`
     * does - would fail on a filesystem that is plainly mounted. */
    bool is_root = path[0] == '/' && path[1] == '\0';

    if (is_root) {
        memset(&ent, 0, sizeof(ent));
        strlcpy(ent.name, "/", sizeof(ent.name));
        ent.is_dir = true;
        ent.attr = FAT_ATTR_DIRECTORY;
    } else if (!fat16_stat(path, &ent)) {
        return SYS_ENOENT;
    }

    if (!ent.is_dir && (flags & O_DIRECTORY))
        return SYS_ENOTDIR;

    struct fatfs_open *o = kmalloc(sizeof(*o));

    if (!o)
        return SYS_ENFILE;

    o->ent = ent;

    f->priv = o;
    f->type = ent.is_dir ? S_TYPE_DIR : S_TYPE_FILE;
    f->ops = ent.is_dir ? &fatfs_dir_ops : &fatfs_file_ops;
    f->offset = 0;

    opens++;

    return 0;
}

static i32 fatfs_stat_path(const char *path, struct sys_stat *out)
{
    struct fat_dirent ent;
    const struct fat_info *info = fat16_get_info();

    if (!fat16_mounted())
        return SYS_ENOENT;

    if (path[0] == '/' && path[1] == '\0') {
        out->type = S_TYPE_DIR;
        out->mode = S_MODE_READ;
        return 0;
    }

    if (!fat16_stat(path, &ent))
        return SYS_ENOENT;

    out->size = ent.size;
    out->type = ent.is_dir ? S_TYPE_DIR : S_TYPE_FILE;
    out->mode = S_MODE_READ;
    out->blocks =
        info && info->cluster_bytes
            ? (ent.size + info->cluster_bytes - 1) / info->cluster_bytes
            : 0;

    return 0;
}

/* "/" rather than "/mnt" or anything else: this is the root filesystem, and
 * the prefix match in the VFS makes it the fallback for every path no other
 * filesystem claims. */
static const struct vfs_fs fatfs = {
    .prefix = "/",
    .name = "fat16",
    .open = fatfs_open,
    .stat = fatfs_stat_path,
};

void fatfs_init(void)
{
    vfs_register(&fatfs);
}

void fatfs_counters(u32 *o, u32 *r, u32 *b)
{
    if (o)
        *o = opens;
    if (r)
        *r = reads;
    if (b)
        *b = bytes_read;
}
