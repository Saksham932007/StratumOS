/* StratumOS - formatted output.
 *
 * One formatter drives both kprintf() (straight to the console) and
 * ksnprintf() (into a caller's buffer), via a small sink struct. Writing it
 * twice is how the two drift apart and the one you debug with starts lying.
 *
 * Supported: %c %s %d %i %u %x %X %o %p %%
 * Flags:     '-' left-justify, '0' zero-pad, '+' force sign, ' ' sign space,
 *            '#' alternate form (0x / 0 prefix)
 * Width:     decimal or '*'
 * Precision: '.N' or '.*'  (max chars for %s, min digits for integers)
 * Length:    hh h l ll z t
 *
 * Unknown conversions are emitted verbatim rather than silently swallowed, so
 * a typo in a format string shows up in the output instead of vanishing.
 */
#include <kernel/console.h>
#include <kernel/printf.h>
#include <kernel/string.h>

struct sink {
    char *buf;      /* NULL means "write straight to the console" */
    size_t size;    /* capacity of buf, including the NUL          */
    size_t written; /* what a large enough buffer would have held  */
};

static void sink_putc(struct sink *s, char c)
{
    if (s->buf) {
        if (s->written + 1 < s->size)
            s->buf[s->written] = c;
    } else {
        console_putc(c);
    }
    s->written++;
}

static void sink_pad(struct sink *s, char c, int count)
{
    while (count-- > 0)
        sink_putc(s, c);
}

/* ------------------------------------------------------------------------- */

#define FLAG_LEFT  0x01
#define FLAG_ZERO  0x02
#define FLAG_PLUS  0x04
#define FLAG_SPACE 0x08
#define FLAG_ALT   0x10

struct spec {
    unsigned flags;
    int width;
    int prec; /* -1 when unspecified */
};

static void emit_number(struct sink *s, u64 value, unsigned base, bool upper,
                        const struct spec *sp, const char *sign)
{
    static const char digits_lo[] = "0123456789abcdef";
    static const char digits_up[] = "0123456789ABCDEF";
    const char *digits = upper ? digits_up : digits_lo;

    char tmp[24];
    int n = 0;

    if (value == 0) {
        tmp[n++] = '0';
    } else if (base == 10) {
        /* Keep the common case on 32-bit hardware division when it fits. */
        if ((value >> 32) == 0) {
            u32 v = (u32)value;
            while (v) {
                tmp[n++] = digits[v % 10];
                v /= 10;
            }
        } else {
            while (value) {
                tmp[n++] = digits[value % 10];
                value /= 10;
            }
        }
    } else {
        unsigned shift = (base == 16) ? 4 : 3;
        unsigned mask = base - 1;
        while (value) {
            tmp[n++] = digits[value & mask];
            value >>= shift;
        }
    }

    char prefix[3] = {0, 0, 0};
    int plen = 0;

    if (sign && *sign)
        prefix[plen++] = *sign;

    if ((sp->flags & FLAG_ALT) && base == 16) {
        prefix[plen++] = '0';
        prefix[plen++] = upper ? 'X' : 'x';
    } else if ((sp->flags & FLAG_ALT) && base == 8 && tmp[n - 1] != '0') {
        prefix[plen++] = '0';
    }

    /* Precision sets a minimum digit count; width sets a minimum total. The
     * '0' flag is ignored when a precision is given, per C. */
    int zeros = (sp->prec > n) ? sp->prec - n : 0;
    int len = plen + zeros + n;

    if (sp->prec < 0 && (sp->flags & FLAG_ZERO) && !(sp->flags & FLAG_LEFT) &&
        sp->width > len) {
        zeros += sp->width - len;
        len = sp->width;
    }

    int padding = (sp->width > len) ? sp->width - len : 0;

    if (!(sp->flags & FLAG_LEFT))
        sink_pad(s, ' ', padding);

    for (int i = 0; i < plen; i++)
        sink_putc(s, prefix[i]);
    sink_pad(s, '0', zeros);
    while (n--)
        sink_putc(s, tmp[n]);

    if (sp->flags & FLAG_LEFT)
        sink_pad(s, ' ', padding);
}

static void emit_string(struct sink *s, const char *str, const struct spec *sp)
{
    if (!str)
        str = "(null)";

    int len =
        (int)((sp->prec >= 0) ? strnlen(str, (size_t)sp->prec) : strlen(str));
    int padding = (sp->width > len) ? sp->width - len : 0;

    if (!(sp->flags & FLAG_LEFT))
        sink_pad(s, ' ', padding);
    for (int i = 0; i < len; i++)
        sink_putc(s, str[i]);
    if (sp->flags & FLAG_LEFT)
        sink_pad(s, ' ', padding);
}

static int format(struct sink *s, const char *fmt, va_list ap)
{
    enum { LEN_INT, LEN_CHAR, LEN_SHORT, LEN_LONG, LEN_LLONG, LEN_SIZE };

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            sink_putc(s, *fmt);
            continue;
        }

        const char *start = fmt;
        struct spec sp = {0, 0, -1};
        int length = LEN_INT;

        fmt++; /* past '%' */

        /* --- flags --- */
        for (;; fmt++) {
            if (*fmt == '-')
                sp.flags |= FLAG_LEFT;
            else if (*fmt == '0')
                sp.flags |= FLAG_ZERO;
            else if (*fmt == '+')
                sp.flags |= FLAG_PLUS;
            else if (*fmt == ' ')
                sp.flags |= FLAG_SPACE;
            else if (*fmt == '#')
                sp.flags |= FLAG_ALT;
            else
                break;
        }

        /* --- width --- */
        if (*fmt == '*') {
            int w = va_arg(ap, int);
            if (w < 0) {
                sp.flags |= FLAG_LEFT;
                w = -w;
            }
            sp.width = w;
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                sp.width = sp.width * 10 + (*fmt++ - '0');
        }

        /* --- precision --- */
        if (*fmt == '.') {
            fmt++;
            sp.prec = 0;
            if (*fmt == '*') {
                sp.prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    sp.prec = sp.prec * 10 + (*fmt++ - '0');
            }
        }

        /* --- length modifier --- */
        if (*fmt == 'h') {
            fmt++;
            length = LEN_SHORT;
            if (*fmt == 'h') {
                fmt++;
                length = LEN_CHAR;
            }
        } else if (*fmt == 'l') {
            fmt++;
            length = LEN_LONG;
            if (*fmt == 'l') {
                fmt++;
                length = LEN_LLONG;
            }
        } else if (*fmt == 'z' || *fmt == 't') {
            fmt++;
            length = LEN_SIZE;
        }

        /* --- conversion --- */
        switch (*fmt) {
        case 'd':
        case 'i': {
            i64 v;
            if (length == LEN_LLONG)
                v = va_arg(ap, long long);
            else if (length == LEN_LONG)
                v = va_arg(ap, long);
            else if (length == LEN_SIZE)
                v = (i64)va_arg(ap, intptr_t);
            else
                v = va_arg(ap, int);
            if (length == LEN_SHORT)
                v = (i16)v;
            if (length == LEN_CHAR)
                v = (i8)v;

            const char *sign = "";
            u64 mag;
            if (v < 0) {
                sign = "-";
                mag = (u64)(-(i64)v);
            } else {
                mag = (u64)v;
                if (sp.flags & FLAG_PLUS)
                    sign = "+";
                else if (sp.flags & FLAG_SPACE)
                    sign = " ";
            }
            emit_number(s, mag, 10, false, &sp, sign);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            u64 v;
            if (length == LEN_LLONG)
                v = va_arg(ap, unsigned long long);
            else if (length == LEN_LONG)
                v = va_arg(ap, unsigned long);
            else if (length == LEN_SIZE)
                v = va_arg(ap, size_t);
            else
                v = va_arg(ap, unsigned int);
            if (length == LEN_SHORT)
                v = (u16)v;
            if (length == LEN_CHAR)
                v = (u8)v;

            unsigned base = (*fmt == 'o') ? 8 : (*fmt == 'u' ? 10 : 16);
            emit_number(s, v, base, *fmt == 'X', &sp, NULL);
            break;
        }
        case 'p': {
            void *ptr = va_arg(ap, void *);
            struct spec psp = sp;
            /* Pointers always print as a full, zero-padded 32-bit value: a
             * mixture of 0x1000 and 0x00001000 in a log is miserable to scan. */
            psp.flags |= FLAG_ALT | FLAG_ZERO;
            psp.prec = -1;
            if (psp.width < 10)
                psp.width = 10;
            emit_number(s, (u32)(uintptr_t)ptr, 16, false, &psp, NULL);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            int padding = (sp.width > 1) ? sp.width - 1 : 0;
            if (!(sp.flags & FLAG_LEFT))
                sink_pad(s, ' ', padding);
            sink_putc(s, c);
            if (sp.flags & FLAG_LEFT)
                sink_pad(s, ' ', padding);
            break;
        }
        case 's':
            emit_string(s, va_arg(ap, const char *), &sp);
            break;
        case '%':
            sink_putc(s, '%');
            break;
        case '\0':
            /* Trailing '%' with nothing after it: emit it and stop. */
            sink_putc(s, '%');
            fmt--;
            break;
        default:
            /* Unrecognised: reproduce the whole directive so the mistake is
             * visible rather than invisible. */
            while (start <= fmt)
                sink_putc(s, *start++);
            break;
        }
    }

    if (s->buf && s->size)
        s->buf[(s->written < s->size) ? s->written : s->size - 1] = '\0';

    return (int)s->written;
}

int kvprintf(const char *fmt, va_list ap)
{
    struct sink s = {NULL, 0, 0};
    return format(&s, fmt, ap);
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct sink s = {buf, size, 0};
    return format(&s, fmt, ap);
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
