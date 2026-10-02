/* StratumOS - MC146818 real-time clock / CMOS.
 *
 * Reading the RTC correctly means waiting out the update-in-progress flag and
 * then reading twice to make sure the values did not change under us, because
 * the chip's registers are not latched. BCD and 12-hour modes are both
 * handled, as signalled by status register B.
 */
#ifndef _DRIVERS_RTC_H
#define _DRIVERS_RTC_H

#include <kernel/types.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

struct rtc_time {
    u16 year;
    u8 month, day, hour, minute, second;
};

void rtc_init(void);
void rtc_read(struct rtc_time *out);
/* Days since 1970-01-01 converted to a Unix timestamp (UTC assumed). */
u64 rtc_unix_time(void);

#endif /* _DRIVERS_RTC_H */
