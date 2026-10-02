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

void log_emit(enum log_level level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    u64 ms;

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
