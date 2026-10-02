/* StratumOS - dropping to ring 3.
 *
 * The user program is a separate ELF, built from user/ and embedded in the
 * kernel image as a blob. At boot its segments are loaded into
 * user-accessible pages by kernel/core/elf.c, a stack is allocated, and
 * usermode_enter() forges an inter-privilege IRET frame.
 *
 * Building it separately rather than as a section of the kernel is not
 * cosmetic. A program linked at a kernel address and then mapped somewhere
 * else would have every absolute reference - every string literal - pointing
 * into the kernel's half, where ring 3 cannot read. Linking it for user space
 * is the only way its own addresses are addresses it can use.
 *
 * Embedding the blob rather than reading it from a filesystem is a scoping
 * decision: there is no filesystem yet, and the loader is written so that
 * swapping the blob for a file read is the only change needed.
 */
#define LOG_TAG "user"

#include <arch/gdt.h>

#include <kernel/elf.h>
#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/usermode.h>

#include <mm/pmm.h>
#include <mm/vmm.h>

/* Produced by objcopy from build/user/init.elf - see the Makefile. */
extern const u8 _binary_init_elf_start[];
extern const u8 _binary_init_elf_end[];

/* The user stack. Placed well below the kernel boundary and well above the
 * program image, so that an overflow in either direction lands on an unmapped
 * page and faults rather than quietly corrupting the other. */
#define USER_STACK_TOP   0xB0000000u
#define USER_STACK_PAGES 4

static bool ran;
static struct elf_load_info loaded;

bool usermode_ran(void)
{
    return ran;
}

static size_t payload_size(void)
{
    return (size_t)(_binary_init_elf_end - _binary_init_elf_start);
}

static bool map_user_stack(void)
{
    for (u32 i = 1; i <= USER_STACK_PAGES; i++) {
        vaddr_t page = USER_STACK_TOP - i * PAGE_SIZE;

        if (!vmm_alloc_at(page, PTE_PRESENT | PTE_WRITE | PTE_USER)) {
            pr_err("cannot allocate the user stack");
            for (u32 j = 1; j < i; j++)
                vmm_unmap(USER_STACK_TOP - j * PAGE_SIZE);
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
     * must point at *this* task's kernel stack before we leave ring 0. Get
     * this wrong and a ring-3 interrupt corrupts another task's stack. */
    tss_set_kernel_stack(self->kernel_esp0);
    self->user = true;

    pr_info("pid %u entering ring 3 at %p", self->pid, (void *)loaded.entry);
    ran = true;

    /* Does not return: the task spends the rest of its life in ring 3 and
     * leaves through the exit syscall. */
    usermode_enter(loaded.entry, USER_STACK_TOP);
}

bool usermode_spawn_demo(void)
{
    size_t size = payload_size();

    if (size == 0) {
        pr_err("the embedded user program is empty");
        return false;
    }

    pr_debug("embedded user program: %u bytes", (unsigned)size);

    if (!elf_load_user(_binary_init_elf_start, size, &loaded))
        return false;

    if (!map_user_stack()) {
        elf_unload_user(&loaded);
        return false;
    }

    if (!task_create("init", usermode_task, NULL)) {
        elf_unload_user(&loaded);
        return false;
    }

    return true;
}
