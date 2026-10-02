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

/* Free the stacks of tasks that have exited. Only ever called from a task
 * that is definitely not the one being reaped - freeing the stack you are
 * standing on is not recoverable. */
static void reap_zombies(void)
{
    for (size_t i = 1; i < TASK_MAX; i++) {
        struct task *t = &tasks[i];

        if (t->state != TASK_ZOMBIE || t == current)
            continue;

        bool irqs = irq_save();

        /* Unlink from the run queue. */
        struct task *p = current;
        while (p->next != t && p->next != current)
            p = p->next;
        if (p->next == t)
            p->next = t->next;

        void *stack = t->stack_base;
        u32 pid = t->pid;

        t->state = TASK_UNUSED;
        t->stack_base = NULL;
        t->next = NULL;
        irq_restore(irqs);

        if (stack)
            kfree(stack);
        pr_debug("reaped pid %u", pid);
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

    struct task *next = pick_next();
    /* pick_next() will not return a zombie, so this always moves us off the
     * dying task. Its stack stays allocated until someone else reaps it. */
    switch_to(next);
    irq_restore(irqs);

    panic("a zombie task was scheduled again (pid %u)", current->pid);
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
