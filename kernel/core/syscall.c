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
#include <arch/idt.h>

#include <drivers/keyboard.h>
#include <drivers/timer.h>

#include <kernel/console.h>
#include <kernel/layout.h>
#include <kernel/log.h>
#include <kernel/sched.h>
#include <kernel/syscall.h>
#include <kernel/usermode.h>

#include <mm/vmm.h>

#define SYS_WRITE_MAX 1024

static u32 call_count;

bool user_range_ok(vaddr_t base, size_t len)
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

        if (page + PAGE_SIZE < page)
            break; /* reached the top of the address space */
    }

    return true;
}

static i32 sys_write(u32 ptr, u32 len)
{
    if (len > SYS_WRITE_MAX)
        len = SYS_WRITE_MAX;

    if (!user_range_ok(ptr, len)) {
        pr_warn("pid %u passed an unreadable buffer %p+%u to write()",
                task_current() ? task_current()->pid : 0, (void *)ptr, len);
        return SYS_EFAULT;
    }

    console_write((const char *)ptr, len);
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
            pr_warn("pid %u passed an unreadable string pointer %p",
                    task_current() ? task_current()->pid : 0, (void *)ptr);
            return false;
        }

        dst[i] = *(const char *)(ptr + i);

        if (dst[i] == '\0')
            return true;
    }

    pr_warn("pid %u passed a string longer than %u bytes",
            task_current() ? task_current()->pid : 0, (unsigned)max);
    return false;
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

    case SYS_WRITE:
        ret = sys_write(r->ebx, r->ecx);
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
            if (!user_range_ok(r->ebx, sizeof(int))) {
                pr_warn("pid %u passed an unwritable status pointer %p to "
                        "wait()",
                        task_current() ? task_current()->pid : 0,
                        (void *)r->ebx);
                ret = SYS_EFAULT;
                break;
            }
            *(int *)r->ebx = status;
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
            pr_warn("pid %u asked to exec \"%s\", which is not a program this "
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
        pr_warn("unknown syscall %u from EIP %p", r->eax, (void *)r->eip);
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
