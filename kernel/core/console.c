/* StratumOS - console multiplexer.
 *
 * Fans every character out to the sinks that are currently enabled. The point
 * is that kprintf() has exactly one implementation and the caller never has to
 * decide whether a message is "for the screen" or "for the log".
 *
 * Writes run with interrupts masked so that an interrupt handler's log line
 * cannot be spliced into the middle of one from task context. The cost is real
 * - a polled UART write holds interrupts off for the duration - and is
 * discussed in docs/ROADMAP.md, where buffered transmit is the fix.
 */
#include <arch/io.h>

#include <drivers/serial.h>
#include <drivers/vga.h>

#include <kernel/console.h>

static unsigned enabled_sinks;

void console_init(void)
{
    enabled_sinks = 0;

    if (serial_ready())
        enabled_sinks |= CONSOLE_SINK_SERIAL;

    enabled_sinks |= CONSOLE_SINK_VGA;
}

void console_enable(unsigned sinks)
{
    if ((sinks & CONSOLE_SINK_SERIAL) && !serial_ready())
        sinks &= ~(unsigned)CONSOLE_SINK_SERIAL;
    enabled_sinks |= sinks;
}

void console_disable(unsigned sinks)
{
    enabled_sinks &= ~sinks;
}

unsigned console_sinks(void)
{
    return enabled_sinks;
}

void console_putc(char c)
{
    bool irqs = irq_save();

    if (enabled_sinks & CONSOLE_SINK_VGA)
        vga_putchar(c);
    if (enabled_sinks & CONSOLE_SINK_SERIAL)
        serial_putchar(c);

    irq_restore(irqs);
}

void console_write(const char *s, size_t n)
{
    bool irqs = irq_save();

    if (enabled_sinks & CONSOLE_SINK_VGA)
        vga_write(s, n);
    if (enabled_sinks & CONSOLE_SINK_SERIAL)
        serial_write(s, n);

    irq_restore(irqs);
}

void console_puts(const char *s)
{
    while (*s)
        console_putc(*s++);
}
