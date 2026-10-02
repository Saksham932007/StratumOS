/* StratumOS - the ring 3 payload.
 *
 * Everything in this file is compiled into the `.user` section and runs at
 * privilege level 3. That imposes real constraints, and they are the point:
 *
 *   * No calls into the kernel. Not kprintf, not memcpy - the kernel's pages
 *     are not user-accessible, so a call would fault immediately.
 *   * No string literals in .rodata. A literal would be emitted into the
 *     kernel's read-only section, which ring 3 cannot read; the strings here
 *     live in .user alongside the code.
 *   * No libc. The only way to affect the world is `int 0x80`.
 *
 * If this code tries to do something it is not allowed to - touch kernel
 * memory, execute a privileged instruction, write to an I/O port - the result
 * is a general protection fault or a page fault with CS showing ring 3, which
 * is exactly the demonstration being aimed at. The `probe` step below does
 * that deliberately and the kernel survives it.
 */
#include <kernel/syscall.h>
#include <kernel/types.h>
#include <kernel/usermode.h>

#define USER_TEXT   __attribute__((section(".user.text"), used))
#define USER_RODATA __attribute__((section(".user.rodata"), used))

/* ---- syscall stubs ------------------------------------------------------ */

static USER_TEXT inline i32 syscall3(u32 nr, u32 a, u32 b, u32 c)
{
    i32 ret;

    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return ret;
}

static USER_TEXT i32 u_write(const char *s, u32 len)
{
    return syscall3(SYS_WRITE, (u32)s, len, 0);
}

static USER_TEXT i32 u_getpid(void)
{
    return syscall3(SYS_GETPID, 0, 0, 0);
}

static USER_TEXT i32 u_uptime(void)
{
    return syscall3(SYS_UPTIME, 0, 0, 0);
}

static USER_TEXT void u_yield(void)
{
    (void)syscall3(SYS_YIELD, 0, 0, 0);
}

static USER_TEXT void u_sleep(u32 ms)
{
    (void)syscall3(SYS_SLEEP, ms, 0, 0);
}

static USER_TEXT NORETURN void u_exit(int code)
{
    (void)syscall3(SYS_EXIT, (u32)code, 0, 0);
    for (;;)
        ; /* unreachable: the kernel never returns from exit */
}

/* ---- tiny local helpers (no libc in ring 3) ---------------------------- */

static USER_TEXT u32 u_strlen(const char *s)
{
    u32 n = 0;
    while (s[n])
        n++;
    return n;
}

static USER_TEXT void u_puts(const char *s)
{
    (void)u_write(s, u_strlen(s));
}

/* Render an unsigned value into `buf` and print it. */
static USER_TEXT void u_putu(u32 v)
{
    char buf[12];
    int i = (int)sizeof(buf);

    buf[--i] = '\0';
    if (v == 0)
        buf[--i] = '0';
    while (v && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }

    (void)u_write(&buf[i], u_strlen(&buf[i]));
}

/* ---- strings, kept inside the user-readable section -------------------- */

static USER_RODATA char msg_hello[] =
    "  [ring3] hello from user mode - privilege level 3\n";
static USER_RODATA char msg_pid[] = "  [ring3] getpid() returned ";
static USER_RODATA char msg_uptime[] = "  [ring3] uptime() returned ";
static USER_RODATA char msg_seconds[] = " s\n";
static USER_RODATA char msg_yield[] = "  [ring3] yielded and was rescheduled\n";
static USER_RODATA char msg_slept[] = "  [ring3] slept 50 ms via syscall\n";
static USER_RODATA char msg_fault[] =
    "  [ring3] asking the kernel to read a kernel address on my behalf\n";
static USER_RODATA char msg_denied[] =
    "  [ring3] kernel refused it (EFAULT) - the pointer check works\n";
static USER_RODATA char msg_allowed[] =
    "  [ring3] WARNING: kernel accepted a kernel pointer from ring 3!\n";
static USER_RODATA char msg_bad_call[] =
    "  [ring3] unknown syscall correctly rejected\n";
static USER_RODATA char msg_bye[] = "  [ring3] calling exit(0)\n";
static USER_RODATA char msg_nl[] = "\n";

/* ---- entry point ------------------------------------------------------- */

/* Declared in kernel/include/kernel/usermode.h; core/usermode.c takes its
 * address to build the ring-3 IRET frame. */
USER_TEXT void user_demo_entry(void)
{
    u_puts(msg_hello);

    u_puts(msg_pid);
    u_putu((u32)u_getpid());
    u_puts(msg_nl);

    u_puts(msg_uptime);
    u_putu((u32)u_uptime());
    u_puts(msg_seconds);

    u_yield();
    u_puts(msg_yield);

    u_sleep(50);
    u_puts(msg_slept);

    /* Hand the kernel a pointer into its own address space and check that it
     * is rejected. This is the security property that matters most about the
     * syscall boundary, so it is verified from the untrusted side. */
    u_puts(msg_fault);
    if (u_write((const char *)0x00100000u, 16) == SYS_EFAULT)
        u_puts(msg_denied);
    else
        u_puts(msg_allowed);

    /* An out-of-range call number must come back as an error, not a jump
     * through an unchecked table. */
    if (syscall3(9999, 0, 0, 0) == SYS_EBADCALL)
        u_puts(msg_bad_call);

    u_puts(msg_bye);
    u_exit(0);
}
