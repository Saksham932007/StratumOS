/* StratumOS - VGA text mode driver.
 *
 * The 80x25 text buffer at 0xB8000 holds one 16-bit cell per character: low
 * byte the code point, high byte the attribute (low nibble foreground, high
 * nibble background).
 *
 * Two details that are easy to get wrong and both handled here:
 *
 *   * The hardware cursor is addressed through the CRTC index/data port pair
 *     at 0x3D4/0x3D5, not by writing to the framebuffer.
 *   * Row 0 is reserved for a status line, so scrolling must leave it alone.
 */
#include <arch/io.h>
#include <drivers/vga.h>
#include <kernel/string.h>

#define CRTC_INDEX 0x3D4
#define CRTC_DATA  0x3D5

#define CRTC_CURSOR_START 0x0A
#define CRTC_CURSOR_END   0x0B
#define CRTC_CURSOR_HI    0x0E
#define CRTC_CURSOR_LO    0x0F

/* The top row is a status bar; normal output lives below it. */
#define TEXT_TOP 1

static volatile u16 *const fb = (volatile u16 *)VGA_PHYS;
static size_t cur_x, cur_y;
static u8 attr;
static bool cursor_visible = true;

static inline u16 cell(char c, u8 a)
{
    return (u16)(u8)c | ((u16)a << 8);
}

static void crtc_write(u8 index, u8 value)
{
    outb(CRTC_INDEX, index);
    outb(CRTC_DATA, value);
}

static void sync_cursor(void)
{
    u16 pos = (u16)(cur_y * VGA_WIDTH + cur_x);

    crtc_write(CRTC_CURSOR_LO, (u8)(pos & 0xFF));
    crtc_write(CRTC_CURSOR_HI, (u8)((pos >> 8) & 0xFF));
}

void vga_init(void)
{
    attr = vga_attr(VGA_LIGHT_GREY, VGA_BLACK);
    cur_x = 0;
    cur_y = TEXT_TOP;
    vga_clear();
    vga_show_cursor();
}

void vga_set_attr(u8 a) { attr = a; }
u8   vga_get_attr(void) { return attr; }

void vga_clear(void)
{
    for (size_t y = TEXT_TOP; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            fb[y * VGA_WIDTH + x] = cell(' ', attr);

    cur_x = 0;
    cur_y = TEXT_TOP;
    sync_cursor();
}

static void scroll(void)
{
    /* Shift rows up, starting below the status line. */
    for (size_t y = TEXT_TOP; y + 1 < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            fb[y * VGA_WIDTH + x] = fb[(y + 1) * VGA_WIDTH + x];

    for (size_t x = 0; x < VGA_WIDTH; x++)
        fb[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = cell(' ', attr);

    cur_y = VGA_HEIGHT - 1;
}

static void newline(void)
{
    cur_x = 0;
    if (++cur_y >= VGA_HEIGHT)
        scroll();
}

void vga_putchar(char c)
{
    switch (c) {
    case '\n':
        newline();
        break;
    case '\r':
        cur_x = 0;
        break;
    case '\t':
        /* Advance to the next 8-column stop, wrapping if that overruns. */
        cur_x = (cur_x + 8) & ~(size_t)7;
        if (cur_x >= VGA_WIDTH)
            newline();
        break;
    case '\b':
        /* Destructive backspace, able to step back onto the previous line -
         * which is what a line editor needs when the user deletes past a
         * wrap point. */
        if (cur_x > 0) {
            cur_x--;
        } else if (cur_y > TEXT_TOP) {
            cur_y--;
            cur_x = VGA_WIDTH - 1;
        }
        fb[cur_y * VGA_WIDTH + cur_x] = cell(' ', attr);
        break;
    default:
        if ((u8)c < 0x20)
            break; /* ignore other control characters */
        fb[cur_y * VGA_WIDTH + cur_x] = cell(c, attr);
        if (++cur_x >= VGA_WIDTH)
            newline();
        break;
    }

    if (cursor_visible)
        sync_cursor();
}

void vga_write(const char *s, size_t n)
{
    while (n--)
        vga_putchar(*s++);
}

void vga_set_cursor(size_t x, size_t y)
{
    if (x >= VGA_WIDTH)
        x = VGA_WIDTH - 1;
    if (y < TEXT_TOP)
        y = TEXT_TOP;
    if (y >= VGA_HEIGHT)
        y = VGA_HEIGHT - 1;

    cur_x = x;
    cur_y = y;
    sync_cursor();
}

void vga_get_cursor(size_t *x, size_t *y)
{
    if (x)
        *x = cur_x;
    if (y)
        *y = cur_y;
}

void vga_show_cursor(void)
{
    cursor_visible = true;
    crtc_write(CRTC_CURSOR_START, 14); /* scanline range for a thin underline */
    crtc_write(CRTC_CURSOR_END, 15);
    sync_cursor();
}

void vga_hide_cursor(void)
{
    cursor_visible = false;
    crtc_write(CRTC_CURSOR_START, 0x20); /* bit 5 disables the cursor */
}

void vga_status_line(const char *text, u8 a)
{
    size_t i = 0;

    for (; text[i] && i < VGA_WIDTH; i++)
        fb[i] = cell(text[i], a);
    for (; i < VGA_WIDTH; i++)
        fb[i] = cell(' ', a);

    /* Deliberately does not touch cur_x/cur_y: the status bar must be
     * paintable from a timer handler without disturbing a half-typed line. */
    if (cursor_visible)
        sync_cursor();
}
