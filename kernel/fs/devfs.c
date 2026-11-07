/* StratumOS - the device filesystem.
 *
 * Four character devices under /dev, and they exist for a reason beyond
 * completeness: they are the second implementation behind `struct vfs_ops`.
 * A VFS validated only against FAT16 would be a FAT16 interface with extra
 * steps. These devices disagree with a file about nearly everything - they
 * have no size, no position, and /dev/console's read blocks - and each
 * disagreement is a place the interface had to be right rather than
 * convenient.
 *
 * /dev/console is the one that matters. Descriptors 0, 1 and 2 are opened on
 * it before a process starts, which is what makes a ring-3 `write(1, ...)`
 * work without the kernel special-casing the number 1 anywhere. Before this
 * file, SYS_WRITE *was* that special case: it took a pointer and a length and
 * went straight to console_write(), so there was exactly one thing a program
 * could write to and no way to name any other. Routing it through a
 * descriptor is what made redirection expressible at all.
 *
 * The blocking read is the interesting half. keyboard_getchar() yields the
 * CPU until a key arrives, so a process reading from the console is
 * descheduled rather than spinning - and because the first byte blocks while
 * the rest drain non-blocking, a read returns a whole burst of typing in one
 * call without waiting for a buffer to fill. That is the behaviour a line
 * editor needs, and it is where O_NONBLOCK earns its place: a caller that
 * cannot afford to wait gets SYS_EAGAIN instead.
 */
#define LOG_TAG "devfs"

#include <drivers/keyboard.h>

#include <kernel/console.h>
#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/string.h>

#include <fs/fslock.h>
#include <fs/vfs.h>

/* Which device an open file is. Stored in ->priv as a small integer rather
 * than a pointer, because there is nothing to point at - a device has no
 * per-open state beyond this. */
enum dev_kind {
    DEV_CONSOLE = 1,
    DEV_NULL,
    DEV_ZERO,
    DEV_KBD,
};

static u32 console_reads;
static u32 console_bytes_in;
static u32 console_bytes_out;
static bool console_echo = true;

static enum dev_kind kind_of(const struct vfs_file *f)
{
    return (enum dev_kind)(uptr)f->priv;
}

/* ---- /dev/console, /dev/kbd ------------------------------------------- */

static i32 console_dev_read(struct vfs_file *f, void *buf, u32 len)
{
    char *out = buf;
    u32 got = 0;

    if (len == 0)
        return 0;

    if (f->flags & O_NONBLOCK) {
        if (!keyboard_has_input())
            return SYS_EAGAIN;
    } else {
        /* Blocks by yielding, so a reader costs nothing while it waits. A
         * key code above the printable range (an arrow, an F-key) is
         * delivered as its raw value; a program that does not understand one
         * sees a byte it can ignore, which is better than the driver
         * deciding to drop it. */
        /* The filesystem lock goes down for the wait. Blocking here with it
         * held would make the whole filesystem unavailable to every other
         * processor until somebody pressed a key - and yielding takes the
         * scheduler's lock, which is below this one and would trip the
         * hierarchy check. Nothing read before the window is relied on
         * after it; the next thing this does is read the keyboard ring,
         * which excludes its interrupt by masking. */
        bool released = fs_sleep_begin();
        int c = keyboard_getchar();

        fs_sleep_end(released);

        if (c < 0)
            return 0;

        out[got++] = (char)c;

        if (console_echo && kind_of(f) == DEV_CONSOLE && c >= 0x20 && c < 0x7F)
            console_putc((char)c);
    }

    /* Drain whatever else is already queued. This is what turns a paste or a
     * fast typist into one read rather than one read per character. */
    while (got < len && keyboard_has_input()) {
        int c = keyboard_poll();

        if (c < 0)
            break;

        out[got++] = (char)c;

        if (console_echo && kind_of(f) == DEV_CONSOLE && c >= 0x20 && c < 0x7F)
            console_putc((char)c);
    }

    console_reads++;
    console_bytes_in += got;

    return (i32)got;
}

static i32 console_dev_write(struct vfs_file *f, const void *buf, u32 len)
{
    UNUSED(f);

    console_write(buf, len);
    console_bytes_out += len;

    return (i32)len;
}

static bool console_dev_poll_read(struct vfs_file *f)
{
    UNUSED(f);
    return keyboard_has_input();
}

static i32 console_dev_ioctl(struct vfs_file *f, u32 request, u32 arg)
{
    UNUSED(f);

    switch (request) {
    case TIOCNREAD:
        return keyboard_has_input() ? 1 : 0;

    case TIOCSECHO:
        console_echo = arg != 0;
        return 0;

    default:
        return SYS_EINVAL;
    }
}

static i32 dev_stat(struct vfs_file *f, struct sys_stat *out)
{
    UNUSED(f);

    /* A character device has no size, and reporting one would be a lie a
     * caller could act on - a program that stats before reading would decide
     * there was nothing there. Zero, explicitly. */
    out->size = 0;
    out->blocks = 0;

    return 0;
}

static const struct vfs_ops console_ops = {
    .name = "console",
    .read = console_dev_read,
    .write = console_dev_write,
    .stat = dev_stat,
    .ioctl = console_dev_ioctl,
    .poll_read = console_dev_poll_read,
};

/* ---- /dev/null, /dev/zero --------------------------------------------- */

static i32 null_read(struct vfs_file *f, void *buf, u32 len)
{
    UNUSED(f);
    UNUSED(buf);
    UNUSED(len);

    /* End of file, immediately and forever. */
    return 0;
}

static i32 null_write(struct vfs_file *f, const void *buf, u32 len)
{
    UNUSED(f);
    UNUSED(buf);

    /* Accepts everything and keeps none of it, which is the whole contract. */
    return (i32)len;
}

static i32 zero_read(struct vfs_file *f, void *buf, u32 len)
{
    UNUSED(f);

    memset(buf, 0, len);
    return (i32)len;
}

static const struct vfs_ops null_ops = {
    .name = "null",
    .read = null_read,
    .write = null_write,
    .stat = dev_stat,
};

static const struct vfs_ops zero_ops = {
    .name = "zero",
    .read = zero_read,
    .write = null_write,
    .stat = dev_stat,
};

/* ---- /dev itself ------------------------------------------------------
 *
 * A directory with no backing store: its contents are the `devices` table
 * below, so getdent() reads out of an array rather than off a disk. That is
 * the whole of what a synthetic filesystem is, and having one here means the
 * VFS's directory interface was exercised by something that is not FAT16
 * before anything depended on it.
 */
static i32 devdir_getdent(struct vfs_file *f, u32 index,
                          struct sys_dirent *out);

static i32 devdir_stat(struct vfs_file *f, struct sys_stat *out)
{
    UNUSED(f);

    out->type = S_TYPE_DIR;
    out->size = 0;
    return 0;
}

static const struct vfs_ops devdir_ops = {
    .name = "devdir",
    .stat = devdir_stat,
    .getdent = devdir_getdent,
};

/* ---- the filesystem --------------------------------------------------- */

static const struct {
    const char *name;
    const struct vfs_ops *ops;
    enum dev_kind kind;
} devices[] = {
    {"CONSOLE", &console_ops, DEV_CONSOLE}, {"TTY", &console_ops, DEV_CONSOLE},
    {"KBD", &console_ops, DEV_KBD},         {"NULL", &null_ops, DEV_NULL},
    {"ZERO", &zero_ops, DEV_ZERO},
};

/* The component after "/DEV/", or NULL for the directory itself. Paths
 * arriving here have already been through vfs_resolve(), so they are
 * absolute, canonical and upper-case - this does not have to defend against
 * "/dev/../etc" or a trailing slash. */
static const char *dev_name_of(const char *path)
{
    const char *n = path + 4; /* past "/DEV" */

    if (*n == '/')
        n++;

    return *n ? n : NULL;
}

static i32 devfs_open(const char *path, u32 flags, struct vfs_file *f)
{
    const char *name = dev_name_of(path);

    if (!name) {
        /* "/dev" itself. Opening a directory is legal and is how a program
         * lists it; what it cannot do is read bytes from it, which the VFS
         * rejects on the type rather than here. */
        if ((flags & O_ACCMODE) != O_RDONLY)
            return SYS_EISDIR;

        f->ops = &devdir_ops;
        f->type = S_TYPE_DIR;
        f->priv = NULL;
        return 0;
    }

    for (size_t i = 0; i < ARRAY_SIZE(devices); i++) {
        if (strcmp(name, devices[i].name) != 0)
            continue;

        /* O_CREAT on a device is not an error and not a creation: the device
         * is already there. O_TRUNC is ignored for the same reason - there is
         * nothing to truncate. Silently accepting both is what lets a program
         * open "/dev/null" with the same flags it would use for a log file. */
        f->ops = devices[i].ops;
        f->type = S_TYPE_CHAR;
        f->priv = (void *)(uptr)devices[i].kind;
        f->offset = 0;

        return 0;
    }

    return SYS_ENOENT;
}

static i32 devfs_stat(const char *path, struct sys_stat *out)
{
    const char *name = dev_name_of(path);

    if (!name) {
        out->type = S_TYPE_DIR;
        out->mode = S_MODE_READ;
        return 0;
    }

    for (size_t i = 0; i < ARRAY_SIZE(devices); i++) {
        if (strcmp(name, devices[i].name) != 0)
            continue;

        out->type = S_TYPE_CHAR;
        out->mode = S_MODE_READ | S_MODE_WRITE;
        out->size = 0;
        return 0;
    }

    return SYS_ENOENT;
}

static i32 devdir_getdent(struct vfs_file *f, u32 index, struct sys_dirent *out)
{
    UNUSED(f);

    if (index >= ARRAY_SIZE(devices))
        return 0; /* end of directory */

    strlcpy(out->name, devices[index].name, sizeof(out->name));
    out->type = S_TYPE_CHAR;
    out->size = 0;

    return 1;
}

/* No unlink, mkdir, rmdir or rename. The VFS turns a missing operation into
 * SYS_EROFS, which is the truthful answer: these devices are compiled in, so
 * the namespace genuinely is read-only. */
static const struct vfs_fs devfs = {
    .prefix = "/DEV/",
    .name = "devfs",
    .open = devfs_open,
    .stat = devfs_stat,
};

/* A second registration for the bare directory, because the prefix match
 * needs the trailing slash to avoid claiming "/DEVICES" - and "/DEV" with no
 * slash still has to resolve to something. */
static const struct vfs_fs devfs_root = {
    .prefix = "/DEV",
    .name = "devfs",
    .open = devfs_open,
    .stat = devfs_stat,
};

void devfs_init(void)
{
    vfs_register(&devfs);
    vfs_register(&devfs_root);

    pr_info("%u character devices under /dev", (unsigned)ARRAY_SIZE(devices));
}

void devfs_counters(u32 *reads, u32 *in, u32 *out)
{
    if (reads)
        *reads = console_reads;
    if (in)
        *in = console_bytes_in;
    if (out)
        *out = console_bytes_out;
}
