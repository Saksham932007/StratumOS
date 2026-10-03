/* StratumOS - the NS16550A on QEMU's `virt` machine.
 *
 * The same chip the x86 side drives at port 0x3F8, reached a different way:
 * there is no port I/O space on RISC-V and no `in`/`out` instruction, so the
 * registers are ordinary memory-mapped bytes at 0x10000000 and a driver is
 * volatile loads and stores.
 *
 * That is the whole difference. The register layout, the divisor latch, the
 * line-status bit to poll - all identical, because it is the same part. Which
 * is the first small piece of evidence for the claim docs/PORTING.md is
 * about: a driver's hardware knowledge ports, and only its access method
 * does not.
 */
#include <arch/riscv64/riscv.h>

#define RBR        0 /* receive buffer (read)                        */
#define THR        0 /* transmit holding (write)                     */
#define IER        1 /* interrupt enable                             */
#define FCR        2 /* FIFO control                                 */
#define LCR        3 /* line control                                 */
#define LSR        5 /* line status                                  */

#define DLL        0 /* divisor latch low, when LCR.DLAB is set      */
#define DLM        1 /* divisor latch high                           */

#define LCR_8N1    0x03
#define LCR_DLAB   0x80
#define LSR_THRE   0x20 /* transmit holding register empty       */
#define LSR_DR     0x01 /* data ready                            */

#define FCR_ENABLE 0x01
#define FCR_CLEAR  0x06

static inline void uart_reg_write(u32 reg, u8 v)
{
    mmio_write8(VIRT_UART0 + reg, v);
}

static inline u8 uart_reg_read(u32 reg)
{
    return mmio_read8(VIRT_UART0 + reg);
}

void uart_init(void)
{
    uart_reg_write(IER, 0x00); /* no interrupts: this port is polled */

    /* The divisor latch, which is the one place the sequence matters: DLAB
     * has to be set to see the latch registers at all, and cleared again
     * before the data register means what it says. QEMU does not care about
     * the baud rate, and doing it properly costs four stores and keeps the
     * driver honest about being a real 16550 driver. */
    uart_reg_write(LCR, LCR_DLAB);
    uart_reg_write(DLL, 0x03); /* 38400 at the usual 1.8432 MHz clock */
    uart_reg_write(DLM, 0x00);
    uart_reg_write(LCR, LCR_8N1);

    uart_reg_write(FCR, FCR_ENABLE | FCR_CLEAR);
}

/* The one symbol the portable formatter needs.
 *
 * kernel/core/printf.c compiles for this architecture completely unmodified
 * and this is its only undefined reference - which is most of why ksnprintf,
 * kprintf and the whole %-conversion machinery came across for free. */
void console_putc(char c)
{
    /* The newline convention is the console's business, not the formatter's,
     * which is why it is here and not in printf.c - the same split the x86
     * serial driver makes. */
    if (c == '\n')
        console_putc('\r');

    while (!(uart_reg_read(LSR) & LSR_THRE))
        ;

    uart_reg_write(THR, (u8)c);
}

void console_write(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        console_putc(s[i]);
}

void console_puts(const char *s)
{
    while (*s)
        console_putc(*s++);
}
