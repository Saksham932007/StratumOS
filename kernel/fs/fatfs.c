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

/* One place that turns a FAT create failure into an errno, used by both
 * open(O_CREAT) and mkdir() so the two cannot disagree about what a given
 * failure means. */
static i32 create_error_to_errno(enum fat_create_error e)
{
    switch (e) {
    case FAT_CREATE_OK:
        return 0;
    case FAT_CREATE_NO_PARENT:
        return SYS_ENOENT;
    case FAT_CREATE_BAD_NAME:
        /* 8.3 cannot hold the name. ENAMETOOLONG rather than EINVAL because
         * that is overwhelmingly the reason - and a caller that shortens the
         * name will succeed, which is what the error should suggest. */
        return SYS_ENAMETOOLONG;
    case FAT_CREATE_BAD_PATH:
        return SYS_EINVAL;
    case FAT_CREATE_EXISTS:
        return SYS_EEXIST;
    case FAT_CREATE_FULL:
        return SYS_ENOSPC;
    case FAT_CREATE_IO:
    default:
        return SYS_EIO;
    }
}

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
static u32 writes;
static u32 bytes_written;

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

static i32 fatfs_write(struct vfs_file *f, const void *buf, u32 len)
{
    struct fatfs_open *o = f->priv;

    if (!o)
        return SYS_EBADF;

    i32 wrote = fat16_write(&o->ent, f->offset, buf, len);

    if (wrote < 0)
        return SYS_EIO;

    f->offset += (u32)wrote;
    writes++;
    bytes_written += (u32)wrote;

    /* A short write is a full disk, not an error. The caller sees fewer bytes
     * than it asked for and can decide what to do; returning ENOSPC after
     * having written half of them would lose the information that half got
     * through. */
    return wrote;
}

static i32 fatfs_truncate(struct vfs_file *f, u32 length)
{
    struct fatfs_open *o = f->priv;

    if (!o)
        return SYS_EBADF;

    if (!fat16_truncate(&o->ent, length))
        return SYS_EIO;

    /* The position is clamped rather than left where it was. A descriptor
     * sitting past the end of a file it just shortened would have its next
     * write open a hole, which is not what truncate was asked to do. */
    if (f->offset > o->ent.size)
        f->offset = o->ent.size;

    return 0;
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
     * offset cannot wrap into a plausible-looking position.
     *
     * A seek past the end is allowed on a descriptor that can write, because
     * writing there is how a file gets a hole - and fat16_write() fills the
     * gap with zeros, so the hole is readable rather than whatever the
     * clusters used to hold. On a read-only descriptor the same seek is
     * refused: it would leave the position somewhere no read could ever
     * return from, which hides the caller's arithmetic error instead of
     * reporting it. */
    i64 want = base + (i64)offset;
    bool may_write = (f->flags & O_ACCMODE) != O_RDONLY;
    i64 limit = may_write ? (i64)0xFFFFFFFFll : (i64)o->ent.size;

    if (want < 0 || want > limit)
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
    .write = fatfs_write,
    .seek = fatfs_seek,
    .stat = fatfs_stat_file,
    .truncate = fatfs_truncate,
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

    bool wants_write = (flags & O_ACCMODE) != O_RDONLY;

    /* The root directory has no directory entry of its own to find, so it is
     * synthesised. Without this, opendir("/") - the first thing any `ls`
     * does - would fail on a filesystem that is plainly mounted. */
    bool is_root = path[0] == '/' && path[1] == '\0';
    bool exists;

    if (is_root) {
        memset(&ent, 0, sizeof(ent));
        strlcpy(ent.name, "/", sizeof(ent.name));
        ent.is_dir = true;
        ent.attr = FAT_ATTR_DIRECTORY;
        exists = true;
    } else {
        exists = fat16_stat(path, &ent);
    }

    /* The flag combinations, in the order their refusals are most specific.
     *
     * O_EXCL before "does not exist", because a caller that asked for
     * exclusive creation and found the file already there wants EEXIST, not a
     * report about what it was going to do. */
    if (exists && (flags & O_CREAT) && (flags & O_EXCL))
        return SYS_EEXIST;

    if (exists && ent.is_dir && wants_write)
        return SYS_EISDIR;

    if (!exists) {
        if (!(flags & O_CREAT))
            return SYS_ENOENT;

        if (!fat16_writable())
            return SYS_EROFS;

        /* Created only now, after every reason to refuse has been ruled out.
         * A create that happened earlier and was then rejected for a flag
         * would leave an empty file behind as a side effect of a failed
         * open. */
        enum fat_create_error e = fat16_create_ex(path, false, &ent);

        if (e != FAT_CREATE_OK)
            return create_error_to_errno(e);
    }

    if (wants_write && !fat16_writable())
        return SYS_EROFS;

    /* O_TRUNC on an existing file, once it is known that writing is allowed.
     * Truncation is a modification, so it needs write access - a read-only
     * open with O_TRUNC is a contradiction rather than a request. */
    if (exists && (flags & O_TRUNC) && !ent.is_dir) {
        if (!wants_write)
            return SYS_EINVAL;

        if (!fat16_truncate(&ent, 0))
            return SYS_EIO;
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
        out->mode = S_MODE_READ | (fat16_writable() ? S_MODE_WRITE : 0);
        return 0;
    }

    if (!fat16_stat(path, &ent))
        return SYS_ENOENT;

    out->size = ent.size;
    out->type = ent.is_dir ? S_TYPE_DIR : S_TYPE_FILE;
    out->mode = S_MODE_READ | (fat16_writable() ? S_MODE_WRITE : 0);
    out->blocks =
        info && info->cluster_bytes
            ? (ent.size + info->cluster_bytes - 1) / info->cluster_bytes
            : 0;

    return 0;
}

/* "/" rather than "/mnt" or anything else: this is the root filesystem, and
 * the prefix match in the VFS makes it the fallback for every path no other
 * filesystem claims. */
static i32 fatfs_unlink(const char *path)
{
    struct fat_dirent ent;

    if (!fat16_mounted())
        return SYS_ENOENT;
    if (!fat16_writable())
        return SYS_EROFS;

    /* Distinguished before attempting it, so the caller is told which of the
     * two reasons applied. fat16_unlink() refuses a directory too, but it can
     * only return false, and "no such file" and "that is a directory" lead a
     * caller to do different things. */
    if (!fat16_stat(path, &ent))
        return SYS_ENOENT;

    if (ent.is_dir)
        return SYS_EISDIR;

    return fat16_unlink(path) ? 0 : SYS_EIO;
}

static i32 fatfs_mkdir(const char *path)
{
    struct fat_dirent ent;

    if (!fat16_mounted())
        return SYS_ENOENT;
    if (!fat16_writable())
        return SYS_EROFS;

    if (fat16_stat(path, &ent))
        return SYS_EEXIST;

    return create_error_to_errno(fat16_create_ex(path, true, NULL));
}

static i32 fatfs_rmdir(const char *path)
{
    struct fat_dirent ent;

    if (!fat16_mounted())
        return SYS_ENOENT;
    if (!fat16_writable())
        return SYS_EROFS;

    if (!fat16_stat(path, &ent))
        return SYS_ENOENT;

    if (!ent.is_dir)
        return SYS_ENOTDIR;

    /* The only remaining reason fat16_rmdir() refuses a directory that exists
     * and is a directory is that it still has something in it. */
    return fat16_rmdir(path) ? 0 : SYS_ENOTEMPTY;
}

static i32 fatfs_rename(const char *from, const char *to)
{
    struct fat_dirent ent;

    if (!fat16_mounted())
        return SYS_ENOENT;
    if (!fat16_writable())
        return SYS_EROFS;

    if (!fat16_stat(from, &ent))
        return SYS_ENOENT;

    if (fat16_stat(to, &ent))
        return SYS_EEXIST;

    return fat16_rename(from, to) ? 0 : SYS_EIO;
}

static i32 fatfs_sync(void)
{
    if (!fat16_mounted())
        return 0;

    return fat16_sync() ? 0 : SYS_EIO;
}

static const struct vfs_fs fatfs = {
    .prefix = "/",
    .name = "fat16",
    .open = fatfs_open,
    .stat = fatfs_stat_path,
    .unlink = fatfs_unlink,
    .mkdir = fatfs_mkdir,
    .rmdir = fatfs_rmdir,
    .rename = fatfs_rename,
    .sync = fatfs_sync,
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

void fatfs_write_counters(u32 *w, u32 *b)
{
    if (w)
        *w = writes;
    if (b)
        *b = bytes_written;
}
