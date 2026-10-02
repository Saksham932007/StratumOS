/* StratumOS - the ring-3 demonstration program.
 *
 * This is a genuinely separate program: its own ELF, linked for user space at
 * USER_IMAGE_BASE, loaded by the kernel's ELF loader into user-accessible
 * pages. It is not part of the kernel image's address space, which is what
 * makes it a real test of the privilege boundary rather than a kernel function
 * with the USER bit flipped.
 *
 * It has no libc, no kernel headers beyond the syscall ABI, and no way to
 * affect the world except `int 0x80`. If it tries anything it is not allowed
 * to - touching kernel memory, executing a privileged instruction, talking to
 * a port - the result is a fault with CS showing ring 3, which is exactly the
 * demonstration being aimed at. The checks below do that deliberately, and
 * the kernel survives them.
 */
#include "syscall.h"

/* The ELF entry point. Declared so that -Wmissing-prototypes is satisfied;
 * nothing calls it, the CPU jumps here after the kernel's IRET to ring 3. */
void _start(void);

/* Verify from the untrusted side that the kernel refuses a pointer into its
 * own address space. This is the security property that matters most about
 * the syscall boundary, so it is worth testing from the attacker's seat. */
static void probe_kernel_pointer(void)
{
    u_puts("  [ring3] asking the kernel to read a kernel address on my "
           "behalf\n");

    /* 0xC0100000 is where the kernel is linked. A correct kernel rejects it. */
    if (sys_write((const char *)0xC0100000u, 16) == SYS_EFAULT)
        u_puts("  [ring3] kernel refused it (EFAULT) - the pointer check "
               "works\n");
    else
        u_puts("  [ring3] WARNING: kernel accepted a kernel pointer from "
               "ring 3!\n");
}

static void probe_unmapped_pointer(void)
{
    /* An address in our own half that nothing has mapped. The kernel must
     * notice that it is not present, not merely that it is below the kernel
     * boundary. */
    if (sys_write((const char *)0x7F000000u, 8) == SYS_EFAULT)
        u_puts("  [ring3] unmapped user pointer also refused\n");
    else
        u_puts("  [ring3] WARNING: kernel accepted an unmapped pointer!\n");
}

static void probe_bad_call_number(void)
{
    if (syscall3(9999, 0, 0, 0) == SYS_EBADCALL)
        u_puts("  [ring3] unknown syscall correctly rejected\n");
    else
        u_puts("  [ring3] WARNING: unknown syscall was not rejected!\n");
}

/* ---- fork, copy-on-write and wait --------------------------------------
 *
 * This variable is the whole experiment. It is initialised to a non-zero
 * value so it lands in .data - a writable page, which fork() therefore marks
 * read-only and copy-on-write in *both* processes rather than sharing or
 * duplicating outright.
 *
 * The child writes to it. That write faults, the kernel notices the page is
 * COW with a reference count above one, allocates a fresh frame, copies the
 * old contents in and makes the child's mapping writable. Afterwards the two
 * processes see different values at the same address - which is the only
 * observable difference between copy-on-write done correctly and a kernel
 * that simply shared the page.
 *
 * `volatile` is not decoration. Without it the compiler is entitled to
 * assume nothing between the two reads can have changed the variable, keep
 * the initial value in a register, and print the right answer for the wrong
 * reason - which would make the test pass on a kernel with no COW at all.
 */
static volatile u32 inherited = 0x5A5A5A5Au;

#define CHILD_WROTE 0x1234ABCDu
#define CHILD_EXIT  7

static void demo_fork_cow(void)
{
    u_puts("  [ring3] fork(): duplicating this process\n");

    i32 pid = sys_fork();

    if (pid < 0) {
        u_puts("  [ring3] WARNING: fork() failed\n");
        return;
    }

    if (pid == 0) {
        /* The child. Same code, same addresses, different process. */
        u_puts("    [child] fork() returned 0 here; my pid is ");
        u_putu((u32)sys_getpid());
        u_puts(", my parent is ");
        u_putu((u32)sys_getppid());
        u_puts("\n");

        u_puts("    [child] I inherited ");
        u_puthex(inherited);
        u_puts(" and am about to write over it\n");

        /* The copy-on-write fault happens on this line. */
        inherited = CHILD_WROTE;

        u_puts("    [child] my copy now reads ");
        u_puthex(inherited);
        if (inherited != CHILD_WROTE)
            u_puts(" - WARNING: the write did not take!");
        u_puts("\n");

        u_puts("    [child] exiting with ");
        u_putu(CHILD_EXIT);
        u_puts("\n");
        sys_exit(CHILD_EXIT);
    }

    /* The parent. */
    u_puts("  [ring3] fork() returned ");
    u_putu((u32)pid);
    u_puts(" here - one call, two return values\n");

    int status = -1;
    i32 reaped = sys_wait(&status);

    u_puts("  [ring3] wait() collected pid ");
    u_putu((u32)reaped);
    u_puts(" with exit code ");
    u_putu((u32)status);
    if (reaped != pid || status != CHILD_EXIT)
        u_puts(" - WARNING: that is not the child we forked!");
    u_puts("\n");

    /* The point of the whole exercise. */
    if (inherited == 0x5A5A5A5Au)
        u_puts("  [ring3] my own copy still reads 0x5a5a5a5a - copy-on-write "
               "gave the child a private page\n");
    else {
        u_puts("  [ring3] WARNING: my copy reads ");
        u_puthex(inherited);
        u_puts(" - the child wrote through into my address space!\n");
    }

    /* Nothing left to collect, and wait() must say so rather than block. */
    if (sys_wait(0) == -1)
        u_puts("  [ring3] wait() with no children returned -1, as it should\n");
    else
        u_puts("  [ring3] WARNING: wait() invented a child!\n");
}

/* ---- exec -------------------------------------------------------------- */

static void demo_exec(void)
{
    u_puts("  [ring3] exec(): forking a child to replace its own image\n");

    i32 pid = sys_fork();

    if (pid < 0) {
        u_puts("  [ring3] WARNING: fork() failed\n");
        return;
    }

    if (pid == 0) {
        /* A name the kernel does not have, first: exec must fail cleanly and
         * leave this process running its current image. If it instead tore
         * the address space down before checking, the next instruction would
         * fault. */
        if (sys_exec("nonexistent") != SYS_ENOENT) {
            u_puts("    [child] WARNING: exec() of a bogus name did not "
                   "fail!\n");
            sys_exit(1);
        }

        u_puts("    [child] exec(\"nonexistent\") failed cleanly and I am "
               "still here\n");

        /* An absolute path that does not exist must fail too, and must not
         * quietly fall back to something with a similar name - which is
         * exactly what a bare name does on purpose. */
        if (sys_exec("/bin/NOTHERE") != SYS_ENOENT) {
            u_puts("    [child] WARNING: exec() of a bogus path did not "
                   "fail!\n");
            sys_exit(1);
        }

        u_puts("    [child] exec(\"/bin/NOTHERE\") failed cleanly too\n");

        /* And now the real one. By bare name, so it resolves through the
         * filesystem when there is one and through the kernel's embedded
         * copies when there is not. This does not return. */
        (void)sys_exec("hello");

        u_puts("    [child] WARNING: exec(\"hello\") returned!\n");
        sys_exit(1);
    }

    int status = -1;
    i32 reaped = sys_wait(&status);

    u_puts("  [ring3] the exec'd child (pid ");
    u_putu((u32)reaped);
    u_puts(") exited with ");
    u_putu((u32)status);
    if (status != 0)
        u_puts(" - WARNING: it did not finish cleanly!");
    u_puts("\n");
}

void _start(void)
{
    u_puts("  [ring3] hello from user mode - privilege level 3\n");
    u_puts("  [ring3] I am a separate ELF, loaded into my own pages\n");

    u_puts("  [ring3] getpid() returned ");
    u_putu((u32)sys_getpid());
    u_puts("\n");

    u_puts("  [ring3] uptime() returned ");
    u_putu((u32)sys_uptime());
    u_puts(" s\n");

    sys_yield();
    u_puts("  [ring3] yielded and was rescheduled\n");

    sys_sleep(50);
    u_puts("  [ring3] slept 50 ms via syscall\n");

    /* Writing to our own stack must work - it is the one region we own. */
    volatile char scratch[32];
    for (int i = 0; i < 32; i++)
        scratch[i] = (char)i;
    if (scratch[31] == 31)
        u_puts("  [ring3] my own stack is writable, as it should be\n");

    probe_kernel_pointer();
    probe_unmapped_pointer();
    probe_bad_call_number();

    demo_fork_cow();
    demo_exec();

    u_puts("  [ring3] calling exit(0)\n");
    sys_exit(0);
}
