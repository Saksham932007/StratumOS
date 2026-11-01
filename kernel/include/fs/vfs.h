/* StratumOS - the virtual filesystem, and the descriptor table over it.
 *
 * WHY THIS LAYER EXISTS
 *
 * Until this file, the kernel had a filesystem but no way for a ring-3
 * program to reach it, and a network stack with the same problem. Both gaps
 * had one cause: there was no `open`, so there was nothing a descriptor could
 * be, so `read` and `write` had nothing to name. Adding sockets without this
 * would have meant a second, parallel set of calls that happened to return
 * small integers - which is to say a descriptor table, built twice, badly.
 *
 * So this is one indirection with three implementations behind it from the
 * day it lands. That matters more than it sounds: an interface with a single
 * implementation is a guess about what varies. These three disagree about
 * nearly everything - a FAT file is seekable and has a size, the console is
 * neither, a socket is a stream with no position at all - and the places
 * where `struct vfs_ops` has a NULL entry are exactly the places where the
 * abstraction would have been wrong if it had been designed against files
 * alone.
 *
 * THE TWO OBJECTS, AND WHY THEY ARE TWO
 *
 * A *descriptor* is a small integer, private to a process, that indexes its
 * own table. An *open file* is the kernel object it points at: the position,
 * the access mode, the backing store. They are separate because they have
 * different lifetimes and different sharing rules, and every interesting
 * property of a descriptor table comes from that split:
 *
 *   - dup(fd) makes a second descriptor onto the *same* open file, so a seek
 *     through one is visible through the other. That is what makes `dup2` a
 *     redirection primitive rather than a copy.
 *   - fork() duplicates the table, not the open files, so parent and child
 *     share each position. A shell relies on this: two processes appending to
 *     one descriptor must not overwrite each other.
 *   - close() drops one reference. The open file goes away when the last
 *     descriptor onto it does, which may be in a different process than the
 *     one that opened it.
 *
 * Collapsing them into one object makes all three of those wrong in ways
 * that only show up under concurrency, so they are two.
 *
 * BOUNDS
 *
 * The open-file table is a fixed global array, and each process's descriptor
 * table is a fixed member of `struct task`. Neither grows. A ring-3 program
 * can call open() in a loop; with a heap-backed table that is an allocator
 * attack, and with these it is an EMFILE. The kernel already made this
 * choice for tasks (TASK_MAX) and block devices (BLOCKDEV_MAX); descriptors
 * are the same kind of resource and get the same treatment.
 */
#ifndef _FS_VFS_H
#define _FS_VFS_H

#include <kernel/syscall_abi.h>
#include <kernel/types.h>

/* How many open files the whole kernel can hold at once. Larger than
 * SYS_FDS_MAX because several processes each hold descriptors, and smaller
 * than SYS_FDS_MAX * TASK_MAX because they share them - a global ceiling is
 * the point. */
#define VFS_OPEN_MAX 96

#define VFS_PATH_MAX SYS_PATH_MAX

struct vfs_file;

/* What a backend has to provide.
 *
 * A NULL entry is not an oversight, it is the answer: `seek` is NULL on a
 * socket because a socket has no position, and vfs_seek() returns SYS_EINVAL
 * rather than each backend having to write that rejection out. The dispatcher
 * checks for NULL once so that three backends do not each have to.
 */
struct vfs_ops {
    const char *name;

    /* Short reads are legal and mean "less was available", not an error.
     * A return of 0 means end of file for a file, and for a stream means the
     * other end is closed - the two situations a reader has to tell apart,
     * and the one place where "0 is not an error" is load-bearing. */
    i32 (*read)(struct vfs_file *f, void *buf, u32 len);
    i32 (*write)(struct vfs_file *f, const void *buf, u32 len);
    i32 (*seek)(struct vfs_file *f, i32 offset, int whence);
    i32 (*stat)(struct vfs_file *f, struct sys_stat *out);
    i32 (*getdent)(struct vfs_file *f, u32 index, struct sys_dirent *out);
    i32 (*ioctl)(struct vfs_file *f, u32 request, u32 arg);
    i32 (*truncate)(struct vfs_file *f, u32 length);

    /* Called when the last descriptor onto this open file is closed. The
     * backend releases whatever `priv` points at; the VFS returns the slot. */
    void (*close)(struct vfs_file *f);

    /* Would a read or a write proceed without blocking? Used by the blocking
     * read path and, later, by anything that waits on several descriptors.
     * NULL means "always ready", which is true of a regular file. */
    bool (*poll_read)(struct vfs_file *f);
    bool (*poll_write)(struct vfs_file *f);
};

struct vfs_file {
    const struct vfs_ops *ops;
    u32 refcount; /* descriptors pointing here, across all processes */
    u8 type;      /* S_TYPE_*                                        */
    u32 flags;    /* the O_* the caller opened it with                */
    u32 offset;   /* shared by every descriptor onto this file        */
    void *priv;   /* the backend's own handle                         */
    char path[VFS_PATH_MAX];
};

/* ---- a filesystem, as the VFS sees one --------------------------------
 *
 * Mounting is by path prefix, which is the smallest thing that is honestly a
 * namespace: "/dev/" goes to the device filesystem and everything else to
 * FAT16. It is not a general mount table and does not pretend to be - there
 * is no unmount, and a prefix cannot shadow a longer one. What it does buy is
 * two real filesystems behind one interface from the start, which is the only
 * way to find out whether the interface was right.
 */
struct vfs_fs {
    const char *prefix; /* "/dev/", or "/" for the fallback */
    const char *name;

    i32 (*open)(const char *path, u32 flags, struct vfs_file *f);
    i32 (*stat)(const char *path, struct sys_stat *out);
    i32 (*unlink)(const char *path);
    i32 (*mkdir)(const char *path);
    i32 (*rmdir)(const char *path);
    i32 (*rename)(const char *from, const char *to);
    i32 (*sync)(void);
};

void vfs_init(void);

/* Register a filesystem. Longest prefix wins at lookup time, so the order of
 * registration does not matter - which is deliberate, because an interface
 * whose behaviour depends on initialisation order is a bug waiting for a new
 * subsystem to be added. */
bool vfs_register(const struct vfs_fs *fs);

/* ---- the open-file layer ----------------------------------------------
 *
 * These deal in `struct vfs_file *`. The descriptor layer below is what ring
 * 3 sees; kernel code that wants a file without putting it in a process's
 * table (the ELF loader, the shell) uses these directly.
 */
struct vfs_file *vfs_open(const char *path, u32 flags, i32 *err);
/* Hand a backend-constructed open file to the VFS, for things that are not
 * reached by path: an accepted socket, a pipe end. Takes the first reference. */
struct vfs_file *vfs_alloc(const struct vfs_ops *ops, u8 type, u32 flags,
                           void *priv, const char *name);
void vfs_ref(struct vfs_file *f);
void vfs_put(struct vfs_file *f);

i32 vfs_read(struct vfs_file *f, void *buf, u32 len);
i32 vfs_write(struct vfs_file *f, const void *buf, u32 len);
i32 vfs_seek(struct vfs_file *f, i32 offset, int whence);
i32 vfs_stat_file(struct vfs_file *f, struct sys_stat *out);
i32 vfs_getdent(struct vfs_file *f, u32 index, struct sys_dirent *out);
i32 vfs_ioctl(struct vfs_file *f, u32 request, u32 arg);
i32 vfs_truncate_file(struct vfs_file *f, u32 length);
bool vfs_poll_read(struct vfs_file *f);
bool vfs_poll_write(struct vfs_file *f);

/* ---- by path ---------------------------------------------------------- */

i32 vfs_stat(const char *path, struct sys_stat *out);
i32 vfs_unlink(const char *path);
i32 vfs_mkdir(const char *path);
i32 vfs_rmdir(const char *path);
i32 vfs_rename(const char *from, const char *to);
i32 vfs_sync(void);

/* Resolve `path` against `cwd` into `out`, collapsing "." and ".." and
 * rejecting anything that would escape the root. Exposed because exec() and
 * the shell both need the same answer the open path gets. */
bool vfs_resolve(const char *cwd, const char *path, char *out, size_t outlen);

/* ---- a process's descriptor table -------------------------------------
 *
 * Embedded in `struct task` by value rather than pointed at, so a task's
 * descriptors have exactly the task's lifetime and there is no second
 * allocation to fail halfway through a fork. It is defined here rather than
 * in sched.h because the VFS owns its contents; sched.h includes this file.
 *
 * The working directory lives here too, for the same reason: it is per
 * process, it is inherited by fork, it survives exec, and every path a
 * descriptor call resolves is resolved against it.
 */
struct task_files {
    struct vfs_file *fd[SYS_FDS_MAX];
    char cwd[VFS_PATH_MAX];
};

/* ---- the descriptor layer, as ring 3 sees it --------------------------
 *
 * Every one of these takes a descriptor and acts on the calling task's own
 * table. They are the kernel-side implementation of the system calls, split
 * out so that the syscall dispatcher does argument validation and nothing
 * else.
 */
i32 fd_open(const char *path, u32 flags);
i32 fd_close(i32 fd);
i32 fd_read(i32 fd, void *buf, u32 len);
i32 fd_write(i32 fd, const void *buf, u32 len);
i32 fd_seek(i32 fd, i32 offset, int whence);
i32 fd_stat(i32 fd, struct sys_stat *out);
i32 fd_getdent(i32 fd, u32 index, struct sys_dirent *out);
i32 fd_ioctl(i32 fd, u32 request, u32 arg);
i32 fd_dup(i32 fd);
i32 fd_dup2(i32 oldfd, i32 newfd);
i32 fd_chdir(const char *path);
/* The calling task's working directory. Always absolute, always begins with
 * '/', and is "/" for a kernel thread that never set one. */
const char *fd_cwd(void);
/* Resolve a ring-3 path against the calling task's cwd. The one entry point
 * the syscall layer uses, so that no call can forget the cwd. */
bool fd_resolve(const char *path, char *out, size_t outlen);

/* Install an already-open file at the lowest free descriptor. Used by
 * accept() and pipe(), which produce open files rather than paths. Consumes
 * the reference it is given on success; on failure the caller still owns it. */
i32 fd_install(struct vfs_file *f);

/* The open file a descriptor refers to, or NULL. Does not take a reference:
 * the caller holds the descriptor, and a descriptor is a reference. */
struct vfs_file *fd_get(i32 fd);

/* ---- per-process table management ------------------------------------
 *
 * Called from the process lifecycle, not from system calls.
 */
void fd_table_init(struct task_files *files);
/* fork(): the child's table points at the parent's open files. */
void fd_table_clone(struct task_files *dst, const struct task_files *src);
/* exit(): drop every descriptor this process holds. */
void fd_table_close_all(struct task_files *files);
/* Open 0, 1 and 2 on the console for a process that is about to start. */
bool fd_table_open_std(struct task_files *files);

u32 vfs_open_count(void);
u32 vfs_open_peak(void);
u32 vfs_call_count(void);

/* ---- the backends ------------------------------------------------------
 *
 * Each registers itself with the VFS and is otherwise private. Declared here
 * rather than in a header apiece because there is nothing else to say about
 * them: the interface they implement is the one above.
 */
void devfs_init(void);
void devfs_counters(u32 *reads, u32 *bytes_in, u32 *bytes_out);

/* The FAT16 backend. Registering it is separate from mounting the filesystem,
 * because a kernel booted from the GRUB ISO has no partition to mount and
 * still has to have a working /dev. */
void fatfs_init(void);
void fatfs_counters(u32 *opens, u32 *reads, u32 *bytes_read);
void fatfs_write_counters(u32 *writes, u32 *bytes_written);

#endif /* _FS_VFS_H */
