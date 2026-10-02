/* StratumOS - dropping to ring 3.
 *
 * The user program is a separate ELF, built from user/ and embedded in the
 * kernel image as a blob. The init task creates its own address space, loads
 * those segments into it, allocates a stack, and usermode_enter() forges an
 * inter-privilege IRET frame.
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
 *
 * The address space is built *inside* the task rather than by the caller, and
 * that ordering is the whole point. Kernel threads share the kernel's page
 * directory; if init loaded its image while that was current, its user pages
 * would live in the directory every kernel thread uses, and fork() would be
 * cloning the kernel's user half. Giving init a directory of its own first
 * makes "the process's address space" a real thing, which is what fork,
 * copy-on-write and exec all stand on.
 */
#define LOG_TAG "user"

#include <arch/gdt.h>

#include <kernel/elf.h>
#include <kernel/kernel.h>
#include <kernel/layout.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/usermode.h>

#include <mm/pmm.h>
#include <mm/vmm.h>

/* Produced by objcopy from the per-program ELFs under build/user - see the
 * Makefile. */
extern const u8 _binary_init_elf_start[];
extern const u8 _binary_init_elf_end[];
extern const u8 _binary_hello_elf_start[];
extern const u8 _binary_hello_elf_end[];

/* The programs this kernel can run, by name.
 *
 * This is exec()'s namespace, and it stands in for a filesystem - the one
 * piece exec genuinely needs and this kernel does not have yet. Everything
 * else about the call is real: the old address space is torn down, a new
 * image is loaded and validated, a fresh stack is mapped, and the process
 * keeps its pid. Replacing this table with a path lookup is the only change
 * that a filesystem would require, which is the point of naming it here
 * rather than hard-wiring one image into the loader. */
static const struct {
    const char *name;
    const u8 *start;
    const u8 *end;
} programs[] = {
    {"init", _binary_init_elf_start, _binary_init_elf_end},
    {"hello", _binary_hello_elf_start, _binary_hello_elf_end},
};

static bool ran;
static u32 exec_count;

bool usermode_ran(void)
{
    return ran;
}

u32 usermode_exec_count(void)
{
    return exec_count;
}

static size_t payload_size(void)
{
    return (size_t)(_binary_init_elf_end - _binary_init_elf_start);
}

const char *usermode_program_name(u32 index)
{
    return index < ARRAY_SIZE(programs) ? programs[index].name : NULL;
}

bool usermode_map_stack(void)
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

    return true;
}

/* Build this task's address space, load the embedded image into it and leave
 * for ring 3. Runs on the task's own kernel stack, with the task already
 * scheduled, so a failure here is a failed process and not a failed boot. */
static void usermode_task(void *arg)
{
    UNUSED(arg);

    struct task *self = task_current();
    size_t size = payload_size();

    if (size == 0) {
        pr_err("the embedded user program is empty");
        task_exit(1);
    }

    /* A directory of init's own. Its kernel half is the kernel's, so the
     * timer interrupt that arrives a moment from now is delivered exactly as
     * it was before the switch. */
    paddr_t pd = vmm_create_address_space();

    if (!pd) {
        pr_err("cannot create an address space for pid %u", self->pid);
        task_exit(1);
    }

    /* Record it before loading CR3: a preemption between the two would
     * otherwise restore the kernel directory and load the image into it. */
    self->page_dir = pd;
    vmm_switch_address_space(pd);

    struct elf_load_info loaded;

    if (!elf_load_user(_binary_init_elf_start, size, &loaded)) {
        pr_err("the embedded user program failed to load");
        task_exit(1);
    }

    if (!usermode_map_stack()) {
        elf_unload_user(&loaded);
        task_exit(1);
    }

    pr_info("user stack mapped %p-%p",
            (void *)(USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE),
            (void *)USER_STACK_TOP);

    /* The CPU finds the ring-0 stack to switch to on a trap in the TSS, so it
     * must point at *this* task's kernel stack before we leave ring 0. Get
     * this wrong and a ring-3 interrupt corrupts another task's stack. */
    tss_set_kernel_stack(self->kernel_esp0);
    self->user = true;

    pr_info("pid %u entering ring 3 at %p in its own address space (%u user "
            "pages mapped)",
            self->pid, (void *)loaded.entry, vmm_count_user_pages());
    ran = true;

    /* Does not return: the task spends the rest of its life in ring 3 and
     * leaves through the exit syscall. */
    usermode_enter(loaded.entry, USER_STACK_TOP);
}

bool usermode_spawn_demo(void)
{
    if (payload_size() == 0) {
        pr_err("the embedded user program is empty");
        return false;
    }

    pr_debug("embedded user program: %u bytes", (unsigned)payload_size());

    return task_create("init", usermode_task, NULL) != NULL;
}

/* ---- exec ---------------------------------------------------------------
 *
 * Replace the calling process's image with another, keeping its pid, its
 * parent, its kernel stack and its open state - everything except the user
 * half of its address space, which is thrown away and rebuilt.
 *
 * The return path is the trick. exec() does not return to its caller, because
 * its caller no longer exists: the code that issued `int 0x80` was in the old
 * image, and the old image has been unmapped. So instead of returning a value
 * through EAX, this rewrites the trap frame the syscall will IRET through -
 * EIP to the new entry point, ESP to the top of the new stack - and lets the
 * ordinary interrupt-return path deliver control into the new program. The
 * CPU cannot tell the difference between that and a process that was always
 * running this image.
 *
 * Order matters and is not negotiable:
 *
 *   1. copy the name out of user memory first. Step 2 unmaps it.
 *   2. tear the user half down.
 *   3. load, map a stack, rewrite the frame.
 *
 * Past step 2 there is no going back: the process has no image. A failure
 * there is therefore fatal to the process rather than an error code, which is
 * exactly how a real exec behaves.
 */
bool usermode_exec(const char *name, struct regs *r)
{
    const u8 *start = NULL;
    size_t size = 0;

    for (size_t i = 0; i < ARRAY_SIZE(programs); i++) {
        if (strcmp(name, programs[i].name) == 0) {
            start = programs[i].start;
            size = (size_t)(programs[i].end - programs[i].start);
            break;
        }
    }

    if (!start || size == 0)
        return false;

    struct task *self = task_current();

    /* A kernel thread has no user half to replace, and a process without its
     * own address space would be replacing the kernel's. Neither is a thing
     * exec can do. */
    if (!self->page_dir || self->page_dir == vmm_kernel_pd_phys()) {
        pr_err("pid %u called exec() without an address space of its own",
               self->pid);
        return false;
    }

    pr_info("pid %u exec(\"%s\"): replacing %u user pages", self->pid, name,
            vmm_count_user_pages());

    /* Point of no return. */
    vmm_clear_user_space();

    struct elf_load_info loaded;

    if (!elf_load_user(start, size, &loaded)) {
        pr_err("pid %u exec(\"%s\"): the image failed to load and the old one "
               "is already gone",
               self->pid, name);
        task_exit(1);
    }

    if (!usermode_map_stack()) {
        pr_err("pid %u exec(\"%s\"): no stack", self->pid, name);
        task_exit(1);
    }

    /* Rewrite the frame the syscall path is about to IRET through. The
     * general registers are cleared rather than preserved: a fresh image has
     * no business inheriting the old one's register contents, and leaving
     * them would make the new program's startup state depend on whatever its
     * predecessor happened to be doing. */
    r->eip = loaded.entry;
    r->user_esp = USER_STACK_TOP;
    r->eax = 0;
    r->ebx = r->ecx = r->edx = 0;
    r->esi = r->edi = r->ebp = 0;

    exec_count++;

    pr_info("pid %u now running \"%s\" at %p, %u user pages", self->pid, name,
            (void *)loaded.entry, vmm_count_user_pages());

    return true;
}
