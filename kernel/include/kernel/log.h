/* StratumOS - levelled logging.
 *
 * Every line is tagged with its subsystem and severity, which makes the serial
 * transcript something a test harness can assert against instead of guess at.
 */
#ifndef _KERNEL_LOG_H
#define _KERNEL_LOG_H

#include <kernel/printf.h>
#include <kernel/types.h>

enum log_level {
    LOG_PANIC = 0,
    LOG_ERROR,
    LOG_WARN,
    LOG_INFO,
    LOG_DEBUG,
    LOG_TRACE,
};

void log_set_level(enum log_level level);
enum log_level log_get_level(void);
bool log_set_level_by_name(const char *name);
const char *log_level_name(enum log_level level);

void log_emit(enum log_level level, const char *tag, const char *fmt, ...)
    PRINTF_LIKE(3, 4);

/* Each file defines LOG_TAG before including this header. */
#ifndef LOG_TAG
#define LOG_TAG "kernel"
#endif

#define pr_err(...)   log_emit(LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define pr_warn(...)  log_emit(LOG_WARN, LOG_TAG, __VA_ARGS__)
#define pr_info(...)  log_emit(LOG_INFO, LOG_TAG, __VA_ARGS__)
#define pr_debug(...) log_emit(LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define pr_trace(...) log_emit(LOG_TRACE, LOG_TAG, __VA_ARGS__)

/* ---- rate limiting ---------------------------------------------------
 *
 * Every warning above is emitted on a path a user program can reach, and the
 * console is a shared, slow, serial device. A ring-3 process that calls
 * write() with a bad pointer in a loop therefore makes the kernel print on
 * its behalf, as fast as the loop goes: the syscall fuzzer in user/fuzz.c
 * produced tens of thousands of lines and turned a 40,000-call run into
 * minutes of serial traffic, during which nothing else got a word in.
 *
 * That is a denial of service by an unprivileged process against the one
 * channel the operator uses to see what the machine is doing, so the fix
 * belongs in the logging layer rather than in each caller.
 *
 * A limiter allows `burst` lines per `window_ms`, then counts what it drops
 * and says so once the window closes - the information that something is
 * being hit hard is the part worth keeping, not the thousandth copy of the
 * message. Each call site gets its own limiter, so a flood of one warning
 * never hides a different one.
 *
 * The counters are plain words, deliberately: under SMP two processors can
 * race and allow one line more or fewer than the burst. The cost of being
 * wrong is a line, and taking a lock to print would put the console's lock
 * ordering underneath every warning in the kernel.
 *
 * One known gap, stated rather than papered over: the suppressed count is
 * reported by the *next* call from that site, so a flood that stops dead
 * leaves its last window's count unreported. Fixing it needs a timer callback
 * walking a registry of limiters, which is more machinery than the missing
 * line is worth - and the flood that matters is the one still going. */
struct log_ratelimit {
    u64 window_ms; /* length of the accounting window */
    u32 burst;     /* lines allowed per window */
    u64 window_start;
    u32 printed;
    u32 suppressed;
};

/* True if the caller should emit its line. Reports and clears any suppressed
 * count when a window closes, so the drop is always visible somewhere. */
bool log_ratelimit_allow(struct log_ratelimit *rl, enum log_level level,
                         const char *tag);

/* Default: five lines a second from any one call site. Enough that a genuine
 * problem is still visible in the transcript - and that the test harness,
 * which asserts on these very lines, still finds them - while a loop calling
 * from ring 3 cannot own the console. */
#define LOG_RATELIMIT_WINDOW_MS 1000u
#define LOG_RATELIMIT_BURST     5u

#define log_emit_ratelimited(level, ...)                 \
    do {                                                 \
        static struct log_ratelimit _rl = {              \
            .window_ms = LOG_RATELIMIT_WINDOW_MS,        \
            .burst = LOG_RATELIMIT_BURST,                \
        };                                               \
        if (log_ratelimit_allow(&_rl, (level), LOG_TAG)) \
            log_emit((level), LOG_TAG, __VA_ARGS__);     \
    } while (0)

#define pr_err_ratelimited(...)  log_emit_ratelimited(LOG_ERROR, __VA_ARGS__)
#define pr_warn_ratelimited(...) log_emit_ratelimited(LOG_WARN, __VA_ARGS__)

/* A uniform "doing X ... ok" line for the boot sequence. */
void log_boot_step(const char *name, bool ok, const char *detail);

/* Open or close a window in which ERROR-level lines are counted rather than
 * printed. Used only by the test suites, whose job includes provoking the
 * errors that CI is right to treat as failures when they are not expected.
 * A test asserts the count afterwards, so a window that swallows nothing is
 * itself a failure. */
void log_expect_errors(bool on);
u32 log_expected_errors(void);

#endif /* _KERNEL_LOG_H */
