/* StratumOS - tasks and the round-robin scheduler.
 *
 * A kernel thread's entire context is its stack pointer. Switching means
 * pushing the callee-saved registers, recording ESP, loading the next task's
 * ESP and popping - see arch/x86/switch.asm. There is no TSS-based hardware
 * task switching here; software switching is faster, portable in spirit, and
 * vastly easier to reason about.
 *
 * Where the switch happens matters more than how. Preemption is requested by
 * the timer interrupt but *performed* at the very tail of the interrupt path,
 * after the PIC has been acknowledged. Switching before the EOI would park the
 * outgoing task mid-handler with the interrupt still in service; if the
 * incoming task then blocked somewhere other than an interrupt, nothing would
 * ever send that EOI and the timer would stop for good. That failure mode
 * takes a long evening to find, so it is worth the comment.
 *
 * The boot context becomes the idle task rather than being abandoned, which
 * means there is always exactly one runnable task and pick_next() never has
 * to answer "what if nothing can run".
 */
#define LOG_TAG "sched"

#include <arch/gdt.h>
#include <arch/io.h>

#include <drivers/timer.h>

#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/string.h>

#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

/* Laid out by task_create() so that switch.asm's `ret` lands here. */
extern void thread_trampoline(void);

/* Kernel stack from arch/x86/boot.asm, used by the idle/boot task. */
extern u8 stack_top[];

static struct task tasks[TASK_MAX];
static struct task *current;
static struct task *idle;
static u32 next_pid;
static u32 switch_count;
static volatile bool need_resched;
static bool sched_ready;

static const char *const state_names[] = {
    "unused", "ready", "running", "sleeping", "blocked", "zombie",
};

const char *task_state_name(enum task_state s)
{
    if ((unsigned)s >= ARRAY_SIZE(state_names))
        return "?";
    return state_names[s];
}

struct task *task_current(void)
{
    return current;
}

u32 sched_switch_count(void)
{
    return switch_count;
}

void sched_init(void)
{
    memset(tasks, 0, sizeof(tasks));

    /* Adopt the context we are already running in as task 0. It owns the
     * boot stack, so there is nothing to allocate and nothing to switch to
     * in order to get started. */
    idle = &tasks[0];
    idle->pid = 0;
    strlcpy(idle->name, "idle", sizeof(idle->name));
    idle->state = TASK_RUNNING;
    idle->quantum_left = SCHED_QUANTUM;
    idle->stack_base = NULL; /* not heap-allocated; never freed */
    idle->stack_size = 0;
    idle->kernel_esp0 = (u32)stack_top;
    idle->parent_pid = 0;
    /* Kernel threads all run in the kernel's address space. Only a forked
     * process gets its own. */
    idle->page_dir = vmm_kernel_pd_phys();
    idle->next = idle;

    current = idle;
    next_pid = 1;
    switch_count = 0;
    need_resched = false;
    sched_ready = true;

    tss_set_kernel_stack(idle->kernel_esp0);
    pr_info("scheduler ready; boot context adopted as pid 0 (idle)");
}

static struct task *alloc_slot(void)
{
    for (size_t i = 1; i < TASK_MAX; i++)
        if (tasks[i].state == TASK_UNUSED)
            return &tasks[i];
    return NULL;
}

struct task *task_create(const char *name, task_entry_t entry, void *arg)
{
    if (!sched_ready)
        panic("task_create(\"%s\") before sched_init()", name);

    bool irqs = irq_save();
    struct task *t = alloc_slot();

    if (!t) {
        irq_restore(irqs);
        pr_err("cannot create \"%s\": all %u task slots in use", name,
               (unsigned)TASK_MAX);
        return NULL;
    }

    /* Claim the slot before dropping the lock so a concurrent create cannot
     * hand out the same one. */
    t->state = TASK_BLOCKED;
    irq_restore(irqs);

    void *stack = kmalloc_aligned(TASK_STACK_SIZE, 16);
    if (!stack) {
        t->state = TASK_UNUSED;
        pr_err("cannot create \"%s\": no memory for a %u KiB stack", name,
               (unsigned)(TASK_STACK_SIZE / KIB));
        return NULL;
    }

    memset(t->name, 0, sizeof(t->name));
    strlcpy(t->name, name, sizeof(t->name));
    t->stack_base = stack;
    t->stack_size = TASK_STACK_SIZE;
    t->kernel_esp0 = (u32)stack + TASK_STACK_SIZE;
    t->quantum_left = SCHED_QUANTUM;
    t->ticks_total = 0;
    t->switches = 0;
    t->wake_at = 0;
    t->exit_code = 0;
    t->user = false;
    t->resources_freed = false;
    t->parent_pid = current ? current->pid : 0;
    /* A kernel thread shares the kernel's address space; there is nothing
     * private for it to see. */
    t->page_dir = vmm_kernel_pd_phys();

    /* Build the stack so that context_switch()'s epilogue - pop ebp/edi/esi/
     * ebx, popfd, ret - delivers control to thread_trampoline with `entry`
     * and `arg` sitting where a normal cdecl call would have left them.
     *
     *   sp[0..3] ebp edi esi ebx   (popped)
     *   sp[4]    eflags            (popfd; IF set so the task is preemptible)
     *   sp[5]    return address -> thread_trampoline
     *   sp[6]    trampoline's own unused return slot
     *   sp[7]    entry
     *   sp[8]    arg
     */
    u32 *sp = (u32 *)((u8 *)stack + TASK_STACK_SIZE);

    *--sp = (u32)arg;
    *--sp = (u32)entry;
    *--sp = 0;
    *--sp = (u32)thread_trampoline;
    *--sp = 0x202; /* IF | reserved bit 1 */
    *--sp = 0;     /* ebx */
    *--sp = 0;     /* esi */
    *--sp = 0;     /* edi */
    *--sp = 0;     /* ebp - zero terminates a backtrace cleanly */

    t->saved_esp = (u32)sp;

    irqs = irq_save();
    t->pid = next_pid++;
    /* Splice into the circular run queue just after the current task. */
    t->next = current->next;
    current->next = t;
    t->state = TASK_READY;
    irq_restore(irqs);

    pr_debug("created pid %u \"%s\", stack %p-%p", t->pid, t->name, stack,
             (void *)((u8 *)stack + TASK_STACK_SIZE));
    return t;
}

/* Release one dead task's stack and address space.
 *
 * Only ever called from a task that is definitely not the one being released -
 * freeing the stack you are standing on, or the address space you are running
 * in, is not recoverable. Two callers qualify: the idle task's reaper, and a
 * parent collecting a child in wait().
 *
 * The task *slot* is deliberately not freed here. It stays ZOMBIE, holding
 * the exit code, until a parent collects it. That is what a zombie process
 * is: resources gone, exit status still owed to somebody.
 *
 * Idempotent, because both callers race for it: `resources_freed` is the
 * flag, set under the same IRQ-off window that unlinks the task.
 */
static void release_task_resources(struct task *t)
{
    bool irqs = irq_save();

    if (t->resources_freed || t == current) {
        irq_restore(irqs);
        return;
    }

    /* Unlink from the run queue so pick_next() stops walking through it. The
     * walk starts at `current`, which is by definition still in the list. */
    struct task *p = current;
    while (p->next != t && p->next != current)
        p = p->next;
    if (p->next == t)
        p->next = t->next;

    void *stack = t->stack_base;
    paddr_t pd = t->page_dir;
    u32 pid = t->pid;

    t->stack_base = NULL;
    t->next = NULL;
    t->resources_freed = true;
    /* Keep page_dir readable for diagnostics but make clear it is gone. */
    t->page_dir = 0;
    irq_restore(irqs);

    if (stack)
        kfree(stack);

    /* A forked process owns its address space; a kernel thread shares the
     * kernel's and must not free it. */
    if (pd && pd != vmm_kernel_pd_phys())
        vmm_destroy_address_space(pd);

    pr_debug("released pid %u's stack and address space", pid);
}

/* Sweep every zombie that nobody has collected yet. Called from the idle
 * task, so a process whose parent never calls wait() still has its memory
 * returned - only the slot lingers. */
static void reap_zombies(void)
{
    for (size_t i = 1; i < TASK_MAX; i++) {
        struct task *t = &tasks[i];

        if (t->state != TASK_ZOMBIE || t == current || t->resources_freed)
            continue;

        release_task_resources(t);
    }
}

/* Move sleepers whose deadline has passed back to READY. */
static void wake_sleepers(void)
{
    u64 now = timer_ticks();

    for (size_t i = 0; i < TASK_MAX; i++)
        if (tasks[i].state == TASK_SLEEPING && now >= tasks[i].wake_at)
            tasks[i].state = TASK_READY;
}

/* Round robin: walk the circular queue from just after the current task and
 * take the first thing that can run. Idle is the fallback, never a choice. */
static struct task *pick_next(void)
{
    struct task *t = current->next;

    for (size_t guard = 0; guard < TASK_MAX + 1; guard++) {
        if (!t)
            break;
        if (t != idle && t->state == TASK_READY)
            return t;
        t = t->next;
        if (t == current->next)
            break;
    }

    if (current != idle && current->state == TASK_RUNNING)
        return current; /* nobody else wants the CPU */

    return idle;
}

static void switch_to(struct task *next)
{
    struct task *prev = current;

    if (next == prev) {
        prev->quantum_left = SCHED_QUANTUM;
        return;
    }

    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;

    next->state = TASK_RUNNING;
    next->quantum_left = SCHED_QUANTUM;
    next->switches++;
    current = next;
    switch_count++;

    /* Tell the CPU which stack to land on if an interrupt arrives while this
     * task is in ring 3. Wrong value here means a ring-3 interrupt corrupts
     * some other task's stack. */
    tss_set_kernel_stack(next->kernel_esp0);

    /* Switch address spaces. Safe to do before the stack swap because every
     * task's kernel stack lives in the kernel heap, which is mapped
     * identically in every address space - that identity is the whole reason
     * vmm_create_address_space() copies the kernel's page directory half. */
    if (next->page_dir && next->page_dir != prev->page_dir)
        vmm_switch_address_space(next->page_dir);

    context_switch(&prev->saved_esp, next->saved_esp);
}

void sched_tick(void)
{
    if (!sched_ready)
        return;

    current->ticks_total++;
    wake_sleepers();

    if (current->quantum_left > 0)
        current->quantum_left--;

    /* Idle gives up the CPU the instant anything else is runnable; a real
     * task keeps it until its quantum expires. */
    if (current->quantum_left == 0 || current == idle)
        need_resched = true;
}

void sched_preempt(void)
{
    if (!sched_ready || !need_resched)
        return;

    need_resched = false;

    struct task *next = pick_next();
    if (next != current)
        switch_to(next);
}

void sched_yield(void)
{
    if (!sched_ready) {
        /* Before the scheduler exists, the only sensible "wait" is to let an
         * interrupt happen. */
        __asm__ volatile("sti; hlt");
        return;
    }

    bool irqs = irq_save();

    wake_sleepers();
    need_resched = false;
    struct task *next = pick_next();
    if (next != current)
        switch_to(next);

    /* On the way back in, our saved EFLAGS had IF clear because of the
     * irq_save() above, so the caller's interrupt state is restored here
     * rather than inherited from whoever resumed us. */
    irq_restore(irqs);
}

void task_sleep_ms(u32 ms)
{
    u32 hz = timer_hz();

    if (!sched_ready || hz == 0) {
        timer_busy_wait_ms(ms);
        return;
    }

    /* Round up: a 1 ms sleep at 100 Hz must wait a tick, not zero. */
    u64 ticks_to_wait = ((u64)ms * hz + 999) / 1000;
    if (ticks_to_wait == 0)
        ticks_to_wait = 1;

    bool irqs = irq_save();
    current->wake_at = timer_ticks() + ticks_to_wait;
    current->state = TASK_SLEEPING;
    irq_restore(irqs);

    sched_yield();
}

void task_block(void)
{
    if (!sched_ready)
        return;

    bool irqs = irq_save();
    current->state = TASK_BLOCKED;
    irq_restore(irqs);

    sched_yield();
}

void task_unblock(struct task *t)
{
    if (!t)
        return;

    bool irqs = irq_save();
    if (t->state == TASK_BLOCKED)
        t->state = TASK_READY;
    irq_restore(irqs);
}

NORETURN void task_exit(int code)
{
    if (!sched_ready || current == idle)
        panic("task_exit(%d) called from the idle task", code);

    pr_debug("pid %u \"%s\" exited with %d", current->pid, current->name, code);

    bool irqs = irq_save();
    current->exit_code = code;
    current->state = TASK_ZOMBIE;
    need_resched = false;

    /* If the parent is blocked in wait(), it is waiting for exactly this. */
    for (size_t i = 0; i < TASK_MAX; i++) {
        if (tasks[i].pid == current->parent_pid &&
            tasks[i].state == TASK_BLOCKED) {
            tasks[i].state = TASK_READY;
            break;
        }
    }

    struct task *next = pick_next();
    /* pick_next() will not return a zombie, so this always moves us off the
     * dying task. Its stack stays allocated until someone else reaps it. */
    switch_to(next);
    irq_restore(irqs);

    panic("a zombie task was scheduled again (pid %u)", current->pid);
}

/* ------------------------------------------------------------------------- */
/* Processes                                                                 */
/* ------------------------------------------------------------------------- */

int task_fork(const struct regs *parent_frame)
{
    if (!sched_ready || !parent_frame)
        return -1;

    bool irqs = irq_save();
    struct task *child = alloc_slot();

    if (!child) {
        irq_restore(irqs);
        pr_err("fork: all %u task slots are in use", (unsigned)TASK_MAX);
        return -1;
    }

    child->state = TASK_BLOCKED; /* claim the slot before unlocking */
    irq_restore(irqs);

    void *stack = kmalloc_aligned(TASK_STACK_SIZE, 16);
    if (!stack) {
        child->state = TASK_UNUSED;
        pr_err("fork: no memory for the child's kernel stack");
        return -1;
    }

    /* Copy-on-write clone of the caller's address space. Every writable page
     * becomes read-only in *both* and the frames are shared until written. */
    paddr_t child_pd = vmm_clone_current();
    if (!child_pd) {
        kfree(stack);
        child->state = TASK_UNUSED;
        pr_err("fork: could not clone the address space");
        return -1;
    }

    struct task *parent = current;

    memset(child->name, 0, sizeof(child->name));
    strlcpy(child->name, parent->name, sizeof(child->name));

    child->stack_base = stack;
    child->stack_size = TASK_STACK_SIZE;
    child->kernel_esp0 = (u32)stack + TASK_STACK_SIZE;
    child->page_dir = child_pd;
    child->parent_pid = parent->pid;
    child->quantum_left = SCHED_QUANTUM;
    child->ticks_total = 0;
    child->switches = 0;
    child->wake_at = 0;
    child->exit_code = 0;
    child->user = parent->user;
    child->resources_freed = false;

    /* Build the child's kernel stack so that the first context switch into it
     * restores a copy of the parent's trap frame and returns to user space.
     *
     * Top of the stack holds the frame itself; below it, the five words
     * context_switch's epilogue pops, then the trampoline's argument:
     *
     *   [struct regs]                       <- the copied frame, eax = 0
     *   [&frame]                            <- popped by fork_trampoline
     *   [fork_trampoline]                   <- context_switch's `ret`
     *   [eflags] [ebx] [esi] [edi] [ebp]    <- popfd and four pops
     *                                  ^-- saved_esp
     */
    u8 *top = (u8 *)stack + TASK_STACK_SIZE;
    struct regs *frame = (struct regs *)(top - sizeof(struct regs));

    *frame = *parent_frame;

    /* The one difference between parent and child, and the whole trick:
     * fork returns the child's pid in the parent and 0 in the child.
     * isr_restore_and_return's `popa` picks this up. */
    frame->eax = 0;

    u32 *sp = (u32 *)frame;
    *--sp = (u32)frame;
    *--sp = (u32)fork_trampoline;
    *--sp = 0x202; /* IF | reserved bit 1 */
    *--sp = 0;     /* ebx */
    *--sp = 0;     /* esi */
    *--sp = 0;     /* edi */
    *--sp = 0;     /* ebp */

    child->saved_esp = (u32)sp;

    irqs = irq_save();
    child->pid = next_pid++;
    child->next = current->next;
    current->next = child;
    child->state = TASK_READY;
    irq_restore(irqs);

    pr_info("fork: pid %u -> pid %u, address space %p", parent->pid, child->pid,
            (void *)child_pd);

    return (int)child->pid;
}

int task_wait(int *status_out)
{
    if (!sched_ready)
        return -1;

    for (;;) {
        bool irqs = irq_save();
        bool have_children = false;

        for (size_t i = 1; i < TASK_MAX; i++) {
            struct task *t = &tasks[i];

            if (t->state == TASK_UNUSED || t->parent_pid != current->pid)
                continue;

            if (t->state == TASK_ZOMBIE) {
                int pid = (int)t->pid;

                if (status_out)
                    *status_out = t->exit_code;

                irq_restore(irqs);

                /* The reaper may not have got to this child yet, and the slot
                 * must not go back into circulation while it is still in the
                 * run queue holding a stack and a page directory. Releasing
                 * here rather than waiting for idle is also what makes
                 * wait() the point at which a child's memory is definitely
                 * gone. Idempotent, so racing the reaper is harmless. */
                release_task_resources(t);

                /* Collecting the exit code is what finally frees the slot. */
                irqs = irq_save();
                t->state = TASK_UNUSED;
                t->parent_pid = 0;
                irq_restore(irqs);
                return pid;
            }

            have_children = true;
        }

        if (!have_children) {
            irq_restore(irqs);
            return -1;
        }

        /* Block rather than spin. task_exit() moves us back to READY. */
        current->state = TASK_BLOCKED;
        irq_restore(irqs);
        sched_yield();
    }
}

u32 task_count(void)
{
    u32 n = 0;

    for (size_t i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != TASK_UNUSED)
            n++;
    return n;
}

NORETURN void sched_start(void)
{
    /* This is the idle task's body. Reaping happens here because it is the
     * one context guaranteed not to be standing on the stack it frees. */
    for (;;) {
        reap_zombies();
        __asm__ volatile("sti; hlt");
    }
}

void sched_foreach(void (*fn)(const struct task *t, void *ctx), void *ctx)
{
    for (size_t i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != TASK_UNUSED)
            fn(&tasks[i], ctx);
}
