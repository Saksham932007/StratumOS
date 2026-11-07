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
#include <kernel/spinlock.h>

static unsigned enabled_sinks;

/* The lock that keeps one processor's line out of the middle of another's.
 *
 * It is the outermost level in the hierarchy, which is to say the innermost
 * lock in practice: any subsystem may print while holding its own lock, so
 * the console's must be the one it is always legal to take next. Nothing the
 * console calls takes a lock of its own - the VGA driver writes to memory and
 * the UART is polled - so there is nothing below it to invert against.
 *
 * Masking interrupts is still necessary and is now the lock's job: a handler
 * that printed while interrupting a printing task would otherwise splice its
 * line into the middle of the other, on the one processor that cannot make
 * progress on it.
 *
 * A panic is the exception. The panicking processor may already hold this
 * lock - it is the lock most likely to be held when something has gone wrong
 * - and spinning on itself would replace a readable panic with a hang. So
 * every entry point below takes the lock only if this processor does not
 * already have it, and the panic path prints through the recursive case. */
static spinlock_t console_lock =
    SPINLOCK_INIT_AT("console", LOCK_LEVEL_CONSOLE);

/* Returns true if the caller must release. */
static bool console_acquire(void)
{
    if (spin_held_by_self(&console_lock))
        return false;
    spin_lock(&console_lock);
    return true;
}

static void console_release(bool owned)
{
    if (owned)
        spin_unlock(&console_lock);
}

/* Hold the console across a whole line.
 *
 * kprintf() emits one character at a time, so without this a line from two
 * processors would interleave character by character even though each
 * character is individually atomic. kvprintf() brackets its whole format with
 * these, and the per-character console_putc() calls inside then find the lock
 * already held by this processor and do nothing. */
bool console_begin(void)
{
    return console_acquire();
}

void console_end(bool owned)
{
    console_release(owned);
}

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

static void console_putc_locked(char c)
{
    if (enabled_sinks & CONSOLE_SINK_VGA)
        vga_putchar(c);
    if (enabled_sinks & CONSOLE_SINK_SERIAL)
        serial_putchar(c);
}

void console_putc(char c)
{
    bool owned = console_acquire();

    console_putc_locked(c);
    console_release(owned);
}

void console_write(const char *s, size_t n)
{
    bool owned = console_acquire();

    if (enabled_sinks & CONSOLE_SINK_VGA)
        vga_write(s, n);
    if (enabled_sinks & CONSOLE_SINK_SERIAL)
        serial_write(s, n);

    console_release(owned);
}

void console_puts(const char *s)
{
    /* The whole string under one acquisition, not one per character: a
     * per-character lock would let two processors interleave their output
     * character by character, which is worse than useless. */
    bool owned = console_acquire();

    while (*s)
        console_putc_locked(*s++);

    console_release(owned);
}
