/* StratumOS - VGA text mode (80x25) driver. */
#ifndef _DRIVERS_VGA_H
#define _DRIVERS_VGA_H

#include <kernel/types.h>

#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_PHYS    0xB8000u

typedef enum {
    VGA_BLACK = 0, VGA_BLUE, VGA_GREEN, VGA_CYAN,
    VGA_RED, VGA_MAGENTA, VGA_BROWN, VGA_LIGHT_GREY,
    VGA_DARK_GREY, VGA_LIGHT_BLUE, VGA_LIGHT_GREEN, VGA_LIGHT_CYAN,
    VGA_LIGHT_RED, VGA_LIGHT_MAGENTA, VGA_YELLOW, VGA_WHITE,
} vga_color_t;

static inline u8 vga_attr(vga_color_t fg, vga_color_t bg)
{
    return (u8)(fg | (bg << 4));
}

void vga_init(void);
void vga_putchar(char c);
void vga_write(const char *s, size_t n);
void vga_clear(void);
void vga_set_attr(u8 attr);
u8   vga_get_attr(void);
void vga_set_cursor(size_t x, size_t y);
void vga_get_cursor(size_t *x, size_t *y);
void vga_hide_cursor(void);
void vga_show_cursor(void);

/* Paint a one-line status bar on the top row without disturbing the cursor. */
void vga_status_line(const char *text, u8 attr);

#endif /* _DRIVERS_VGA_H */
