/* StratumOS - PS/2 keyboard driver.
 *
 * Interrupt driven. The handler does nothing but translate a scancode and drop
 * it in a ring buffer; all the interesting work happens in task context. Keys
 * that have no ASCII equivalent (arrows, Home/End, F-keys) are delivered as
 * values above 0x80 so a line editor can act on them.
 */
#ifndef _DRIVERS_KEYBOARD_H
#define _DRIVERS_KEYBOARD_H

#include <kernel/types.h>

#define KBD_DATA   0x60
#define KBD_STATUS 0x64
#define KBD_CMD    0x64

#define KBD_BUFFER_SIZE 128

/* Non-ASCII key codes, returned above the printable range. */
enum {
    KEY_UP = 0x80, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_DELETE, KEY_INSERT,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_ESCAPE,
};

#define KBD_MOD_SHIFT 0x01
#define KBD_MOD_CTRL  0x02
#define KBD_MOD_ALT   0x04
#define KBD_MOD_CAPS  0x08
#define KBD_MOD_NUM   0x10

void keyboard_init(void);
/* Non-blocking: returns -1 when the buffer is empty. */
int  keyboard_poll(void);
/* Blocking: yields the CPU until a key arrives. */
int  keyboard_getchar(void);
bool keyboard_has_input(void);
u8   keyboard_modifiers(void);
u32  keyboard_event_count(void);

/* Push a byte into the input queue as if it had been typed. Used by the
 * serial console so a remote terminal can drive the shell, and by the test
 * harness to script input. */
void keyboard_inject(int key);

#endif /* _DRIVERS_KEYBOARD_H */
