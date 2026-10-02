/* StratumOS - unrecoverable failure handling. */
#ifndef _KERNEL_PANIC_H
#define _KERNEL_PANIC_H

#include <kernel/types.h>

struct regs;

NORETURN void panic(const char *fmt, ...) PRINTF_LIKE(1, 2);
NORETURN void panic_with_regs(const struct regs *r, const char *fmt, ...)
    PRINTF_LIKE(2, 3);

/* Walk the saved frame-pointer chain and print return addresses. The kernel is
 * built with -fno-omit-frame-pointer specifically so this works. */
void backtrace(u32 ebp, unsigned max_frames);

#define BUG_ON(cond)                                              \
    do {                                                          \
        if (cond)                                                 \
            panic("BUG at %s:%d: %s", __FILE__, __LINE__, #cond); \
    } while (0)

#define ASSERT(cond)                                                           \
    do {                                                                       \
        if (!(cond))                                                           \
            panic("assertion failed at %s:%d: %s", __FILE__, __LINE__, #cond); \
    } while (0)

#endif /* _KERNEL_PANIC_H */
