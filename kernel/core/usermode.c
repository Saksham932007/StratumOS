/* StratumOS - dropping to ring 3.
 *
 * The user payload is compiled into the kernel image in its own `.user`
 * section (see core/user_demo.c and linker/kernel.ld). At boot its pages are
 * re-mapped with the USER bit set - read-only and executable - and a separate
 * writable user stack is allocated. Then usermode_enter() forges an
 * inter-privilege IRET frame and the CPU drops to ring 3.
 *
 * Shipping the payload inside the kernel rather than loading it from a
 * filesystem is a deliberate scoping decision: there is no filesystem yet, and
 * the point of the exercise is the privilege transition, not ELF loading -
 * which the bootloader already demonstrates.
 */
#define LOG_TAG "user"

#include <arch/gdt.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/usermode.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

/* Provided by the linker script, page-aligned on both ends. */
extern u8 __user_start[];
extern u8 __user_end[];

/* The user stack. Placed well away from both the identity map and the kernel
 * heap window so that a stack overflow lands on an unmapped page and faults
 * instead of quietly corrupting something. */
#define USER_STACK_TOP   0xB0000000u
#define USER_STACK_PAGES 2

static bool ran;

bool usermode_ran(void)
{
    return ran;
}

static bool map_user_payload(void)
{
    u32 start = (u32)__user_start;
    u32 end = (u32)__user_end;

    if (end <= start) {
        pr_err("the .user section is empty - nothing to run in ring 3");
        return false;
    }

    if (!IS_ALIGNED(start, PAGE_SIZE)) {
        pr_err(".user section is not page aligned (%p)", (void *)start);
        return false;
    }

    /* The payload is already identity-mapped as part of the kernel image; all
     * that changes is its permissions. USER is added so ring 3 can read and
     * execute it, and WRITE is deliberately left off so it cannot patch its
     * own code. vmm_protect() keeps the existing frame, which is why this is
     * not a vmm_map() call. */
    if (!vmm_protect_range(start, end - start, PTE_PRESENT | PTE_USER)) {
        pr_err("cannot change the .user section's permissions");
        return false;
    }

    pr_info(".user payload mapped %p-%p as ring-3 read/execute",
            (void *)start, (void *)end);
    return true;
}

static bool map_user_stack(void)
{
    for (u32 i = 1; i <= USER_STACK_PAGES; i++) {
        vaddr_t page = USER_STACK_TOP - i * PAGE_SIZE;

        if (!vmm_alloc_at(page, PTE_PRESENT | PTE_WRITE | PTE_USER)) {
            pr_err("cannot allocate the user stack");
            return false;
        }
    }

    pr_info("user stack mapped %p-%p",
            (void *)(USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE),
            (void *)USER_STACK_TOP);
    return true;
}

static void usermode_task(void *arg)
{
    UNUSED(arg);

    struct task *self = task_current();

    /* The CPU finds the ring-0 stack to switch to on a trap in the TSS, so it
     * must point at *this* task's kernel stack before we leave ring 0. */
    tss_set_kernel_stack(self->kernel_esp0);
    self->user = true;

    pr_info("pid %u entering ring 3 at %p", self->pid,
            (void *)user_demo_entry);
    ran = true;

    /* Does not return: the task's remaining life is spent in ring 3, and it
     * leaves via the exit syscall. */
    usermode_enter((vaddr_t)user_demo_entry, USER_STACK_TOP);
}

bool usermode_spawn_demo(void)
{
    if (!map_user_payload())
        return false;
    if (!map_user_stack())
        return false;

    return task_create("usermode", usermode_task, NULL) != NULL;
}
