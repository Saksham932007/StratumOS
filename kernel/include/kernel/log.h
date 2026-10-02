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
