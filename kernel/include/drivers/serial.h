/* StratumOS - 16550 UART driver.
 *
 * The serial port is the kernel's lifeline: it works before the display does,
 * it survives a VGA driver bug, and `qemu -serial stdio` turns it into a
 * transcript that CI can assert on. Everything the kernel logs goes here.
 */
#ifndef _DRIVERS_SERIAL_H
#define _DRIVERS_SERIAL_H

#include <kernel/types.h>

#define COM1_BASE           0x3F8
#define COM2_BASE           0x2F8

/* Register offsets from the port base */
#define UART_DATA           0 /* RBR/THR when DLAB=0 */
#define UART_IER            1
#define UART_DIVISOR_LO     0 /* when DLAB=1 */
#define UART_DIVISOR_HI     1 /* when DLAB=1 */
#define UART_FCR            2
#define UART_LCR            3
#define UART_MCR            4
#define UART_LSR            5

#define UART_LSR_DATA_READY 0x01
#define UART_LSR_THR_EMPTY  0x20

bool serial_init(u16 port, u32 baud);
bool serial_ready(void);
void serial_putchar(char c);
void serial_write(const char *s, size_t n);
bool serial_has_input(void);
int serial_getchar_nonblock(void);

/* Self-test the UART with its own loopback mode - this is how we know the
 * port is really there rather than reading 0xFF off a missing device. */
bool serial_loopback_test(u16 port);

/* Turn the serial port into an input device: enable its receive interrupt and
 * feed arriving bytes into the keyboard queue, translating the terminal's
 * conventions (CR for Enter, DEL for backspace, ANSI escapes for the arrow
 * keys) into the same codes the PS/2 driver produces. The shell then works
 * identically over a serial line and on the VGA console. */
void serial_console_init(void);
u32 serial_rx_count(void);

#endif /* _DRIVERS_SERIAL_H */
