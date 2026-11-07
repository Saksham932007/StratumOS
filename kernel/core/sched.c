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
 * Each processor's boot context becomes that processor's idle task rather
 * than being abandoned - the boot context for the processor that booted, and
 * the context ap_main() is standing in for every other. So every processor
 * always has something to be, and pick_next_locked() never has to answer
 * "what if nothing can run".
 *
 * The scheduler runs on all of them, from one shared run queue, under one
 * lock at the bottom of the kernel's lock hierarchy. The part of that which
 * is not obvious is that the lock cannot be held across the context switch:
 * see the comment on sched_lock, do_switch() and sched_handoff(), in that
 * order, and docs/SMP.md for the four bugs the change surfaced.
 */
#define LOG_TAG "sched"

#include <arch/gdt.h>
#include <arch/io.h>

#include <drivers/timer.h>

#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/smp.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

/* Laid out by task_create() so that switch.asm's `ret` lands here. */
extern void thread_trampoline(void);

/* Kernel stack from arch/x86/boot.asm, used by the idle/boot task. */
extern u8 stack_top[];

static struct task tasks[TASK_MAX];
static u32 next_pid;
static u32 switch_count;
static bool sched_ready;
static bool aps_may_join;

/* The scheduler's lock. Level 1, the bottom of the hierarchy, which is the
 * right place for it: a task is created while holding the filesystem's lock
 * (a descriptor table), it is woken while holding the network's (a segment
 * arrived), and it is killed while holding none - so every other subsystem
 * has to be allowed to reach this one and none of them may be reached from
 * under it. See kernel/spinlock.h.
 *
 * It protects `tasks[]`, the run-queue ring threaded through `->next`, the
 * pid counter, and every processor's `current`, `idle` and `prev`.
 *
 * What it must never be held across is the context switch itself. Not
 * because of deadlock - because of what the lock *is*: a spinlock holds its
 * saved interrupt flag in the lock, and a context switch hands control to a
 * task that will eventually release the lock from a different stack with a
 * different interrupt state. do_switch() therefore releases it before
 * swapping stacks, having first arranged that there is nothing left to
 * protect; see the comment there, and sched_handoff(). Every acquisition is
 * wrapped in an explicit irq_save()/irq_restore() so that the release
 * cannot re-enable interrupts in the middle of a switch. */
static spinlock_t sched_lock = SPINLOCK_INIT_AT("sched", LOCK_LEVEL_SCHED);

/* The round-robin cursor. One for the whole machine rather than one per
 * processor, because the run queue is one queue: see the note on
 * pick_next_locked(). */
static struct task *rr_cursor;

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
    /* Before sched_init() there is no task, which is what the NULL means and
     * what every caller of this already tests for. Asking smp_this_cpu()
     * first would be asking the task register what it is before the GDT has
     * one. */
    return sched_ready ? smp_this_cpu()->current : NULL;
}

u32 sched_switch_count(void)
{
    return __atomic_load_n(&switch_count, __ATOMIC_RELAXED);
}

bool sched_ready_for_aps(void)
{
    return __atomic_load_n(&aps_may_join, __ATOMIC_ACQUIRE);
}

/* Build the idle task for one processor.
 *
 * An idle task is not allocated and never freed: it adopts the stack the
 * processor is already standing on - the boot stack for the bootstrap
 * processor, the stack smp_init() handed it for an application processor -
 * and it occupies the slot in tasks[] with that processor's index, so the
 * mapping from processor to idle task needs no lookup and cannot drift.
 *
 * Every idle task is pid 0. That is not laziness: pid 0 is how the rest of
 * the kernel asks "is this a real task", and the profiler, the reaper and
 * pick_next_locked() all want the same answer for all of them. */
/* Defined further down with the run queue, but needed by the task lifecycle
 * above it. */
static void runqueue_insert_locked(struct task *t);
static bool running_somewhere_locked(const struct task *t);
static void reap_zombies(void);

static struct task *idle_init(struct cpu *cpu, u32 esp0)
{
    struct task *t = &tasks[cpu->index];

    t->pid = 0;
    strlcpy(t->name, "idle", sizeof(t->name));
    t->state = TASK_RUNNING;
    t->quantum_left = SCHED_QUANTUM;
    t->stack_base = NULL; /* not heap-allocated; never freed */
    t->stack_size = 0;
    t->kernel_esp0 = esp0;
    t->parent_pid = 0;
    t->cpu = cpu->index;
    /* Kernel threads all run in the kernel's address space. Only a forked
     * process gets its own. */
    t->page_dir = vmm_kernel_root_phys();
    t->next = NULL; /* idle tasks are not in the run queue; see pick_next */

    cpu->idle = t;
    cpu->current = t;
    cpu->prev = NULL;
    cpu->need_resched = false;

    return t;
}

void sched_init(void)
{
    memset(tasks, 0, sizeof(tasks));

    next_pid = 1;
    switch_count = 0;

    /* Adopt the context we are already running in as this processor's idle
     * task. It owns the boot stack, so there is nothing to allocate and
     * nothing to switch to in order to get started. */
    struct cpu *self = smp_this_cpu();
    struct task *mine = idle_init(self, (u32)stack_top);

    rr_cursor = NULL;
    sched_ready = true;
    tss_set_kernel_stack(mine->kernel_esp0);

    /* And one for every other processor that is already online. They are
     * sitting in ap_main() waiting for exactly this; building their idle
     * tasks here rather than letting each build its own keeps every write to
     * tasks[] on one processor until the moment the ring opens. */
    u32 joined = 0;

    for (u32 i = 0; i < SMP_MAX_CPUS; i++) {
        const struct cpu *c = smp_cpu(i);

        if (!c || !c->online || c->index == self->index)
            continue;

        idle_init(smp_cpu_slot(i), c->stack_top);
        joined++;
    }

    _Static_assert(SMP_MAX_CPUS < TASK_MAX,
                   "every processor needs a tasks[] slot for its idle task, "
                   "and there must be room left over for real tasks");

    /* The release pairs with the acquire in sched_ready_for_aps(): an
     * application processor must not see the permission to join before it
     * can see the idle task that was built for it. */
    __atomic_store_n(&aps_may_join, true, __ATOMIC_RELEASE);

    if (joined)
        pr_info("scheduler ready; %u processors, each with its own idle task",
                joined + 1);
    else
        pr_info("scheduler ready; boot context adopted as pid 0 (idle)");
}

/* ---- kernel stacks, with guard pages -----------------------------------
 *
 * Each task's stack lives at a fixed slot in the KSTACK_BASE region rather
 * than in the heap, and each slot begins with a page that is deliberately
 * left unmapped. An overflowing stack therefore walks into nothing and takes
 * a page fault - with the offending EIP still in the trap frame - instead of
 * quietly overwriting whatever the heap put below it.
 *
 * The slot index is the task's index in `tasks[]`, which is unique and stable
 * for the life of the slot, so there is no separate allocator to get wrong.
 * Slot 0 is never used: the idle task runs on the boot stack.
 *
 *   KSTACK_BASE + i*KSTACK_SLOT                  guard page (unmapped)
 *   KSTACK_BASE + i*KSTACK_SLOT + PAGE_SIZE      lowest usable word
 *   ... + PAGE_SIZE + TASK_STACK_SIZE            esp0, the top
 *
 * Slot i's guard page sits immediately above slot i-1's top, so a stack that
 * grows the wrong way is caught as well.
 */
#define KSTACK_SLOT  (PAGE_SIZE + TASK_STACK_SIZE)
#define KSTACK_SLOTS (KSTACK_REGION / KSTACK_SLOT)

_Static_assert(KSTACK_SLOTS >= TASK_MAX,
               "the kernel-stack region cannot hold TASK_MAX stacks");

/* The lowest usable word of a stack holds this, and switch_to() checks it.
 * The guard page catches a stack that grew too far; the canary catches a wild
 * write that landed past it - a memcpy with a bad length, say, which skips
 * over the guard entirely. */
#define KSTACK_CANARY 0x5452414Bu /* "KART", backwards, in a hex dump */

static u32 canary_failures;

static vaddr_t kstack_base_of(size_t slot)
{
    return KSTACK_BASE + slot * KSTACK_SLOT + PAGE_SIZE;
}

/* Map a task's stack. Returns its top (what esp0 wants), or 0. */
static u32 kstack_alloc(size_t slot)
{
    ASSERT(slot > 0 && slot < TASK_MAX);

    vaddr_t base = kstack_base_of(slot);

    for (u32 off = 0; off < TASK_STACK_SIZE; off += PAGE_SIZE) {
        if (!vmm_alloc_at(base + off, PTE_PRESENT | PTE_WRITE)) {
            pr_err("cannot map a kernel stack for slot %u", (unsigned)slot);
            while (off > 0) {
                off -= PAGE_SIZE;
                vmm_unmap(base + off);
            }
            return 0;
        }
    }

    memset((void *)base, 0, TASK_STACK_SIZE);
    *(volatile u32 *)base = KSTACK_CANARY;

    return base + TASK_STACK_SIZE;
}

static void kstack_free(size_t slot)
{
    vaddr_t base = kstack_base_of(slot);

    for (u32 off = 0; off < TASK_STACK_SIZE; off += PAGE_SIZE)
        vmm_unmap(base + off);
}

/* Cheap enough to run on every context switch: one load and one compare. */
static void kstack_check(const struct task *t)
{
    if (!t->stack_base)
        return; /* the idle task, on the boot stack */

    if (*(const volatile u32 *)t->stack_base == KSTACK_CANARY)
        return;

    canary_failures++;
    panic("pid %u \"%s\" overran its kernel stack: the canary at %p reads "
          "%08x, not %08x",
          t->pid, t->name, t->stack_base, *(const u32 *)t->stack_base,
          KSTACK_CANARY);
}

u32 sched_canary_failures(void)
{
    return canary_failures;
}

/* Take the scheduler's lock with interrupts masked, and give them back
 * exactly as they were.
 *
 * Every acquisition goes through these. The explicit mask matters: a
 * spinlock release restores the interrupt state the *lock* recorded, and the
 * switch path releases the lock and then swaps stacks. Masking first means
 * the release is a no-op for the interrupt flag and the switch cannot be
 * interrupted halfway through. */
static bool sched_enter(void)
{
    bool irqs = irq_save();

    spin_lock(&sched_lock);
    return irqs;
}

static void sched_leave(bool irqs)
{
    spin_unlock(&sched_lock);
    irq_restore(irqs);
}

/* The first SMP_MAX_CPUS slots are reserved for idle tasks, one per
 * processor, whether or not that processor exists. Reserving them
 * unconditionally rather than only for the processors that turned up keeps
 * the slot index equal to the processor index, which is the thing that makes
 * the mapping impossible to get wrong. */
static struct task *alloc_slot_locked(void)
{
    for (size_t i = SMP_MAX_CPUS; i < TASK_MAX; i++)
        if (tasks[i].state == TASK_UNUSED)
            return &tasks[i];
    return NULL;
}

struct task *task_create(const char *name, task_entry_t entry, void *arg)
{
    if (!sched_ready)
        panic("task_create(\"%s\") before sched_init()", name);

    bool irqs = sched_enter();
    struct task *t = alloc_slot_locked();

    if (!t) {
        sched_leave(irqs);
        pr_err("cannot create \"%s\": all %u task slots in use", name,
               (unsigned)TASK_MAX);
        return NULL;
    }

    /* Claim the slot before dropping the lock so a concurrent create cannot
     * hand out the same one. */
    t->state = TASK_BLOCKED;

    struct task *parent = smp_this_cpu()->current;

    sched_leave(irqs);

    u32 esp0 = kstack_alloc((size_t)(t - tasks));
    if (!esp0) {
        t->state = TASK_UNUSED;
        pr_err("cannot create \"%s\": no memory for a %u KiB stack", name,
               (unsigned)(TASK_STACK_SIZE / KIB));
        return NULL;
    }

    void *stack = (void *)(esp0 - TASK_STACK_SIZE);

    memset(t->name, 0, sizeof(t->name));
    strlcpy(t->name, name, sizeof(t->name));
    t->stack_base = stack;
    t->stack_size = TASK_STACK_SIZE;
    t->kernel_esp0 = esp0;
    t->quantum_left = SCHED_QUANTUM;
    t->ticks_total = 0;
    t->switches = 0;
    t->wake_at = 0;
    t->exit_code = 0;
    t->user = false;
    t->resources_freed = false;
    t->parent_pid = parent ? parent->pid : 0;
    /* An empty table and a working directory of "/". A kernel thread does not
     * get descriptors 0-2 opened for it: it has kprintf, and giving it a
     * console descriptor it never uses would hold an open-file slot for the
     * life of the kernel. usermode_task() opens them for a process that is
     * about to enter ring 3, where there is no kprintf. */
    fd_table_init(&t->files);
    /* A kernel thread shares the kernel's address space; there is nothing
     * private for it to see. */
    t->page_dir = vmm_kernel_root_phys();

    /* Build the stack so that context_switch()'s epilogue - pop ebp/edi/esi/
     * ebx, popfd, ret - delivers control to thread_trampoline with `entry`
     * and `arg` sitting where a normal cdecl call would have left them.
     *
     *   sp[0..3] ebp edi esi ebx   (popped)
     *   sp[4]    eflags            (popfd; IF *clear* - see below)
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
    /* Interrupts off, not on. A new task arrives through
     * thread_trampoline(), whose first job is sched_handoff() - releasing the
     * task this processor just left - and that has to happen before anything
     * can interrupt and switch again. The trampoline sets IF itself,
     * immediately afterwards. */
    *--sp = 0x002; /* reserved bit 1; IF clear */
    *--sp = 0;     /* ebx */
    *--sp = 0;     /* esi */
    *--sp = 0;     /* edi */
    *--sp = 0;     /* ebp - zero terminates a backtrace cleanly */

    t->saved_esp = (u32)sp;

    irqs = sched_enter();
    t->pid = next_pid++;
    t->cpu = smp_cpu_index();
    runqueue_insert_locked(t);
    t->state = TASK_READY;
    sched_leave(irqs);

    /* Somebody may be idle. Telling them is cheaper than waiting for their
     * next tick to notice. */
    smp_reschedule_others();

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
    bool irqs = sched_enter();

    /* The claim and the check are in the same acquisition, which is what
     * makes two idle processors reaping at once harmless: the second one
     * finds the flag already set and returns. */
    if (t->resources_freed || running_somewhere_locked(t)) {
        sched_leave(irqs);
        return;
    }

    /* Unlink from the ring so pick_next_locked() stops walking through it.
     * The cursor may *be* this task, in which case it has to move first - a
     * cursor pointing at freed memory is a walk into nothing on the next
     * selection. */
    if (rr_cursor == t)
        rr_cursor = (t->next == t) ? NULL : t->next;

    if (rr_cursor) {
        struct task *p = rr_cursor;

        while (p->next != t && p->next != rr_cursor)
            p = p->next;
        if (p->next == t)
            p->next = t->next;
    }

    bool had_stack = t->stack_base != NULL;
    size_t slot = (size_t)(t - tasks);
    paddr_t pd = t->page_dir;
    u32 pid = t->pid;

    t->stack_base = NULL;
    t->next = NULL;
    t->resources_freed = true;
    /* Keep page_dir readable for diagnostics but make clear it is gone. */
    t->page_dir = 0;

    sched_leave(irqs);

    if (had_stack)
        kstack_free(slot);

    /* A forked process owns its address space; a kernel thread shares the
     * kernel's and must not free it. */
    if (pd && pd != vmm_kernel_root_phys())
        vmm_destroy_address_space(pd);

    pr_debug("released pid %u's stack and address space", pid);
}

/* Sweep every zombie that nobody has collected yet. Called from the idle
 * task, so a process whose parent never calls wait() still has its memory
 * returned - only the slot lingers. */
/* ---- the run queue ------------------------------------------------------
 *
 * One queue for the whole machine, not one per processor.
 *
 * That is a choice and it is worth being explicit about, because "per-CPU run
 * queues" is the usual phrase. A queue per processor buys two things: no
 * contention on a shared lock, and cache locality from a task tending to
 * return to the processor whose cache holds its data. It costs a balancer -
 * code that moves tasks between queues, decides when to, and is where every
 * scheduler that has one keeps its hardest bugs.
 *
 * This kernel has thirty-two task slots and at most eight processors, and a
 * selection under the lock is a walk of a short list. The contention a shared
 * queue would have to show before a balancer earned its complexity is not
 * reachable at this size. So: one queue, one lock, no affinity, and the
 * processor a task last ran on recorded and reported rather than respected.
 * docs/SMP.md says when that stops being the right answer.
 *
 * Idle tasks are not in the ring at all. They are not alternatives to a real
 * task - each belongs to exactly one processor - so the fallback is explicit
 * rather than a last entry that must never be chosen.
 */

/* Splice a task into the ring. Called with the lock held. */
static void runqueue_insert_locked(struct task *t)
{
    if (!rr_cursor) {
        t->next = t;
        rr_cursor = t;
        return;
    }

    t->next = rr_cursor->next;
    rr_cursor->next = t;
}

/* True if any processor is currently running this task. Called with the lock
 * held, which is what makes the answer mean anything: a task that is RUNNING
 * is somebody's `current` or somebody's `prev` whose stack pointer is still
 * being written. */
static bool running_somewhere_locked(const struct task *t)
{
    for (u32 i = 0; i < SMP_MAX_CPUS; i++) {
        const struct cpu *c = smp_cpu(i);

        if (!c || !c->online)
            continue;
        if (c->current == t || c->prev == t)
            return true;
    }

    return false;
}

/* Find a zombie whose resources can be released, and claim it. Called with
 * the lock held; the freeing itself happens outside, because it unmaps pages
 * and frees frames and there is no reason to hold up the whole machine's
 * scheduling while it does. */
static struct task *claim_zombie_locked(void)
{
    for (size_t i = SMP_MAX_CPUS; i < TASK_MAX; i++) {
        struct task *t = &tasks[i];

        if (t->state != TASK_ZOMBIE || t->resources_freed)
            continue;
        if (running_somewhere_locked(t))
            continue;

        /* A candidate, not a claim. release_task_resources() takes the lock
         * again and does the claiming there, so two idle processors that
         * both pick this task end with one of them doing the work. */
        return t;
    }

    return NULL;
}

static void reap_zombies(void)
{
    /* Bounded by the table size rather than "until none are left": a zombie
     * that is still some processor's `prev` cannot be freed yet and would
     * otherwise be picked again on every pass. It will be gone next time
     * round the idle loop. */
    for (size_t pass = SMP_MAX_CPUS; pass < TASK_MAX; pass++) {
        bool irqs = sched_enter();
        struct task *t = claim_zombie_locked();

        sched_leave(irqs);

        if (!t)
            return;

        release_task_resources(t);
    }
}

/* Move sleepers whose deadline has passed back to READY. Called with the
 * lock held. */
static void wake_sleepers_locked(void)
{
    u64 now = timer_ticks();

    for (size_t i = SMP_MAX_CPUS; i < TASK_MAX; i++)
        if (tasks[i].state == TASK_SLEEPING && now >= tasks[i].wake_at)
            tasks[i].state = TASK_READY;
}

/* Round robin: walk the ring from wherever the cursor left off and take the
 * first thing that can run. Called with the lock held.
 *
 * The cursor is global, so two processors picking at the same instant get
 * different tasks rather than fighting over the head of the queue - and a
 * task is marked RUNNING here, inside the lock, which is what stops the
 * second processor choosing what the first has just taken.
 *
 * This processor's idle task is the fallback, never a choice.
 */
static struct task *pick_next_locked(struct cpu *cpu)
{
    struct task *start = rr_cursor;

    if (start) {
        struct task *t = start;

        do {
            t = t->next;
            if (t->state == TASK_READY) {
                rr_cursor = t;
                return t;
            }
        } while (t != start);
    }

    /* Nobody else wants the processor. Carry on with what it was doing, if
     * that is still a runnable thing to do. */
    if (cpu->current != cpu->idle && cpu->current->state == TASK_RUNNING)
        return cpu->current;

    return cpu->idle;
}

/* Perform a switch. Called with the lock held and interrupts masked; returns
 * with the lock released, and - for the task that resumes here later - after
 * sched_handoff() has tidied up behind it.
 *
 * The ordering is the whole of the design:
 *
 *   1. `next` is marked RUNNING *inside* the lock, so no other processor can
 *      pick it between here and the stack swap.
 *   2. `prev` is left marked RUNNING, and recorded as this processor's
 *      `prev`. It is therefore unpickable too - which it has to be, because
 *      its saved stack pointer has not been written yet.
 *   3. The lock is released. Interrupts stay masked, because the caller
 *      masked them before taking it.
 *   4. The stacks are swapped.
 *   5. Whoever resumes calls sched_handoff(), which marks the processor's
 *      `prev` READY now that its context is safely stored.
 *
 * Step 2 is the step that is easy to miss. Marking `prev` READY before the
 * switch is the obvious thing to write and it is a corruption bug on a
 * machine with two processors: the other one picks the task up and starts
 * restoring a context that this one has not finished saving.
 */
static void do_switch(struct cpu *cpu, struct task *next)
{
    struct task *prev = cpu->current;

    if (next == prev) {
        prev->quantum_left = SCHED_QUANTUM;
        spin_unlock(&sched_lock);
        return;
    }

    /* Before anything else: the task being switched away from is the one
     * that has just been running, so its stack is the one that may have been
     * overrun. Checking here means the panic names the culprit. */
    kstack_check(prev);

    next->state = TASK_RUNNING;
    next->quantum_left = SCHED_QUANTUM;
    next->switches++;
    next->cpu = cpu->index;

    cpu->current = next;
    cpu->prev = prev;
    cpu->switches++;
    switch_count++;

    /* Tell the CPU which stack to land on if an interrupt arrives while this
     * task is in ring 3. Wrong value here means a ring-3 interrupt corrupts
     * some other task's stack. */
    tss_set_kernel_stack(next->kernel_esp0);

    /* Switch address spaces. Safe to do before the stack swap because every
     * task's kernel stack lives in the kernel's gigabyte, which is one
     * shared page directory in every address space - that sharing is the
     * whole reason vmm_create_address_space() points at the kernel's
     * directory instead of copying it. */
    if (next->page_dir && next->page_dir != prev->page_dir)
        vmm_switch_address_space(next->page_dir);

    spin_unlock(&sched_lock);

    context_switch(&prev->saved_esp, next->saved_esp);

    /* Resumed - possibly on a different processor than the one that parked
     * us, which is why sched_handoff() asks who it is running on now. */
    sched_handoff();
}

void sched_handoff(void)
{
    struct cpu *cpu = smp_this_cpu();
    struct task *prev = cpu->prev;

    if (!prev)
        return;

    /* Interrupts are masked here - do_switch()'s caller masked them, and the
     * two trampolines arrive with IF clear for exactly this reason - so
     * nothing can preempt between the read and the clear. */
    cpu->prev = NULL;

    spin_lock(&sched_lock);
    /* Still RUNNING means it was merely preempted and is runnable again.
     * SLEEPING, BLOCKED or ZOMBIE means it parked itself deliberately and
     * the state it chose is the one that must survive. */
    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    spin_unlock(&sched_lock);
}

/* Give up the processor.
 *
 * `park` is the state to leave the calling task in: TASK_RUNNING means "just
 * yield, you are still runnable", and anything else means the task is parking
 * itself and must not be picked up again until something changes it back.
 *
 * Setting that state and switching away have to happen under one acquisition
 * of the lock. Doing it in two - as this did when there was one processor -
 * leaves a window in which the task is marked SLEEPING while it is still
 * running, and another processor's wake_sleepers_locked() can make it READY
 * and run it on two processors at once.
 */
static void sched_switch_away(enum task_state park, u64 wake_at)
{
    bool irqs = irq_save();
    struct cpu *cpu = smp_this_cpu();

    spin_lock(&sched_lock);

    if (park != TASK_RUNNING) {
        cpu->current->state = park;
        cpu->current->wake_at = wake_at;
    }

    cpu->need_resched = false;
    wake_sleepers_locked();

    do_switch(cpu, pick_next_locked(cpu)); /* releases the lock */

    irq_restore(irqs);
}

void sched_tick(void)
{
    if (!sched_ready)
        return;

    struct cpu *cpu = smp_this_cpu();
    struct task *cur = cpu->current;

    if (!cur)
        return;

    cur->ticks_total++;

    if (cur->quantum_left > 0)
        cur->quantum_left--;

    /* Idle gives up the processor the instant anything else is runnable; a
     * real task keeps it until its quantum expires.
     *
     * Waking sleepers is *not* done here, although it used to be. It mutates
     * tasks[] and so needs the lock, and taking a lock in the timer handler
     * on every tick on every processor to do work that only matters if a
     * switch follows is the wrong trade. It happens in
     * sched_switch_away() instead, under the acquisition that is already
     * being made. An idle processor sets need_resched every tick, so as long
     * as one processor is idle a sleeper wakes within a tick; if every
     * processor is busy it waits out a quantum, which is the same answer a
     * busy machine gives to everything else. */
    if (cur->quantum_left == 0 || cur == cpu->idle)
        cpu->need_resched = true;
}

void sched_preempt(void)
{
    if (!sched_ready)
        return;

    struct cpu *cpu = smp_this_cpu();

    if (!cpu->need_resched)
        return;

    sched_switch_away(TASK_RUNNING, 0);
}

void sched_yield(void)
{
    if (!sched_ready) {
        /* Before the scheduler exists, the only sensible "wait" is to let an
         * interrupt happen. */
        __asm__ volatile("sti; hlt");
        return;
    }

    sched_switch_away(TASK_RUNNING, 0);
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

    sched_switch_away(TASK_SLEEPING, timer_ticks() + ticks_to_wait);
}

void task_block(void)
{
    if (!sched_ready)
        return;

    sched_switch_away(TASK_BLOCKED, 0);
}

void task_unblock(struct task *t)
{
    if (!t)
        return;

    bool irqs = sched_enter();
    bool woke = t->state == TASK_BLOCKED;

    if (woke)
        t->state = TASK_READY;

    sched_leave(irqs);

    /* A task has become runnable and this processor is busy with something
     * else. Prod the others: one of them may be idle, and an idle processor
     * that is not told waits for its own next tick to find out. */
    if (woke)
        smp_reschedule_others();
}

NORETURN void task_exit(int code)
{
    struct cpu *cpu = smp_this_cpu();

    if (!sched_ready || cpu->current == cpu->idle)
        panic("task_exit(%d) called from the idle task", code);

    struct task *me = cpu->current;

    pr_debug("pid %u \"%s\" exited with %d", me->pid, me->name, code);

    /* Descriptors go first, and they go here rather than in the reaper.
     * Closing one can run backend code - freeing a FAT handle, sending a FIN
     * on a socket - and that code is entitled to assume it is running as the
     * process that owns the descriptor. The reaper runs as an idle task, in
     * a different address space, so a close from there would be doing that
     * work on behalf of a process that no longer exists.
     *
     * Interrupts are still on at this point, deliberately: a close takes the
     * filesystem's lock and may touch a device, and doing it inside the
     * IRQ-off window below would extend that window by however long a flush
     * takes. It is also why the page-fault handler drops the VMM lock before
     * it reaches here - closing a descriptor must be free to take locks of
     * its own. */
    fd_table_close_all(&me->files);

    bool irqs = irq_save();

    spin_lock(&sched_lock);

    me->exit_code = code;
    me->state = TASK_ZOMBIE;
    cpu->need_resched = false;

    /* If the parent is blocked in wait(), it is waiting for exactly this. */
    for (size_t i = SMP_MAX_CPUS; i < TASK_MAX; i++) {
        if (tasks[i].pid == me->parent_pid && tasks[i].state == TASK_BLOCKED) {
            tasks[i].state = TASK_READY;
            break;
        }
    }

    /* pick_next_locked() will not return a zombie, so this always moves us
     * off the dying task. Its stack stays allocated until an idle processor
     * reaps it. */
    do_switch(cpu, pick_next_locked(cpu)); /* releases the lock */

    irq_restore(irqs);

    panic("a zombie task was scheduled again (pid %u)", me->pid);
}

/* ------------------------------------------------------------------------- */
/* Processes                                                                 */
/* ------------------------------------------------------------------------- */

int task_fork(const struct regs *parent_frame)
{
    if (!sched_ready || !parent_frame)
        return -1;

    bool irqs = sched_enter();
    struct task *child = alloc_slot_locked();

    if (!child) {
        sched_leave(irqs);
        pr_err("fork: all %u task slots are in use", (unsigned)TASK_MAX);
        return -1;
    }

    child->state = TASK_BLOCKED; /* claim the slot before unlocking */

    struct task *parent = smp_this_cpu()->current;

    sched_leave(irqs);

    u32 esp0 = kstack_alloc((size_t)(child - tasks));
    if (!esp0) {
        child->state = TASK_UNUSED;
        pr_err("fork: no memory for the child's kernel stack");
        return -1;
    }

    void *stack = (void *)(esp0 - TASK_STACK_SIZE);

    /* Copy-on-write clone of the caller's address space. Every writable page
     * becomes read-only in *both* and the frames are shared until written. */
    paddr_t child_pd = vmm_clone_current();
    if (!child_pd) {
        kstack_free((size_t)(child - tasks));
        child->state = TASK_UNUSED;
        pr_err("fork: could not clone the address space");
        return -1;
    }

    memset(child->name, 0, sizeof(child->name));
    strlcpy(child->name, parent->name, sizeof(child->name));

    child->stack_base = stack;
    child->stack_size = TASK_STACK_SIZE;
    child->kernel_esp0 = esp0;
    child->page_dir = child_pd;
    child->parent_pid = parent->pid;
    child->quantum_left = SCHED_QUANTUM;
    child->ticks_total = 0;
    child->switches = 0;
    child->wake_at = 0;
    child->exit_code = 0;
    child->user = parent->user;
    child->resources_freed = false;

    /* The child's table points at the parent's open files, one new reference
     * each. That sharing is the whole behaviour of fork over descriptors: the
     * file *position* is shared, so a parent and child appending to the same
     * descriptor advance one offset between them instead of overwriting each
     * other. Copying the open files instead would be easier and wrong. */
    fd_table_clone(&child->files, &parent->files);

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
    u8 *top = (u8 *)esp0;
    struct regs *frame = (struct regs *)(top - sizeof(struct regs));

    *frame = *parent_frame;

    /* The one difference between parent and child, and the whole trick:
     * fork returns the child's pid in the parent and 0 in the child.
     * isr_restore_and_return's `popa` picks this up. */
    frame->eax = 0;

    u32 *sp = (u32 *)frame;
    *--sp = (u32)frame;
    *--sp = (u32)fork_trampoline;
    /* IF clear: fork_trampoline() calls sched_handoff() before it restores
     * the child's trap frame, and the iret out of that frame sets IF from
     * the frame's own EFLAGS - which is the parent's, with interrupts on. */
    *--sp = 0x002; /* reserved bit 1; IF clear */
    *--sp = 0;     /* ebx */
    *--sp = 0;     /* esi */
    *--sp = 0;     /* edi */
    *--sp = 0;     /* ebp */

    child->saved_esp = (u32)sp;

    irqs = sched_enter();
    child->pid = next_pid++;
    child->cpu = smp_cpu_index();
    runqueue_insert_locked(child);
    child->state = TASK_READY;
    sched_leave(irqs);

    smp_reschedule_others();

    pr_info("fork: pid %u -> pid %u, address space %p", parent->pid, child->pid,
            (void *)child_pd);

    return (int)child->pid;
}

int task_wait(int *status_out)
{
    if (!sched_ready)
        return -1;

    for (;;) {
        bool irqs = sched_enter();
        bool have_children = false;
        struct task *dead = NULL;
        int pid = -1;
        struct task *me = smp_this_cpu()->current;

        for (size_t i = SMP_MAX_CPUS; i < TASK_MAX; i++) {
            struct task *t = &tasks[i];

            if (t->state == TASK_UNUSED || t->parent_pid != me->pid)
                continue;

            if (t->state == TASK_ZOMBIE) {
                pid = (int)t->pid;

                if (status_out)
                    *status_out = t->exit_code;

                /* Collecting the exit code is what finally frees the slot,
                 * and doing it here, inside the lock, is what stops two
                 * parents - or a parent and the reaper - claiming the same
                 * child. The *resources* are released outside, because that
                 * unmaps pages. */
                dead = t;
                t->state = TASK_UNUSED;
                t->parent_pid = 0;
                break;
            }

            have_children = true;
        }

        if (dead) {
            sched_leave(irqs);

            /* The reaper may not have got to this child yet, and the slot
             * must not go back into circulation while it still holds a stack
             * and a page directory. Releasing here rather than waiting for
             * idle is also what makes wait() the point at which a child's
             * memory is definitely gone. Idempotent, so racing the reaper is
             * harmless. */
            release_task_resources(dead);
            return pid;
        }

        if (!have_children) {
            sched_leave(irqs);
            return -1;
        }

        /* Block rather than spin. task_exit() moves us back to READY. The
         * state change and the switch happen under one acquisition, which is
         * why this cannot simply set BLOCKED and call sched_yield(). */
        me->state = TASK_BLOCKED;
        spin_unlock(&sched_lock);
        irq_restore(irqs);

        sched_yield();
    }
}

u32 task_count(void)
{
    u32 n = 0;
    bool irqs = sched_enter();

    for (size_t i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != TASK_UNUSED)
            n++;

    sched_leave(irqs);
    return n;
}

NORETURN void sched_start(void)
{
    /* This is an idle task's body, and every processor runs its own copy of
     * it. Reaping happens here because an idle task is the one context
     * guaranteed not to be standing on the stack it frees - and because an
     * idle processor is by definition the one with time to do it. */
    struct cpu *cpu = smp_this_cpu();

    for (;;) {
        cpu->idle_loops++;
        reap_zombies();

        /* STI then HLT, in that order and with nothing between them: the
         * processor guarantees that an interrupt pending at the STI is taken
         * after the HLT is entered, so a wakeup cannot be lost in the gap.
         * Written the other way round, a task made runnable between the two
         * instructions would wait for the next tick. */
        __asm__ volatile("sti; hlt");
    }
}

void sched_foreach(void (*fn)(const struct task *t, void *ctx), void *ctx)
{
    /* The callback runs under the scheduler's lock, so that a `ps` cannot
     * print half a task table that another processor is changing underneath
     * it. That means the callback must not block or take a lock below level
     * 1 - which is every lock - so in practice it may only read and print.
     * Printing is fine: the console is level 7. */
    bool irqs = sched_enter();

    for (size_t i = 0; i < TASK_MAX; i++)
        if (tasks[i].state != TASK_UNUSED)
            fn(&tasks[i], ctx);

    sched_leave(irqs);
}
