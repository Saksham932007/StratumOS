/* StratumOS - PS/2 keyboard driver.
 *
 * Scancode set 1, which is what the 8042 translates to by default. The
 * interrupt handler is deliberately minimal: read the port, update modifier
 * state, translate, enqueue. Anything slower than that belongs in task
 * context, because this runs with interrupts disabled.
 *
 * Two things that trip people up and are handled here:
 *
 *   * 0xE0 prefix. Arrow keys, Home/End and the grey Delete all send a 0xE0
 *     escape followed by a scancode that collides with the numeric keypad.
 *     Without tracking that prefix, pressing Up types an '8'.
 *   * Caps Lock applies only to letters, and interacts with Shift by
 *     cancelling it - "caps on, shift held" gives lowercase.
 */
#define LOG_TAG "kbd"

#include <arch/io.h>
#include <arch/irq.h>

#include <drivers/keyboard.h>

#include <kernel/log.h>
#include <kernel/sched.h>

/* Scancode set 1, unshifted. Index is the make code. */
static const char keymap[128] = {
    0,    27,  '1', '2',  '3',  '4', '5',  '6',  /* 00-07 */
    '7',  '8', '9', '0',  '-',  '=', '\b', '\t', /* 08-0F */
    'q',  'w', 'e', 'r',  't',  'y', 'u',  'i',  /* 10-17 */
    'o',  'p', '[', ']',  '\n', 0,   'a',  's',  /* 18-1F */
    'd',  'f', 'g', 'h',  'j',  'k', 'l',  ';',  /* 20-27 */
    '\'', '`', 0,   '\\', 'z',  'x', 'c',  'v',  /* 28-2F */
    'b',  'n', 'm', ',',  '.',  '/', 0,    '*',  /* 30-37 */
    0,    ' ', 0,   0,    0,    0,   0,    0,    /* 38-3F */
    0,    0,   0,   0,    0,    0,   0,    '7',  /* 40-47 */
    '8',  '9', '-', '4',  '5',  '6', '+',  '1',  /* 48-4F */
    '2',  '3', '0', '.',  0,    0,   0,    0,    /* 50-57 */
};

static const char keymap_shift[128] = {
    0,   27,   '!',  '@', '#', '$', '%', '^', '&', '*', '(', ')', '_',
    '+', '\b', '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P',
    '{', '}',  '\n', 0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L',
    ':', '"',  '~',  0,   '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<',
    '>', '?',  0,    '*', 0,   ' ', 0,   0,   0,   0,   0,   0,   0,
    0,   0,    0,    0,   0,   0,   '7', '8', '9', '-', '4', '5', '6',
    '+', '1',  '2',  '3', '0', '.', 0,   0,   0,   0,
};

/* Scancodes (set 1) */
#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_CTRL     0x1D
#define SC_ALT      0x38
#define SC_CAPS     0x3A
#define SC_NUMLOCK  0x45
#define SC_EXTENDED 0xE0

#define SC_UP       0x48
#define SC_DOWN     0x50
#define SC_LEFT     0x4B
#define SC_RIGHT    0x4D
#define SC_HOME     0x47
#define SC_END      0x4F
#define SC_PGUP     0x49
#define SC_PGDN     0x51
#define SC_DELETE   0x53
#define SC_INSERT   0x52

static volatile int ring[KBD_BUFFER_SIZE];
static volatile u32 ring_head, ring_tail;
static u8 modifiers;
static bool extended_pending;
static volatile u32 events;

static void enqueue(int key)
{
    u32 next = (ring_head + 1) % KBD_BUFFER_SIZE;

    /* Drop on overflow rather than overwriting the oldest byte: losing the
     * newest keystroke is less confusing than losing the start of a command
     * the user already typed. */
    if (next == ring_tail)
        return;

    ring[ring_head] = key;
    ring_head = next;
}

void keyboard_inject(int key)
{
    bool irqs = irq_save();
    enqueue(key);
    irq_restore(irqs);
}

static int translate_extended(u8 code)
{
    switch (code) {
    case SC_UP:
        return KEY_UP;
    case SC_DOWN:
        return KEY_DOWN;
    case SC_LEFT:
        return KEY_LEFT;
    case SC_RIGHT:
        return KEY_RIGHT;
    case SC_HOME:
        return KEY_HOME;
    case SC_END:
        return KEY_END;
    case SC_PGUP:
        return KEY_PGUP;
    case SC_PGDN:
        return KEY_PGDN;
    case SC_DELETE:
        return KEY_DELETE;
    case SC_INSERT:
        return KEY_INSERT;
    default:
        return -1;
    }
}

static int translate_function(u8 code)
{
    if (code >= 0x3B && code <= 0x44)
        return KEY_F1 + (code - 0x3B);
    if (code == 0x57)
        return KEY_F11;
    if (code == 0x58)
        return KEY_F12;
    return -1;
}

static void keyboard_irq(struct regs *r)
{
    UNUSED(r);

    u8 raw = inb(KBD_DATA);

    if (raw == SC_EXTENDED) {
        extended_pending = true;
        return;
    }

    bool released = (raw & 0x80) != 0;
    u8 code = raw & 0x7F;
    bool was_extended = extended_pending;
    extended_pending = false;

    events++;

    /* --- modifiers ------------------------------------------------------ */
    switch (code) {
    case SC_LSHIFT:
    case SC_RSHIFT:
        if (released)
            modifiers &= (u8)~KBD_MOD_SHIFT;
        else
            modifiers |= KBD_MOD_SHIFT;
        return;
    case SC_CTRL:
        if (released)
            modifiers &= (u8)~KBD_MOD_CTRL;
        else
            modifiers |= KBD_MOD_CTRL;
        return;
    case SC_ALT:
        if (released)
            modifiers &= (u8)~KBD_MOD_ALT;
        else
            modifiers |= KBD_MOD_ALT;
        return;
    case SC_CAPS:
        if (!released)
            modifiers ^= KBD_MOD_CAPS; /* toggles on press only */
        return;
    case SC_NUMLOCK:
        if (!released)
            modifiers ^= KBD_MOD_NUM;
        return;
    default:
        break;
    }

    if (released)
        return;

    /* --- extended (0xE0-prefixed) keys ---------------------------------- */
    if (was_extended) {
        int key = translate_extended(code);
        if (key > 0)
            enqueue(key);
        return;
    }

    /* --- function keys --------------------------------------------------- */
    int fkey = translate_function(code);
    if (fkey > 0) {
        enqueue(fkey);
        return;
    }

    /* --- printable characters ------------------------------------------- */
    if (code >= ARRAY_SIZE(keymap))
        return;

    bool shift = (modifiers & KBD_MOD_SHIFT) != 0;
    bool caps = (modifiers & KBD_MOD_CAPS) != 0;
    char ch = shift ? keymap_shift[code] : keymap[code];

    if (ch == 0)
        return;

    /* Caps Lock affects letters only, and Shift inverts it rather than
     * stacking with it. */
    if (caps) {
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 'a' + 'A');
        else if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch - 'A' + 'a');
    }

    /* Ctrl+letter produces the classic control code, so Ctrl+C and Ctrl+L
     * are available to the shell. */
    if (modifiers & KBD_MOD_CTRL) {
        char lower = (ch >= 'A' && ch <= 'Z') ? (char)(ch + 32) : ch;
        if (lower >= 'a' && lower <= 'z')
            ch = (char)(lower - 'a' + 1);
    }

    enqueue(ch);
}

void keyboard_init(void)
{
    ring_head = ring_tail = 0;
    modifiers = 0;
    extended_pending = false;
    events = 0;

    /* Drain anything the firmware left in the output buffer, or the first
     * real keypress will be preceded by a stale byte. */
    for (int i = 0; i < 16 && (inb(KBD_STATUS) & 0x01); i++)
        (void)inb(KBD_DATA);

    irq_install_handler(IRQ_KEYBOARD, keyboard_irq, "ps2-keyboard");
}

bool keyboard_has_input(void)
{
    return ring_head != ring_tail;
}

int keyboard_poll(void)
{
    int key;
    bool irqs = irq_save();

    if (ring_head == ring_tail) {
        irq_restore(irqs);
        return -1;
    }

    key = ring[ring_tail];
    ring_tail = (ring_tail + 1) % KBD_BUFFER_SIZE;
    irq_restore(irqs);
    return key;
}

int keyboard_getchar(void)
{
    for (;;) {
        int key = keyboard_poll();
        if (key >= 0)
            return key;

        /* Give up the CPU rather than spinning. Once the scheduler is
         * running this lets other tasks work; before that it idles the CPU
         * until the next interrupt. */
        sched_yield();
    }
}

u8 keyboard_modifiers(void)
{
    return modifiers;
}

u32 keyboard_event_count(void)
{
    return events;
}
