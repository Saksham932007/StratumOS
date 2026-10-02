/* StratumOS - tasks and the round-robin scheduler.
 *
 * Tasks are kernel threads: each gets its own stack and its own saved ESP, and
 * switching between them is a matter of swapping stacks (see
 * arch/x86/switch.asm). Preemption is driven from the timer IRQ, which simply
 * marks the current task as out of quantum; the switch itself happens on the
 * way out of the interrupt handler, where the stack is in a known state.
 */
#ifndef _KERNEL_SCHED_H
#define _KERNEL_SCHED_H

#include <arch/idt.h>

#include <kernel/types.h>

#define TASK_NAME_MAX   24
#define TASK_MAX        32
#define TASK_STACK_SIZE (16 * KIB)
#define SCHED_QUANTUM   5 /* timer ticks before a task is preempted */

enum task_state {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_BLOCKED,
    TASK_ZOMBIE,
};

typedef void (*task_entry_t)(void *arg);

struct task {
    u32 saved_esp; /* must stay first: switch.asm indexes it */
    u32 pid;
    char name[TASK_NAME_MAX];
    enum task_state state;
    u32 quantum_left;
    u32 ticks_total; /* timer ticks spent running */
    u32 switches;
    u64 wake_at; /* tick to wake a sleeper */
    void *stack_base;
    size_t stack_size;
    u32 kernel_esp0; /* ring-0 stack top for the TSS */
    int exit_code;
    bool user;
    struct task *next;
};

void sched_init(void);
struct task *task_create(const char *name, task_entry_t entry, void *arg);
NORETURN void task_exit(int code);
void sched_yield(void);
void task_sleep_ms(u32 ms);
void task_block(void);
void task_unblock(struct task *t);

/* Called from the timer IRQ: account time and decide whether to preempt. */
void sched_tick(void);
/* Called at the tail of the interrupt path: perform a pending switch. */
void sched_preempt(void);
/* Hand the CPU over for the first time; never returns. */
NORETURN void sched_start(void);

struct task *task_current(void);
u32 sched_switch_count(void);
const char *task_state_name(enum task_state s);
void sched_foreach(void (*fn)(const struct task *t, void *ctx), void *ctx);

/* Implemented in arch/x86/switch.asm */
void context_switch(u32 *save_esp, u32 load_esp);

#endif /* _KERNEL_SCHED_H */
