/* StratumOS - in-kernel test registry.
 *
 * Tests are plain functions registered in a table and run by the `selftest`
 * shell command or automatically when the kernel is booted with `autotest` on
 * its command line. Results go to the serial port in a fixed, greppable format
 * so tools/run-tests.py can assert on them without parsing prose.
 */
#ifndef _KERNEL_KTEST_H
#define _KERNEL_KTEST_H

#include <kernel/types.h>

struct ktest_result {
    unsigned checks;
    unsigned failures;
    const char *first_failure;
};

typedef void (*ktest_fn)(struct ktest_result *r);

struct ktest {
    const char *name;
    const char *description;
    ktest_fn fn;
};

/* Record a check. Keeps going after a failure so one run reports everything
 * that is broken rather than only the first thing. */
void ktest_check(struct ktest_result *r, bool ok, const char *what);

#define KT_ASSERT(r, cond) ktest_check((r), (cond), #cond)
#define KT_EQ(r, a, b)     ktest_check((r), (a) == (b), #a " == " #b)

/* Returns the number of failed tests. */
unsigned ktest_run_all(void);
unsigned ktest_run_one(const char *name);
void ktest_list(void);
u32 ktest_count(void);

#endif /* _KERNEL_KTEST_H */
