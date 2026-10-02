/* StratumOS - 8253/8254 programmable interval timer.
 *
 * Channel 0 in rate-generator mode gives us a steady tick. Everything that
 * needs to measure time - uptime, sleeps, the scheduler's quantum - counts
 * these. The divisor arithmetic is done with rounding, and the resulting real
 * frequency is reported back, because 1193182 / 100 is not an integer and
 * pretending otherwise makes clocks drift.
 */
#ifndef _DRIVERS_TIMER_H
#define _DRIVERS_TIMER_H

#include <kernel/types.h>

#define PIT_BASE_HZ 1193182u
#define PIT_CH0     0x40
#define PIT_CMD     0x43

#define TIMER_HZ 100u

void timer_init(u32 hz);
u64  timer_ticks(void);
u32  timer_hz(void);
u32  timer_actual_hz_milli(void); /* real frequency * 1000, for reporting */
u64  timer_ms(void);
u32  timer_uptime_seconds(void);
void timer_busy_wait_ms(u32 ms);

#endif /* _DRIVERS_TIMER_H */
