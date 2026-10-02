/* StratumOS - formatted output.
 *
 * Supports: %c %s %d %i %u %x %X %o %p %% with field width, '-' left-align,
 * '0' zero-pad, '+'/' ' sign, '#' alternate form, and the l/ll/z length
 * modifiers (64-bit division is done with a shift-subtract helper because
 * libgcc's __udivdi3 is not linked in).
 */
#ifndef _KERNEL_PRINTF_H
#define _KERNEL_PRINTF_H

#include <kernel/types.h>

int kprintf(const char *fmt, ...) PRINTF_LIKE(1, 2);
int kvprintf(const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...) PRINTF_LIKE(3, 4);
int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#endif /* _KERNEL_PRINTF_H */
