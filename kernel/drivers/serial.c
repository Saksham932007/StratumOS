/* StratumOS - 16550 UART driver.
 *
 * Serial is the first thing brought up and the last thing to break. It needs
 * no memory manager, no interrupts and no display, so it is the only output
 * channel that can be trusted to report a failure in any of those. It is also
 * what makes this kernel testable: `qemu -serial stdio` turns the boot
 * transcript into something a CI script can assert on.
 *
 * Transmit is polled. That is a deliberate simplification with a real cost -
 * see docs/ROADMAP.md - but it means logging works from inside an exception
 * handler, which an interrupt-driven ring buffer would not.
 */
#define LOG_TAG "serial"

#include <arch/io.h>
#include <arch/irq.h>
#include <drivers/keyboard.h>
#include <drivers/serial.h>
#include <kernel/log.h>

static u16 serial_port;
static bool serial_up;

/* Confirm a UART is really present before we start writing into the void.
 * Loopback mode feeds the transmitter straight back to the receiver, so a byte
 * written should come back unchanged. A missing device reads back 0xFF from
 * every register and fails this immediately. */
bool serial_loopback_test(u16 port)
{
    outb(port + UART_MCR, 0x1E); /* loopback + RTS/DTR/OUT1/OUT2 */
    outb(port + UART_DATA, 0xAE);

    if (inb(port + UART_DATA) != 0xAE)
        return false;

    /* A second, different pattern rules out a register that simply latches
     * whatever was last written to it. */
    outb(port + UART_DATA, 0x55);
    if (inb(port + UART_DATA) != 0x55)
        return false;

    return true;
}

bool serial_init(u16 port, u32 baud)
{
    u32 divisor;

    if (baud == 0)
        baud = 115200;

    divisor = 115200u / baud;
    if (divisor == 0)
        divisor = 1;

    outb(port + UART_IER, 0x00); /* mask all UART interrupts        */
    outb(port + UART_LCR, 0x80); /* DLAB=1: next two regs are the divisor */
    outb(port + UART_DIVISOR_LO, (u8)(divisor & 0xFF));
    outb(port + UART_DIVISOR_HI, (u8)((divisor >> 8) & 0xFF));
    outb(port + UART_LCR, 0x03); /* DLAB=0, 8 data bits, no parity, 1 stop */
    outb(port + UART_FCR, 0xC7); /* enable + clear FIFOs, 14-byte trigger  */
    outb(port + UART_MCR, 0x0B); /* RTS/DSR set, OUT2 on (gates the IRQ)   */

    if (!serial_loopback_test(port)) {
        serial_up = false;
        return false;
    }

    outb(port + UART_MCR, 0x0B); /* leave loopback mode */

    serial_port = port;
    serial_up = true;
    return true;
}

bool serial_ready(void)
{
    return serial_up;
}

static void serial_wait_tx(void)
{
    /* Bounded wait: a wedged UART must not hang the kernel. ~100k reads is
     * far longer than one character time at any real baud rate. */
    for (u32 spin = 0; spin < 100000; spin++)
        if (inb(serial_port + UART_LSR) & UART_LSR_THR_EMPTY)
            return;
}

void serial_putchar(char c)
{
    if (!serial_up)
        return;

    /* Terminals expect CRLF; the kernel speaks LF internally. */
    if (c == '\n') {
        serial_wait_tx();
        outb(serial_port + UART_DATA, '\r');
    }

    serial_wait_tx();
    outb(serial_port + UART_DATA, (u8)c);
}

void serial_write(const char *s, size_t n)
{
    while (n--)
        serial_putchar(*s++);
}

bool serial_has_input(void)
{
    if (!serial_up)
        return false;
    return (inb(serial_port + UART_LSR) & UART_LSR_DATA_READY) != 0;
}

int serial_getchar_nonblock(void)
{
    if (!serial_has_input())
        return -1;
    return inb(serial_port + UART_DATA);
}

/* ------------------------------------------------------------------------- */
/* Serial as a console input device                                          */
/* ------------------------------------------------------------------------- */

static u32 rx_count;

/* A terminal sends arrow keys as multi-byte escape sequences (ESC [ A and
 * friends), so the receive path needs a small state machine rather than a
 * byte-for-byte translation. */
enum rx_state {
    RX_NORMAL,
    RX_SAW_ESC,     /* got ESC, expecting '['            */
    RX_SAW_BRACKET, /* got ESC '[', expecting the final  */
};

static enum rx_state rx_state;

static void rx_dispatch(u8 byte)
{
    switch (rx_state) {
    case RX_NORMAL:
        if (byte == 0x1B) {
            rx_state = RX_SAW_ESC;
            return;
        }
        /* Terminals send CR for Enter and DEL for backspace; the kernel's
         * internal conventions are LF and BS. */
        if (byte == '\r')
            byte = '\n';
        else if (byte == 0x7F)
            byte = '\b';
        keyboard_inject(byte);
        return;

    case RX_SAW_ESC:
        if (byte == '[') {
            rx_state = RX_SAW_BRACKET;
            return;
        }
        /* A lone ESC, or something we do not model: deliver the ESC and then
         * reconsider this byte from scratch. */
        rx_state = RX_NORMAL;
        keyboard_inject(KEY_ESCAPE);
        rx_dispatch(byte);
        return;

    case RX_SAW_BRACKET:
        rx_state = RX_NORMAL;
        switch (byte) {
        case 'A': keyboard_inject(KEY_UP);    return;
        case 'B': keyboard_inject(KEY_DOWN);  return;
        case 'C': keyboard_inject(KEY_RIGHT); return;
        case 'D': keyboard_inject(KEY_LEFT);  return;
        case 'H': keyboard_inject(KEY_HOME);  return;
        case 'F': keyboard_inject(KEY_END);   return;
        case '3': keyboard_inject(KEY_DELETE); return; /* ESC [ 3 ~ */
        case '~': return;                              /* the tilde tail */
        default:  return;                              /* ignore the rest */
        }
    }
}

static void serial_irq(struct regs *r)
{
    UNUSED(r);

    /* Drain the FIFO: one interrupt can cover several bytes, and leaving any
     * behind means the next one is only noticed on the following keystroke. */
    while (inb(serial_port + UART_LSR) & UART_LSR_DATA_READY) {
        rx_count++;
        rx_dispatch(inb(serial_port + UART_DATA));
    }
}

void serial_console_init(void)
{
    if (!serial_up)
        return;

    rx_state = RX_NORMAL;
    rx_count = 0;

    /* Discard anything already sitting in the FIFO before enabling the
     * interrupt, or the first keystroke arrives with stale bytes ahead of it. */
    for (int i = 0; i < 16 && serial_has_input(); i++)
        (void)inb(serial_port + UART_DATA);

    /* OUT2 in the modem control register gates the UART's interrupt onto the
     * bus on a PC - without it, IER does nothing at all. serial_init() has
     * already set it. */
    outb(serial_port + UART_IER, 0x01); /* received-data-available only */

    irq_install_handler(IRQ_COM1, serial_irq, "16550-uart");
    pr_info("serial console input enabled on IRQ %u", (unsigned)IRQ_COM1);
}

u32 serial_rx_count(void)
{
    return rx_count;
}
