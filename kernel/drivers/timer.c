/* StratumOS - 8253/8254 programmable interval timer.
 *
 * The PIT counts down from a programmed divisor at a fixed 1193182 Hz and
 * raises IRQ 0 each time it wraps. Mode 3 (square wave) is the conventional
 * choice for a periodic tick.
 *
 * The divisor arithmetic is rounded, not truncated, and the *actual* resulting
 * frequency is computed and reported. 1193182/100 is 11931.82, so a kernel
 * that assumes it programmed exactly 100 Hz has a clock that drifts by about
 * 1.5 ms per second - which shows up as sleeps that are consistently short
 * and is deeply annoying to track down later.
 */
#define LOG_TAG "timer"

#include <arch/io.h>
#include <arch/irq.h>
#include <drivers/timer.h>
#include <kernel/log.h>
#include <kernel/sched.h>

static volatile u64 ticks;
static u32 configured_hz;
static u32 divisor_used;
static u32 actual_hz_milli; /* real frequency x 1000 */

static void timer_irq(struct regs *r)
{
    UNUSED(r);
    ticks++;
    sched_tick();
}

void timer_init(u32 hz)
{
    u32 divisor;

    if (hz == 0)
        hz = TIMER_HZ;

    /* Clamp to what the hardware can express: the divisor is 16 bits, and a
     * divisor of 0 means 65536. */
    if (hz > PIT_BASE_HZ)
        hz = PIT_BASE_HZ;
    if (hz < 19)
        hz = 19; /* PIT_BASE_HZ / 65535, the slowest achievable tick */

    divisor = (PIT_BASE_HZ + hz / 2) / hz; /* round, do not truncate */
    if (divisor == 0)
        divisor = 1;
    if (divisor > 65535)
        divisor = 65535;

    configured_hz = hz;
    divisor_used = divisor;
    actual_hz_milli = (u32)(((u64)PIT_BASE_HZ * 1000) / divisor);

    ticks = 0;

    /* 0x36 = channel 0, access lobyte then hibyte, mode 3, binary. */
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0, (u8)(divisor & 0xFF));
    outb(PIT_CH0, (u8)((divisor >> 8) & 0xFF));

    irq_install_handler(IRQ_TIMER, timer_irq, "pit");

    pr_info("PIT at %u Hz requested, %u.%03u Hz actual (divisor %u)", hz,
            actual_hz_milli / 1000, actual_hz_milli % 1000, divisor);
}

u64 timer_ticks(void)
{
    return ticks;
}

u32 timer_hz(void)
{
    return configured_hz;
}

u32 timer_actual_hz_milli(void)
{
    return actual_hz_milli;
}

u64 timer_ms(void)
{
    /* Callable before timer_init() - the logger timestamps boot messages that
     * happen before the timer exists - so guard the division. */
    if (actual_hz_milli == 0)
        return 0;

    /* ticks * 1000 / hz, done as ticks * 1000000 / (hz*1000) to keep the
     * rounding honest without floating point. */
    return (ticks * 1000000ull) / actual_hz_milli;
}

u32 timer_uptime_seconds(void)
{
    return (u32)(timer_ms() / 1000);
}

void timer_busy_wait_ms(u32 ms)
{
    u64 target = timer_ms() + ms;

    /* Only usable once the timer is ticking; otherwise this would spin
     * forever. Callers that might run earlier should say so. */
    if (actual_hz_milli == 0)
        return;

    while (timer_ms() < target)
        __asm__ volatile("hlt");
}
