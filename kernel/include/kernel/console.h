/* StratumOS - console multiplexer.
 *
 * Output fans out to every registered sink, so the same kprintf() reaches the
 * VGA text screen a human is watching and the serial port the CI harness is
 * scraping. Sinks can be toggled at runtime: the shell silences serial echo
 * while a self-test is running, for instance.
 */
#ifndef _KERNEL_CONSOLE_H
#define _KERNEL_CONSOLE_H

#include <kernel/types.h>

enum console_sink {
    CONSOLE_SINK_VGA = 1 << 0,
    CONSOLE_SINK_SERIAL = 1 << 1,
    CONSOLE_SINK_ALL = CONSOLE_SINK_VGA | CONSOLE_SINK_SERIAL,
};

void console_init(void);
void console_enable(unsigned sinks);
void console_disable(unsigned sinks);
unsigned console_sinks(void);
void console_putc(char c);
void console_write(const char *s, size_t n);
void console_puts(const char *s);

#endif /* _KERNEL_CONSOLE_H */
