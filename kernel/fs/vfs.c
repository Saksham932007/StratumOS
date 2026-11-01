/* StratumOS - the virtual filesystem and the descriptor table.
 *
 * The argument for this layer's existence, and for descriptors and open files
 * being two objects rather than one, is in fs/vfs.h. This file is the
 * mechanism: a fixed pool of open files, a prefix-matched mount table, a path
 * resolver, and the per-process descriptor table that ring 3 actually sees.
 *
 * One rule runs through all of it. Every path that arrives from ring 3 is
 * resolved through vfs_resolve() before any backend sees it, and
 * vfs_resolve() is the only thing in the kernel that is allowed to turn a
 * relative path into an absolute one. A backend that received raw user input
 * would have to defend itself against "../../" on its own, and the first
 * backend that forgot would be a filesystem escape. There is one resolver so
 * that there is one place to get it right.
 */
#define LOG_TAG "vfs"

#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/string.h>

#include <fs/vfs.h>

#define VFS_FS_MAX 4

static struct vfs_file open_files[VFS_OPEN_MAX];
static const struct vfs_fs *filesystems[VFS_FS_MAX];
static u32 fs_count;

static u32 open_now;
static u32 open_peak;
static u32 call_count;

/* A descriptor table for kernel threads, which have no `struct task` of their
 * own early in boot and still need somewhere for fd_* to look. It is never
 * populated with anything but the console, and a kernel thread that opens a
 * file gets its own table once it has a task. */
static struct task_files kernel_files;

void vfs_init(void)
{
    for (u32 i = 0; i < VFS_OPEN_MAX; i++)
        open_files[i].refcount = 0;

    fd_table_init(&kernel_files);

    pr_info("%u open-file slots, %u descriptors per process", VFS_OPEN_MAX,
            SYS_FDS_MAX);
}

bool vfs_register(const struct vfs_fs *fs)
{
    if (!fs || !fs->prefix || !fs->open)
        return false;

    if (fs_count >= VFS_FS_MAX) {
        pr_err("cannot register \"%s\": only %u filesystems fit", fs->name,
               VFS_FS_MAX);
        return false;
    }

    filesystems[fs_count++] = fs;
    pr_info("registered \"%s\" at %s", fs->name, fs->prefix);
    return true;
}

/* Longest matching prefix wins, so registration order cannot change
 * behaviour. "/" matches everything and is therefore always the loser of a
 * tie, which is what makes it the fallback without needing to be marked as
 * one. */
static const struct vfs_fs *fs_for(const char *path)
{
    const struct vfs_fs *best = NULL;
    size_t best_len = 0;

    for (u32 i = 0; i < fs_count; i++) {
        const char *p = filesystems[i]->prefix;
        size_t n = strlen(p);

        if (strncmp(path, p, n) != 0)
            continue;

        if (!best || n > best_len) {
            best = filesystems[i];
            best_len = n;
        }
    }

    return best;
}

/* ---- path resolution ---------------------------------------------------
 *
 * Builds an absolute, canonical path from a base and a possibly-relative
 * input. The output is assembled component by component into a bounded
 * buffer, which is the only form of this function that cannot be made to
 * write past its own end by a long input.
 *
 * ".." pops a component, and popping an empty path is a no-op rather than an
 * error. That is deliberate: "/.." is "/" on every system worth copying, and
 * treating it as an error would mean a path that escapes the root is
 * *reported* rather than *prevented*, which is the wrong half of the problem
 * to solve. There is no way to express a path above the root in the output,
 * so there is nothing for a backend to reject.
 *
 * Case is folded upward. The only disk filesystem here is FAT16, whose names
 * are 8.3 upper case, and the device filesystem's names are ASCII with no
 * case collisions - so folding here means "/BIN/INIT" and "/bin/init" are one
 * path everywhere, rather than in whichever backends remembered.
 */
bool vfs_resolve(const char *cwd, const char *path, char *out, size_t outlen)
{
    if (!path || !out || outlen < 2)
        return false;

    /* The working buffer holds the result as it is built. A component is
     * appended with a bounds check; an overlong path fails rather than being
     * truncated, because a truncated path names a different file. */
    size_t len = 0;

    out[0] = '\0';

    if (path[0] != '/') {
        const char *base = (cwd && cwd[0] == '/') ? cwd : "/";
        size_t n = strlen(base);

        if (n >= outlen)
            return false;

        memcpy(out, base, n);
        len = n;

        /* Drop a trailing slash so appending is uniform below. "/" becomes
         * "" and gains its slash back from the first component. */
        while (len > 0 && out[len - 1] == '/')
            len--;
    }

    const char *p = path;

    while (*p) {
        while (*p == '/')
            p++;

        if (!*p)
            break;

        const char *start = p;

        while (*p && *p != '/')
            p++;

        size_t clen = (size_t)(p - start);

        if (clen == 1 && start[0] == '.')
            continue;

        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            while (len > 0 && out[len - 1] != '/')
                len--;
            /* Remove the slash itself, except for the one at the root. */
            if (len > 0)
                len--;
            continue;
        }

        if (clen > SYS_DIRENT_NAME_MAX + 2) {
            /* A single component longer than any name a backend here can hold
             * cannot resolve to anything. Rejecting it at the resolver keeps
             * the bound in one place. */
            return false;
        }

        if (len + 1 + clen >= outlen)
            return false;

        out[len++] = '/';
        for (size_t i = 0; i < clen; i++) {
            char c = start[i];
            out[len++] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        }
    }

    if (len == 0) {
        out[0] = '/';
        len = 1;
    }

    out[len] = '\0';
    return true;
}

/* ---- the open-file pool ------------------------------------------------ */

static struct vfs_file *slot_alloc(void)
{
    for (u32 i = 0; i < VFS_OPEN_MAX; i++) {
        if (open_files[i].refcount != 0)
            continue;

        struct vfs_file *f = &open_files[i];

        memset(f, 0, sizeof(*f));
        f->refcount = 1;

        if (++open_now > open_peak)
            open_peak = open_now;

        return f;
    }

    /* Counted rather than printed at the default level: a ring-3 program can
     * provoke this as fast as it can issue system calls, and a kernel that
     * floods its own console in response to a userspace loop has turned a
     * refusal into a denial of service. The same reasoning as the rest of
     * this kernel's ratelimited paths - see docs/FUZZING.md. */
    pr_warn_ratelimited("every one of the %u open-file slots is in use",
                        VFS_OPEN_MAX);
    return NULL;
}

struct vfs_file *vfs_alloc(const struct vfs_ops *ops, u8 type, u32 flags,
                           void *priv, const char *name)
{
    struct vfs_file *f = slot_alloc();

    if (!f)
        return NULL;

    f->ops = ops;
    f->type = type;
    f->flags = flags;
    f->priv = priv;
    strlcpy(f->path, name ? name : "", sizeof(f->path));

    return f;
}

void vfs_ref(struct vfs_file *f)
{
    if (f)
        f->refcount++;
}

void vfs_put(struct vfs_file *f)
{
    if (!f)
        return;

    if (f->refcount == 0) {
        /* A double close is a kernel bug, not a userspace one - fd_close()
         * clears the descriptor before it gets here, so ring 3 cannot reach
         * this twice for one descriptor. Loud, because it means the reference
         * counting is wrong somewhere and the next symptom would be a reused
         * slot serving two unrelated files. */
        panic("vfs_put on a slot with no references (%s)", f->path);
    }

    if (--f->refcount > 0)
        return;

    if (f->ops && f->ops->close)
        f->ops->close(f);

    f->ops = NULL;
    f->priv = NULL;

    if (open_now > 0)
        open_now--;
}

struct vfs_file *vfs_open(const char *path, u32 flags, i32 *err)
{
    i32 e = SYS_ENOENT;

    if (err)
        *err = e;

    if (!path || path[0] != '/') {
        if (err)
            *err = SYS_EINVAL;
        return NULL;
    }

    const struct vfs_fs *fs = fs_for(path);

    if (!fs) {
        if (err)
            *err = SYS_ENOENT;
        return NULL;
    }

    struct vfs_file *f = slot_alloc();

    if (!f) {
        if (err)
            *err = SYS_ENFILE;
        return NULL;
    }

    strlcpy(f->path, path, sizeof(f->path));
    f->flags = flags;

    e = fs->open(path, flags, f);

    if (e < 0) {
        /* The backend did not take ownership of anything, so the slot is
         * released directly rather than through vfs_put() - there is no
         * ->close to call on a file that never opened. */
        f->refcount = 0;
        f->ops = NULL;
        f->priv = NULL;
        if (open_now > 0)
            open_now--;
        if (err)
            *err = e;
        return NULL;
    }

    /* O_APPEND positions at the end once, here, rather than being re-checked
     * on every write. That is the weaker of the two possible meanings - a
     * real O_APPEND re-seeks atomically per write, so two processes sharing
     * the descriptor cannot overwrite each other - and the difference only
     * shows with concurrent writers to one file, which nothing here does
     * yet. Noted rather than silently chosen: the stronger version belongs
     * with the write path's locking, not before it. */
    if ((flags & O_APPEND) && f->ops && f->ops->seek) {
        i32 end = f->ops->seek(f, 0, SEEK_END);

        if (end > 0)
            f->offset = (u32)end;
    }

    if (err)
        *err = 0;

    return f;
}

/* ---- dispatch ----------------------------------------------------------
 *
 * Each of these checks the access mode and the presence of the operation,
 * then calls it. The checks are here rather than in the backends so that a
 * new backend cannot forget one: a socket author should not have to remember
 * that a read-only descriptor must reject writes.
 */

static bool may_read(const struct vfs_file *f)
{
    u32 acc = f->flags & O_ACCMODE;

    return acc == O_RDONLY || acc == O_RDWR;
}

static bool may_write(const struct vfs_file *f)
{
    u32 acc = f->flags & O_ACCMODE;

    return acc == O_WRONLY || acc == O_RDWR;
}

i32 vfs_read(struct vfs_file *f, void *buf, u32 len)
{
    if (!f || !f->ops)
        return SYS_EBADF;
    if (!may_read(f))
        return SYS_EINVAL;
    if (f->type == S_TYPE_DIR)
        return SYS_EISDIR;
    if (!f->ops->read)
        return SYS_ENOTSUP;
    if (len == 0)
        return 0;

    call_count++;
    return f->ops->read(f, buf, len);
}

i32 vfs_write(struct vfs_file *f, const void *buf, u32 len)
{
    if (!f || !f->ops)
        return SYS_EBADF;
    if (!may_write(f))
        return SYS_EINVAL;
    if (f->type == S_TYPE_DIR)
        return SYS_EISDIR;
    if (!f->ops->write)
        return SYS_EROFS;
    if (len == 0)
        return 0;

    call_count++;
    return f->ops->write(f, buf, len);
}

i32 vfs_seek(struct vfs_file *f, i32 offset, int whence)
{
    if (!f || !f->ops)
        return SYS_EBADF;
    if (!f->ops->seek)
        return SYS_EINVAL;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)
        return SYS_EINVAL;

    call_count++;
    return f->ops->seek(f, offset, whence);
}

i32 vfs_stat_file(struct vfs_file *f, struct sys_stat *out)
{
    if (!f || !f->ops)
        return SYS_EBADF;
    if (!out)
        return SYS_EFAULT;

    memset(out, 0, sizeof(*out));
    out->type = f->type;
    out->mode =
        (may_read(f) ? S_MODE_READ : 0) | (may_write(f) ? S_MODE_WRITE : 0);

    call_count++;

    if (f->ops->stat)
        return f->ops->stat(f, out);

    return 0;
}

i32 vfs_getdent(struct vfs_file *f, u32 index, struct sys_dirent *out)
{
    if (!f || !f->ops)
        return SYS_EBADF;
    if (!out)
        return SYS_EFAULT;
    if (f->type != S_TYPE_DIR)
        return SYS_ENOTDIR;
    if (!f->ops->getdent)
        return SYS_ENOTSUP;

    call_count++;
    return f->ops->getdent(f, index, out);
}

i32 vfs_ioctl(struct vfs_file *f, u32 request, u32 arg)
{
    if (!f || !f->ops)
        return SYS_EBADF;
    if (!f->ops->ioctl)
        return SYS_ENOTSUP;

    call_count++;
    return f->ops->ioctl(f, request, arg);
}

i32 vfs_truncate_file(struct vfs_file *f, u32 length)
{
    if (!f || !f->ops)
        return SYS_EBADF;
    if (!may_write(f))
        return SYS_EINVAL;
    if (!f->ops->truncate)
        return SYS_ENOTSUP;

    call_count++;
    return f->ops->truncate(f, length);
}

bool vfs_poll_read(struct vfs_file *f)
{
    if (!f || !f->ops)
        return false;

    return f->ops->poll_read ? f->ops->poll_read(f) : true;
}

bool vfs_poll_write(struct vfs_file *f)
{
    if (!f || !f->ops)
        return false;

    return f->ops->poll_write ? f->ops->poll_write(f) : true;
}

/* ---- by path ----------------------------------------------------------- */

i32 vfs_stat(const char *path, struct sys_stat *out)
{
    const struct vfs_fs *fs;

    if (!path || !out)
        return SYS_EFAULT;

    fs = fs_for(path);

    if (!fs)
        return SYS_ENOENT;
    if (!fs->stat)
        return SYS_ENOTSUP;

    call_count++;
    memset(out, 0, sizeof(*out));
    return fs->stat(path, out);
}

i32 vfs_unlink(const char *path)
{
    const struct vfs_fs *fs;

    if (!path)
        return SYS_EFAULT;

    fs = fs_for(path);

    if (!fs)
        return SYS_ENOENT;
    if (!fs->unlink)
        return SYS_EROFS;

    call_count++;
    return fs->unlink(path);
}

i32 vfs_mkdir(const char *path)
{
    const struct vfs_fs *fs;

    if (!path)
        return SYS_EFAULT;

    fs = fs_for(path);

    if (!fs)
        return SYS_ENOENT;
    if (!fs->mkdir)
        return SYS_EROFS;

    call_count++;
    return fs->mkdir(path);
}

i32 vfs_rmdir(const char *path)
{
    const struct vfs_fs *fs;

    if (!path)
        return SYS_EFAULT;

    fs = fs_for(path);

    if (!fs)
        return SYS_ENOENT;
    if (!fs->rmdir)
        return SYS_EROFS;

    call_count++;
    return fs->rmdir(path);
}

i32 vfs_rename(const char *from, const char *to)
{
    const struct vfs_fs *fs;

    if (!from || !to)
        return SYS_EFAULT;

    fs = fs_for(from);

    if (!fs)
        return SYS_ENOENT;

    /* Renaming across filesystems would be a copy and a delete, which is a
     * different operation with different failure modes - a half-finished one
     * leaves two files or none. Refused rather than half-implemented. */
    if (fs_for(to) != fs)
        return SYS_EINVAL;

    if (!fs->rename)
        return SYS_EROFS;

    call_count++;
    return fs->rename(from, to);
}

i32 vfs_sync(void)
{
    i32 ret = 0;

    for (u32 i = 0; i < fs_count; i++) {
        if (!filesystems[i]->sync)
            continue;

        i32 e = filesystems[i]->sync();

        if (e < 0)
            ret = e;
    }

    return ret;
}

/* ---- the per-process descriptor table ---------------------------------- */

static struct task_files *current_files(void)
{
    struct task *t = task_current();

    return t ? &t->files : &kernel_files;
}

void fd_table_init(struct task_files *files)
{
    if (!files)
        return;

    for (u32 i = 0; i < SYS_FDS_MAX; i++)
        files->fd[i] = NULL;

    strlcpy(files->cwd, "/", sizeof(files->cwd));
}

void fd_table_clone(struct task_files *dst, const struct task_files *src)
{
    if (!dst || !src)
        return;

    for (u32 i = 0; i < SYS_FDS_MAX; i++) {
        dst->fd[i] = src->fd[i];
        /* A reference per descriptor, not per open file: the child now holds
         * its own descriptor onto the same object, and the shared offset that
         * follows from that is the behaviour fork() is expected to have. */
        vfs_ref(dst->fd[i]);
    }

    strlcpy(dst->cwd, src->cwd, sizeof(dst->cwd));
}

void fd_table_close_all(struct task_files *files)
{
    if (!files)
        return;

    for (u32 i = 0; i < SYS_FDS_MAX; i++) {
        if (!files->fd[i])
            continue;

        /* Cleared before the put, so that a close hook which somehow
         * re-enters this table cannot see a descriptor pointing at a slot
         * that is being torn down. */
        struct vfs_file *f = files->fd[i];

        files->fd[i] = NULL;
        vfs_put(f);
    }
}

bool fd_table_open_std(struct task_files *files)
{
    if (!files)
        return false;

    struct vfs_file *in = vfs_open("/DEV/CONSOLE", O_RDONLY, NULL);
    struct vfs_file *out = vfs_open("/DEV/CONSOLE", O_WRONLY, NULL);

    if (!in || !out) {
        if (in)
            vfs_put(in);
        if (out)
            vfs_put(out);
        pr_err("cannot open the standard descriptors");
        return false;
    }

    /* 1 and 2 are two descriptors onto one open file, which is what makes
     * stderr and stdout interleave in order rather than racing. */
    vfs_ref(out);

    files->fd[STDIN_FD] = in;
    files->fd[STDOUT_FD] = out;
    files->fd[STDERR_FD] = out;

    return true;
}

struct vfs_file *fd_get(i32 fd)
{
    struct task_files *files = current_files();

    if (fd < 0 || fd >= (i32)SYS_FDS_MAX)
        return NULL;

    return files->fd[fd];
}

static i32 fd_lowest_free(struct task_files *files)
{
    for (u32 i = 0; i < SYS_FDS_MAX; i++) {
        if (!files->fd[i])
            return (i32)i;
    }

    return SYS_EMFILE;
}

i32 fd_install(struct vfs_file *f)
{
    struct task_files *files = current_files();

    if (!f)
        return SYS_EBADF;

    i32 fd = fd_lowest_free(files);

    if (fd < 0)
        return fd;

    files->fd[fd] = f;
    return fd;
}

bool fd_resolve(const char *path, char *out, size_t outlen)
{
    return vfs_resolve(current_files()->cwd, path, out, outlen);
}

const char *fd_cwd(void)
{
    return current_files()->cwd;
}

i32 fd_open(const char *path, u32 flags)
{
    char abs[VFS_PATH_MAX];

    if (!fd_resolve(path, abs, sizeof(abs)))
        return SYS_ENAMETOOLONG;

    /* The descriptor is reserved before the open, so that a successful open
     * cannot be followed by an EMFILE that leaves the file open and
     * unreachable - which would leak an open-file slot per attempt and is
     * exactly the kind of thing a ring-3 loop finds. */
    struct task_files *files = current_files();
    i32 fd = fd_lowest_free(files);

    if (fd < 0)
        return fd;

    i32 err = 0;
    struct vfs_file *f = vfs_open(abs, flags, &err);

    if (!f)
        return err ? err : SYS_ENOENT;

    files->fd[fd] = f;
    return fd;
}

i32 fd_close(i32 fd)
{
    struct task_files *files = current_files();

    if (fd < 0 || fd >= (i32)SYS_FDS_MAX || !files->fd[fd])
        return SYS_EBADF;

    struct vfs_file *f = files->fd[fd];

    files->fd[fd] = NULL;
    vfs_put(f);

    return 0;
}

i32 fd_read(i32 fd, void *buf, u32 len)
{
    return vfs_read(fd_get(fd), buf, len);
}

i32 fd_write(i32 fd, const void *buf, u32 len)
{
    return vfs_write(fd_get(fd), buf, len);
}

i32 fd_seek(i32 fd, i32 offset, int whence)
{
    return vfs_seek(fd_get(fd), offset, whence);
}

i32 fd_stat(i32 fd, struct sys_stat *out)
{
    return vfs_stat_file(fd_get(fd), out);
}

i32 fd_getdent(i32 fd, u32 index, struct sys_dirent *out)
{
    return vfs_getdent(fd_get(fd), index, out);
}

i32 fd_ioctl(i32 fd, u32 request, u32 arg)
{
    return vfs_ioctl(fd_get(fd), request, arg);
}

i32 fd_dup(i32 fd)
{
    struct vfs_file *f = fd_get(fd);

    if (!f)
        return SYS_EBADF;

    i32 newfd = fd_lowest_free(current_files());

    if (newfd < 0)
        return newfd;

    vfs_ref(f);
    current_files()->fd[newfd] = f;

    return newfd;
}

i32 fd_dup2(i32 oldfd, i32 newfd)
{
    struct task_files *files = current_files();
    struct vfs_file *f = fd_get(oldfd);

    if (!f)
        return SYS_EBADF;
    if (newfd < 0 || newfd >= (i32)SYS_FDS_MAX)
        return SYS_EBADF;

    /* dup2(fd, fd) is a no-op, and specifically must not close and reopen:
     * the close would drop the last reference if there were only one, and
     * then the "copy" would be of a freed slot. */
    if (oldfd == newfd)
        return newfd;

    /* The reference is taken before the old occupant is dropped, for the same
     * reason: if they were the same open file, closing first could free it. */
    vfs_ref(f);

    if (files->fd[newfd]) {
        struct vfs_file *old = files->fd[newfd];

        files->fd[newfd] = NULL;
        vfs_put(old);
    }

    files->fd[newfd] = f;
    return newfd;
}

i32 fd_chdir(const char *path)
{
    char abs[VFS_PATH_MAX];
    struct sys_stat st;

    if (!fd_resolve(path, abs, sizeof(abs)))
        return SYS_ENAMETOOLONG;

    i32 e = vfs_stat(abs, &st);

    if (e < 0)
        return e;

    if (st.type != S_TYPE_DIR)
        return SYS_ENOTDIR;

    strlcpy(current_files()->cwd, abs, sizeof(current_files()->cwd));
    return 0;
}

u32 vfs_open_count(void)
{
    return open_now;
}

u32 vfs_open_peak(void)
{
    return open_peak;
}

u32 vfs_call_count(void)
{
    return call_count;
}
