/* StratumOS - the program exec() replaces a process with.
 *
 * Its only job is to be a *different* image from init: a different entry
 * point, different code, different string literals, laid out at the same user
 * addresses. That difference is the proof, because the thing exec() has to
 * demonstrate is not "a program ran" but "this process's address space was
 * torn down and rebuilt around a new image while the process survived".
 *
 * The pid is what carries across. A fork gives you a new pid running the old
 * image; an exec gives you the old pid running a new image. Printing the pid
 * here and comparing it with what the caller printed before the exec is the
 * whole test, and it is the reason this file exists separately rather than
 * init simply calling a second function.
 */
#include "syscall.h"

void _start(void);

/* Addresses the kernel must still be refusing after the address space was
 * rebuilt. exec() clears the user half and loads a new image; if it left a
 * stale page table entry behind, a pointer that should fault would not. */
static void probe_boundary_after_exec(void)
{
    if (sys_write((const char *)0xC0100000u, 16) != SYS_EFAULT) {
        u_puts("  [exec] WARNING: kernel accepted a kernel pointer after "
               "exec!\n");
        return;
    }

    /* init's stack lived here before the exec and the new stack is mapped at
     * the same place, so this proves nothing on its own. The page *below* the
     * stack is the interesting one: exec must not have left it mapped. */
    if (sys_write((const char *)(0xB0000000u - 5 * 4096), 8) != SYS_EFAULT) {
        u_puts("  [exec] WARNING: a page below the new stack is still "
               "mapped!\n");
        return;
    }

    u_puts("  [exec] the rebuilt address space still refuses kernel and "
           "unmapped pointers\n");
}

void _start(void)
{
    u_puts("  [exec] hello: a different image, running in the same process\n");

    u_puts("  [exec] getpid() returned ");
    u_putu((u32)sys_getpid());
    u_puts(" - the pid survived exec, the image did not\n");

    probe_boundary_after_exec();

    u_puts("  [exec] calling exit(0)\n");
    sys_exit(0);
}
