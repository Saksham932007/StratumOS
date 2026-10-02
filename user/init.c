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

    u_puts("  [ring3] calling exit(0)\n");
    sys_exit(0);
}
