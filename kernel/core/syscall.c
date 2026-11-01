/* StratumOS - the system call boundary.
 *
 * `int 0x80` with the call number in EAX and arguments in EBX/ECX/EDX. The
 * gate is installed with DPL 3 so ring 3 is allowed to invoke it - every other
 * vector stays DPL 0, so userspace cannot, for example, fake a page fault.
 *
 * The rule this file exists to enforce: a pointer that arrived from ring 3 is
 * an attacker-controlled integer until proven otherwise. user_range_ok() walks
 * the page tables for every page the buffer touches and insists on
 * present+user before the kernel dereferences anything. Skipping that check
 * is how a `write()` syscall becomes an arbitrary kernel-memory read.
 */
#define LOG_TAG "syscall"

#include <arch/gdt.h>
#include <arch/harden.h>
#include <arch/idt.h>

#include <drivers/keyboard.h>
#include <drivers/timer.h>

#include <kernel/console.h>
#include <kernel/layout.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/usermode.h>

#include <mm/vmm.h>

#include <fs/vfs.h>

/* The largest single read or write a system call will move. Not a limit on
 * what a program can transfer - it loops - but a limit on how much kernel
 * stack one call can be made to use, and on how long interrupts can be held
 * off inside one copy. */
#define SYS_IO_MAX 1024

static u32 call_count;

/* One walk, two questions. `need_write` is what separates "the kernel may
 * read this" from "the kernel may store to this", and the two are genuinely
 * different sets of pages - see user_range_writable() in syscall.h for the
 * panic that proved it. */
static bool range_ok(vaddr_t base, size_t len, bool need_write)
{
    if (len == 0)
        return true;

    /* Reject anything that wraps the address space: base+len overflowing is
     * the classic way to make a bounds check pass for a range that does not
     * exist. */
    if (base + len < base)
        return false;

    /* The kernel owns everything at and above KERNEL_VIRT_BASE. With the
     * kernel in the higher half this is the whole boundary check - no need to
     * enumerate individual kernel windows, which is exactly the simplification
     * the relocation bought. */
    if (is_kernel_address(base) || is_kernel_address(base + len - 1))
        return false;

    vaddr_t first = PAGE_TRUNC(base);
    vaddr_t last = PAGE_TRUNC(base + len - 1);

    for (vaddr_t page = first; page <= last; page += PAGE_SIZE) {
        u32 pte = vmm_pte(page);

        if (!(pte & PTE_PRESENT) || !(pte & PTE_USER))
            return false;

        /* A copy-on-write page counts as writable even though its PTE says
         * otherwise. That is the whole mechanism: it is mapped read-only so
         * that the *first* write traps, and cow_fault() then breaks the
         * sharing and resumes the faulting instruction. cow_fault() does not
         * care which ring the write came from, so a kernel store through such
         * a pointer resolves exactly as a userspace one would.
         *
         * Rejecting these instead would be the subtler bug of the two: every
         * page of a freshly forked process is COW, so a child calling
         * read() into its own buffer before writing to it would get EFAULT
         * for a buffer that is plainly its own. */
        if (need_write && !(pte & PTE_WRITE) && !(pte & PTE_COW))
            return false;

        if (page + PAGE_SIZE < page)
            break; /* reached the top of the address space */
    }

    return true;
}

bool user_range_ok(vaddr_t base, size_t len)
{
    return range_ok(base, len, false);
}

bool user_range_writable(vaddr_t base, size_t len)
{
    return range_ok(base, len, true);
}

static u32 current_pid(void)
{
    return task_current() ? task_current()->pid : 0;
}

/* Deliberately a switch rather than a table indexed by -err. A table would be
 * one line shorter and would go out of step with the enum the first time a
 * code was inserted in the middle; the switch cannot, because the compiler
 * resolves each name. */
const char *sys_strerror(i32 err)
{
    switch (err) {
    case 0:
        return "ok";
    case SYS_EBADCALL:
        return "no such system call";
    case SYS_EFAULT:
        return "bad address";
    case SYS_EINVAL:
        return "invalid argument";
    case SYS_ENOENT:
        return "no such file or directory";
    case SYS_EBADF:
        return "bad file descriptor";
    case SYS_EMFILE:
        return "too many open files in this process";
    case SYS_ENFILE:
        return "too many open files in the system";
    case SYS_EISDIR:
        return "is a directory";
    case SYS_ENOTDIR:
        return "not a directory";
    case SYS_ENOSPC:
        return "no space left on device";
    case SYS_EROFS:
        return "read-only filesystem";
    case SYS_EEXIST:
        return "already exists";
    case SYS_ENOTEMPTY:
        return "directory not empty";
    case SYS_EIO:
        return "input/output error";
    case SYS_EAGAIN:
        return "would block";
    case SYS_ENOTSUP:
        return "not supported";
    case SYS_ENOTCONN:
        return "not connected";
    case SYS_ECONNREFUSED:
        return "connection refused";
    case SYS_ETIMEDOUT:
        return "timed out";
    case SYS_EADDRINUSE:
        return "address already in use";
    case SYS_EPIPE:
        return "broken pipe";
    case SYS_ENAMETOOLONG:
        return "name too long";
    case SYS_ECONNRESET:
        return "connection reset by peer";
    case SYS_EISCONN:
        return "already connected";
    case SYS_EHOSTUNREACH:
        return "host unreachable";
    default:
        return err < 0 ? "unknown error" : "ok";
    }
}

/* ---- moving bytes across the boundary ----------------------------------
 *
 * Every transfer goes through a kernel bounce buffer, and that is a change
 * from how write() used to work. The old version validated the user pointer
 * and handed it straight to console_write() inside a SMAP window, which was
 * safe because console_write() does nothing but copy bytes out to a port.
 *
 * A VFS backend is not that. It can take a lock, issue a disk read, or sleep;
 * with a user pointer in hand it would be touching user memory at an
 * arbitrary later point, under an open SMAP window, in code that has no idea
 * it is holding one. Worse, the pages could be unmapped in between - another
 * thread in the same address space can munmap them, and on this kernel a
 * copy-on-write fault can replace the frame underneath.
 *
 * So the window is here, it is as short as a memcpy, and no backend ever sees
 * a ring-3 address. The cost is one copy; the thing bought is that the rule
 * "a backend never touches user memory" needs no backend to cooperate.
 */
static i32 copy_from_user(void *dst, u32 uaddr, u32 len)
{
    if (len == 0)
        return 0;

    if (!user_range_ok(uaddr, len)) {
        pr_warn_ratelimited("pid %u passed an unreadable buffer %p+%u",
                            current_pid(), (void *)uaddr, len);
        return SYS_EFAULT;
    }

    user_access_begin();
    memcpy(dst, (const void *)uaddr, len);
    user_access_end();

    return (i32)len;
}

static i32 copy_to_user(u32 uaddr, const void *src, u32 len)
{
    if (len == 0)
        return 0;

    if (!user_range_writable(uaddr, len)) {
        pr_warn_ratelimited("pid %u passed an unwritable buffer %p+%u",
                            current_pid(), (void *)uaddr, len);
        return SYS_EFAULT;
    }

    user_access_begin();
    memcpy((void *)uaddr, src, len);
    user_access_end();

    return (i32)len;
}

/* Copy a NUL-terminated string from user space into a kernel buffer.
 *
 * Validated one byte at a time rather than by validating `max` bytes up
 * front: a string that ends two bytes into the last mapped page is perfectly
 * legal, and insisting that the whole buffer be readable would reject it.
 * Walking it instead means the kernel never reads past what the caller
 * actually mapped, which is the property that matters. Unterminated input
 * fails rather than being silently truncated - a truncated program name is a
 * different program name. */
static bool user_copy_string(u32 ptr, char *dst, size_t max)
{
    for (size_t i = 0; i < max; i++) {
        if (!user_range_ok(ptr + i, 1)) {
            pr_warn_ratelimited("pid %u passed an unreadable string pointer %p",
                                task_current() ? task_current()->pid : 0,
                                (void *)ptr);
            return false;
        }

        user_access_begin();
        dst[i] = *(const char *)(ptr + i);
        user_access_end();

        if (dst[i] == '\0')
            return true;
    }

    pr_warn_ratelimited("pid %u passed a string longer than %u bytes",
                        task_current() ? task_current()->pid : 0,
                        (unsigned)max);
    return false;
}

/* ---- the descriptor calls ---------------------------------------------
 *
 * Each of these does exactly two things: validate what arrived from ring 3,
 * and call the one fd_* function that implements it. No policy lives here -
 * whether a descriptor may be written to, whether a path exists, whether a
 * directory can be read as bytes, are all decided in fs/vfs.c, where there is
 * one copy of each decision instead of one per system call.
 */

static i32 sys_read_fd(u32 fd, u32 ptr, u32 len)
{
    char buf[SYS_IO_MAX];

    if (len > sizeof(buf))
        len = sizeof(buf);

    if (len == 0)
        return 0;

    /* The destination is validated before the read, not after. A read that
     * succeeded and then could not be delivered would have consumed bytes
     * from a stream that cannot give them back - the data would simply be
     * gone. Checking first turns that into a clean EFAULT. */
    if (!user_range_writable(ptr, len)) {
        pr_warn_ratelimited("pid %u passed an unwritable buffer %p+%u to read",
                            current_pid(), (void *)ptr, len);
        return SYS_EFAULT;
    }

    i32 got = fd_read((i32)fd, buf, len);

    if (got <= 0)
        return got;

    i32 e = copy_to_user(ptr, buf, (u32)got);

    if (e < 0)
        return e;

    return got;
}

static i32 sys_write_fd(u32 fd, u32 ptr, u32 len)
{
    char buf[SYS_IO_MAX];

    if (len > sizeof(buf))
        len = sizeof(buf);

    if (len == 0)
        return 0;

    i32 e = copy_from_user(buf, ptr, len);

    if (e < 0)
        return e;

    return fd_write((i32)fd, buf, len);
}

/* Copy a path out of user memory and resolve it against the caller's working
 * directory. One function, used by every call that takes a path, so that no
 * call can accidentally pass a relative path or an unresolved one to a
 * backend. */
static i32 user_path(u32 ptr, char *out, size_t outlen)
{
    char raw[SYS_PATH_MAX];

    if (!user_copy_string(ptr, raw, sizeof(raw)))
        return SYS_EFAULT;

    if (!fd_resolve(raw, out, outlen))
        return SYS_ENAMETOOLONG;

    return 0;
}

static i32 sys_stat_path(u32 pathptr, u32 outptr)
{
    char path[SYS_PATH_MAX];
    struct sys_stat st;
    i32 e = user_path(pathptr, path, sizeof(path));

    if (e < 0)
        return e;

    e = vfs_stat(path, &st);

    if (e < 0)
        return e;

    return copy_to_user(outptr, &st, sizeof(st)) < 0 ? SYS_EFAULT : 0;
}

static i32 sys_fstat_fd(u32 fd, u32 outptr)
{
    struct sys_stat st;
    i32 e = fd_stat((i32)fd, &st);

    if (e < 0)
        return e;

    return copy_to_user(outptr, &st, sizeof(st)) < 0 ? SYS_EFAULT : 0;
}

static i32 sys_getdent_fd(u32 fd, u32 index, u32 outptr)
{
    struct sys_dirent de;
    i32 e = fd_getdent((i32)fd, index, &de);

    /* 0 means "no entry at that index", which is the end of the directory and
     * not an error. The caller sees 0 and stops; nothing is copied out,
     * because there is nothing to copy. */
    if (e <= 0)
        return e;

    return copy_to_user(outptr, &de, sizeof(de)) < 0 ? SYS_EFAULT : 1;
}

static i32 sys_getcwd(u32 ptr, u32 len)
{
    const char *cwd = fd_cwd();
    size_t n = strlen(cwd) + 1;

    if (len < n)
        return SYS_EINVAL;

    return copy_to_user(ptr, cwd, (u32)n) < 0 ? SYS_EFAULT : (i32)(n - 1);
}

/* A path-taking call with no arguments beyond the path. */
static i32 sys_path_op(u32 ptr, i32 (*op)(const char *))
{
    char path[SYS_PATH_MAX];
    i32 e = user_path(ptr, path, sizeof(path));

    if (e < 0)
        return e;

    return op(path);
}

static void syscall_handler(struct regs *r)
{
    i32 ret;

    call_count++;

    switch (r->eax) {
    case SYS_EXIT:
        /* task_exit() does not return; the dying task's register frame is
         * simply never restored. */
        task_exit((int)r->ebx);
        return;

    /* write(fd, buf, len). The signature changed when descriptors arrived:
     * it used to be write(buf, len), with the console as the only possible
     * destination. Keeping the call number and changing the arguments is the
     * right trade here - there is no binary compatibility to preserve (see
     * docs/ROADMAP.md, "Not planned"), and a second call number for "write
     * to a descriptor" would have left two writes forever. */
    case SYS_WRITE:
        ret = sys_write_fd(r->ebx, r->ecx, r->edx);
        break;

    case SYS_READ:
        ret = sys_read_fd(r->ebx, r->ecx, r->edx);
        break;

    case SYS_OPEN: {
        char path[SYS_PATH_MAX];

        ret = user_path(r->ebx, path, sizeof(path));

        if (ret < 0)
            break;

        ret = fd_open(path, r->ecx);
        break;
    }

    case SYS_CLOSE:
        ret = fd_close((i32)r->ebx);
        break;

    case SYS_LSEEK:
        ret = fd_seek((i32)r->ebx, (i32)r->ecx, (int)r->edx);
        break;

    case SYS_STAT:
        ret = sys_stat_path(r->ebx, r->ecx);
        break;

    case SYS_FSTAT:
        ret = sys_fstat_fd(r->ebx, r->ecx);
        break;

    case SYS_GETDENT:
        ret = sys_getdent_fd(r->ebx, r->ecx, r->edx);
        break;

    case SYS_DUP:
        ret = fd_dup((i32)r->ebx);
        break;

    case SYS_DUP2:
        ret = fd_dup2((i32)r->ebx, (i32)r->ecx);
        break;

    case SYS_IOCTL:
        ret = fd_ioctl((i32)r->ebx, r->ecx, r->edx);
        break;

    case SYS_CHDIR:
        ret = sys_path_op(r->ebx, fd_chdir);
        break;

    case SYS_GETCWD:
        ret = sys_getcwd(r->ebx, r->ecx);
        break;

    case SYS_UNLINK:
        ret = sys_path_op(r->ebx, vfs_unlink);
        break;

    case SYS_MKDIR:
        ret = sys_path_op(r->ebx, vfs_mkdir);
        break;

    case SYS_RMDIR:
        ret = sys_path_op(r->ebx, vfs_rmdir);
        break;

    case SYS_TRUNCATE:
        ret = vfs_truncate_file(fd_get((i32)r->ebx), r->ecx);
        break;

    case SYS_RENAME: {
        char from[SYS_PATH_MAX];
        char to[SYS_PATH_MAX];

        ret = user_path(r->ebx, from, sizeof(from));
        if (ret < 0)
            break;

        ret = user_path(r->ecx, to, sizeof(to));
        if (ret < 0)
            break;

        ret = vfs_rename(from, to);
        break;
    }

    case SYS_SYNC:
        ret = vfs_sync();
        break;

    case SYS_GETPID:
        ret = task_current() ? (i32)task_current()->pid : 0;
        break;

    case SYS_YIELD:
        sched_yield();
        ret = 0;
        break;

    case SYS_SLEEP:
        /* Bound the request: a userspace task must not be able to ask for a
         * sleep long enough to look like a hang. */
        task_sleep_ms(MIN(r->ebx, 10000u));
        ret = 0;
        break;

    case SYS_UPTIME:
        ret = (i32)timer_uptime_seconds();
        break;

    case SYS_GETKEY:
        ret = keyboard_poll();
        break;

    case SYS_FORK:
        /* The frame is handed through so the child can be given a copy of it
         * with EAX zeroed - which is how one call returns twice. */
        ret = task_fork(r);
        break;

    case SYS_WAIT: {
        int status = 0;
        int pid = task_wait(&status);

        /* The status pointer is optional, and like every pointer from ring 3
         * it is checked before being written through. */
        if (pid >= 0 && r->ebx) {
            if (!user_range_writable(r->ebx, sizeof(int))) {
                pr_warn_ratelimited(
                    "pid %u passed an unwritable status pointer %p to "
                    "wait()",
                    task_current() ? task_current()->pid : 0, (void *)r->ebx);
                ret = SYS_EFAULT;
                break;
            }
            user_access_begin();
            *(int *)r->ebx = status;
            user_access_end();
        }

        ret = pid;
        break;
    }

    case SYS_GETPPID:
        ret = task_current() ? (i32)task_current()->parent_pid : 0;
        break;

    case SYS_EXEC: {
        char name[SYS_NAME_MAX];

        /* The name has to be copied out of user memory before exec touches
         * the address space, because the string itself lives in the image
         * that is about to be unmapped. Reading it afterwards would be a
         * dereference of a page that no longer exists. */
        if (!user_copy_string(r->ebx, name, sizeof(name))) {
            ret = SYS_EFAULT;
            break;
        }

        if (!usermode_exec(name, r)) {
            pr_warn_ratelimited(
                "pid %u asked to exec \"%s\", which is not a program this "
                "kernel has",
                task_current() ? task_current()->pid : 0, name);
            ret = SYS_ENOENT;
            break;
        }

        /* On success there is nothing to return to: usermode_exec() rewrote
         * the frame, including EAX, so the IRET at the end of the interrupt
         * path lands in the new image's entry point instead. Returning here
         * would overwrite that. */
        return;
    }

    default:
        pr_warn_ratelimited("unknown syscall %u from EIP %p", r->eax,
                            (void *)r->eip);
        ret = SYS_EBADCALL;
        break;
    }

    /* The return value goes back in EAX. `popa` in isr_common reloads EAX
     * from this frame, which is what makes writing to it work. */
    r->eax = (u32)ret;
}

/* Generated by arch/x86/isr.asm. */
extern u32 isr_stub_table[IDT_ENTRIES];

void syscall_init(void)
{
    isr_install_handler(INT_SYSCALL, syscall_handler);

    /* Re-install the vector with DPL 3 so ring 3 may execute `int 0x80`;
     * idt_init() gave every vector DPL 0. It stays an *interrupt* gate rather
     * than a trap gate, so interrupts are masked on entry and the kernel
     * decides when to re-enable them. */
    idt_set_gate(INT_SYSCALL, isr_stub_table[INT_SYSCALL], SEL_KERNEL_CODE,
                 IDT_PRESENT | IDT_RING3 | IDT_GATE_INT32);

    pr_info("syscall gate installed at int 0x%02x (%u calls available)",
            INT_SYSCALL, (unsigned)SYS_MAX);
}

u32 syscall_count(void)
{
    return call_count;
}
