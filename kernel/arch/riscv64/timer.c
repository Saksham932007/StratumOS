/* StratumOS - the CLINT timer.
 *
 * RISC-V's timer is not a programmable interval timer. There is one
 * monotonically increasing 64-bit counter (`mtime`) and one comparator per
 * hart (`mtimecmp`); an interrupt is pending for as long as mtime >=
 * mtimecmp. So there is no "periodic mode" to set: the handler has to push
 * the comparator forward every time, and forgetting to makes the interrupt
 * re-fire immediately and the machine lock up.
 *
 * That is a genuine design difference from the 8254, which is a divisor and
 * a mode byte and then ticks on its own. It is also simpler to reason about -
 * mtime is a real clock, where the x86 side has to count ticks and multiply.
 */
#include <arch/riscv64/riscv.h>

#define TICK_HZ       100 /* the same rate the x86 kernel's PIT runs at */

#define TICK_INTERVAL (VIRT_TIMEBASE_HZ / TICK_HZ)

static volatile u64 ticks;

u64 timer_now(void)
{
    return mmio_read64(VIRT_CLINT_MTIME);
}

/* Arm the comparator for one interval from *the last deadline*, not from
 * now. Advancing from now lets the period drift by however long the handler
 * took; advancing the deadline keeps the average exact, which is the same
 * reason the x86 PIT reloads from a divisor rather than being re-armed. */
static void arm_next(u64 hart)
{
    u64 cmp_addr = VIRT_CLINT_MTIMECMP + hart * 8;
    u64 deadline = mmio_read64(cmp_addr) + TICK_INTERVAL;
    u64 now = timer_now();

    /* Unless the deadline is already in the past, which means the machine
     * was stopped - under an emulator, or a debugger - for longer than an
     * interval. Catching up tick by tick would then spend a long time doing
     * nothing else, so the deadline is reset from now and the lost time is
     * simply lost. */
    if (deadline <= now)
        deadline = now + TICK_INTERVAL;

    mmio_write64(cmp_addr, deadline);
}

void timer_init_machine(void)
{
    u64 hart = csr_read(mhartid);

    mmio_write64(VIRT_CLINT_MTIMECMP + hart * 8, timer_now() + TICK_INTERVAL);

    csr_set(mie, MIE_MTIE);
    csr_set(mstatus, MSTATUS_MIE);
}

/* Called from the machine-mode trap handler. */
void timer_tick(void)
{
    ticks++;
    arm_next(csr_read(mhartid));
}

u64 timer_ticks(void)
{
    return ticks;
}

/* The second and last symbol the shared code needs from this architecture.
 *
 * kernel/core/log.c compiles unmodified and its only undefined references
 * are console_putc - which comes from the UART - and this. So the whole
 * levelled logger, its rate limiter and its expected-error windows came
 * across for the cost of two functions.
 *
 * Derived from mtime rather than from the tick counter, which is the better
 * answer and only available here: mtime is a real monotonic clock at a known
 * frequency, so this is exact and keeps working if the tick rate changes.
 * The x86 implementation has to count PIT interrupts and multiply, because
 * the 8254 has no equivalent. */
u64 timer_ms(void)
{
    return timer_now() / (VIRT_TIMEBASE_HZ / 1000);
}
