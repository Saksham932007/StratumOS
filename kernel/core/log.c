/* StratumOS - levelled logging.
 *
 * Every line carries a severity and a subsystem tag, which is what turns the
 * serial transcript from prose into data: tools/run-tests.py asserts on lines
 * like "[    1.234] INFO  pmm: 130812 frames usable" without having to guess
 * at wording.
 */
#include <drivers/timer.h>

#include <kernel/console.h>
#include <kernel/log.h>
#include <kernel/string.h>

static enum log_level current_level = LOG_INFO;

static const char *const level_names[] = {
    "PANIC", "ERROR", "WARN", "INFO", "DEBUG", "TRACE",
};

void log_set_level(enum log_level level)
{
    if (level > LOG_TRACE)
        level = LOG_TRACE;
    current_level = level;
}

enum log_level log_get_level(void)
{
    return current_level;
}

const char *log_level_name(enum log_level level)
{
    if (level > LOG_TRACE)
        return "?";
    return level_names[level];
}

bool log_set_level_by_name(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(level_names); i++) {
        if (strcasecmp(name, level_names[i]) == 0) {
            current_level = (enum log_level)i;
            return true;
        }
    }
    return false;
}

/* ---- expected errors --------------------------------------------------
 *
 * Several test suites work by calling things that must fail: a read past the
 * end of a disk, a lookup of a file that is not there, an ELF with a
 * corrupted header. Each of those logs an error, correctly - and CI treats an
 * ERROR line as a failure, also correctly.
 *
 * Rather than soften the messages, a test can declare a window in which
 * errors are expected. Inside it, ERROR and PANIC-level lines are counted
 * instead of printed, and the count is readable afterwards - so a test can
 * assert that the error it provoked *did* happen, which is stronger than
 * suppressing it and stronger than not provoking it at all.
 *
 * Deliberately narrow: only the two highest levels, only while a test asks
 * for it, and the count is never reset by anything but the test that opened
 * the window.
 */
static bool expecting_errors;
static u32 expected_error_count;

void log_expect_errors(bool on)
{
    expecting_errors = on;
    if (on)
        expected_error_count = 0;
}

u32 log_expected_errors(void)
{
    return expected_error_count;
}

void log_emit(enum log_level level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    u64 ms;

    if (expecting_errors && level <= LOG_ERROR) {
        expected_error_count++;
        return;
    }

    if (level > current_level)
        return;

    /* A monotonic timestamp makes it possible to see *when* in the boot a
     * message appeared, which is most of the value of a log. */
    ms = timer_ms();
    kprintf("[%5llu.%03llu] %-5s %s: ", ms / 1000, ms % 1000,
            log_level_name(level), tag);

    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);

    kprintf("\n");
}

void log_boot_step(const char *name, bool ok, const char *detail)
{
    u64 ms = timer_ms();

    kprintf("[%5llu.%03llu] %-5s boot: %-18s [%s]", ms / 1000, ms % 1000,
            "INFO", name, ok ? "ok" : "FAIL");
    if (detail && *detail)
        kprintf(" %s", detail);
    kprintf("\n");
}
