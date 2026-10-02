/* StratumOS - MC146818 real-time clock / CMOS.
 *
 * Reading the RTC naively gives wrong answers roughly once a second. Its
 * registers are not latched, so a read that straddles the chip's internal
 * update can return 10:59:60 or 11:00:00's minute with 10's hour. The correct
 * procedure, and the one used here, is:
 *
 *   1. Wait for the update-in-progress flag to clear.
 *   2. Read every field.
 *   3. Read them all again and compare; if anything moved, start over.
 *
 * Status register B then says whether the values are BCD and whether the hour
 * is in 12-hour form with a PM bit - neither of which can be assumed.
 */
#define LOG_TAG "rtc"

#include <arch/io.h>
#include <drivers/rtc.h>
#include <kernel/log.h>
#include <kernel/string.h>

#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS   0x04
#define RTC_DAY     0x07
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09
#define RTC_CENTURY 0x32
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

#define STATUS_A_UPDATE_IN_PROGRESS 0x80
#define STATUS_B_24_HOUR            0x02
#define STATUS_B_BINARY             0x04

static bool have_century;

static u8 cmos_read(u8 reg)
{
    /* Bit 7 of the index port disables NMI; preserving it is polite, but the
     * conventional thing is to leave NMI masked only briefly. */
    outb(CMOS_ADDR, reg);
    io_wait();
    return inb(CMOS_DATA);
}

static bool update_in_progress(void)
{
    return (cmos_read(RTC_STATUS_A) & STATUS_A_UPDATE_IN_PROGRESS) != 0;
}

static u8 from_bcd(u8 v)
{
    return (u8)((v & 0x0F) + ((v >> 4) * 10));
}

void rtc_init(void)
{
    struct rtc_time t;

    /* A century register exists on most chips but is not architectural; a
     * reading outside a plausible range means the register is not there. */
    u8 century = cmos_read(RTC_CENTURY);
    u8 status_b = cmos_read(RTC_STATUS_B);
    u8 raw = (status_b & STATUS_B_BINARY) ? century : from_bcd(century);

    have_century = (raw >= 19 && raw <= 21);

    rtc_read(&t);
    pr_info("RTC reports %04u-%02u-%02u %02u:%02u:%02u UTC (%s, %s hour%s)",
            t.year, t.month, t.day, t.hour, t.minute, t.second,
            (status_b & STATUS_B_BINARY) ? "binary" : "BCD",
            (status_b & STATUS_B_24_HOUR) ? "24" : "12",
            have_century ? ", century register present" : "");
}

void rtc_read(struct rtc_time *out)
{
    struct rtc_time a, b;
    u8 status_b;
    unsigned attempts = 0;

    if (!out)
        return;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    do {
        /* Bounded: a dead or absent RTC must not spin forever. */
        if (++attempts > 16)
            break;

        for (unsigned spin = 0; spin < 100000 && update_in_progress(); spin++)
            ;

        a.second = cmos_read(RTC_SECONDS);
        a.minute = cmos_read(RTC_MINUTES);
        a.hour   = cmos_read(RTC_HOURS);
        a.day    = cmos_read(RTC_DAY);
        a.month  = cmos_read(RTC_MONTH);
        a.year   = cmos_read(RTC_YEAR);
        u8 cent_a = have_century ? cmos_read(RTC_CENTURY) : 0;

        for (unsigned spin = 0; spin < 100000 && update_in_progress(); spin++)
            ;

        b.second = cmos_read(RTC_SECONDS);
        b.minute = cmos_read(RTC_MINUTES);
        b.hour   = cmos_read(RTC_HOURS);
        b.day    = cmos_read(RTC_DAY);
        b.month  = cmos_read(RTC_MONTH);
        b.year   = cmos_read(RTC_YEAR);
        u8 cent_b = have_century ? cmos_read(RTC_CENTURY) : 0;

        if (memcmp(&a, &b, sizeof(a)) == 0 && cent_a == cent_b) {
            a.year = (u16)(a.year | ((u16)cent_a << 8)); /* stash for below */
            break;
        }
    } while (1);

    u8 century_raw = (u8)(a.year >> 8);
    a.year &= 0xFF;

    status_b = cmos_read(RTC_STATUS_B);

    if (!(status_b & STATUS_B_BINARY)) {
        a.second = from_bcd(a.second);
        a.minute = from_bcd(a.minute);
        a.day    = from_bcd(a.day);
        a.month  = from_bcd(a.month);
        a.year   = from_bcd((u8)a.year);
        century_raw = from_bcd(century_raw);
        /* The hour's PM flag lives in bit 7 and must survive BCD conversion,
         * so it is masked off first and re-applied. */
        a.hour = (u8)(from_bcd(a.hour & 0x7F) | (a.hour & 0x80));
    }

    if (!(status_b & STATUS_B_24_HOUR) && (a.hour & 0x80)) {
        /* 12-hour mode: 12 PM stays 12, 1-11 PM add 12. */
        a.hour = (u8)(((a.hour & 0x7F) % 12) + 12);
    } else {
        a.hour &= 0x7F;
    }

    if (have_century && century_raw >= 19 && century_raw <= 21)
        out->year = (u16)(century_raw * 100 + a.year);
    else
        /* No century register: the usual convention is that a two-digit year
         * below 70 means the 2000s. */
        out->year = (u16)((a.year < 70 ? 2000 : 1900) + a.year);

    out->month  = a.month;
    out->day    = a.day;
    out->hour   = a.hour;
    out->minute = a.minute;
    out->second = a.second;
}

u64 rtc_unix_time(void)
{
    struct rtc_time t;
    rtc_read(&t);

    if (t.year < 1970)
        return 0;

    /* Days before the start of each month in a non-leap year. */
    static const u16 month_start[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334,
    };

    u32 year = t.year;
    u64 days = 0;

    for (u32 y = 1970; y < year; y++)
        days += ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 366 : 365;

    u32 month = (t.month >= 1 && t.month <= 12) ? t.month : 1;
    days += month_start[month - 1];

    /* This year's leap day only counts once we are past February. */
    if (month > 2 && (((year % 4 == 0) && (year % 100 != 0)) || year % 400 == 0))
        days += 1;

    days += (t.day ? t.day - 1u : 0u);

    return days * 86400ull + t.hour * 3600ull + t.minute * 60ull + t.second;
}
