/* StratumOS - memory and string primitives.
 *
 * These are not optional conveniences. GCC lowers struct assignment, array
 * initialisation and some loops into calls to memcpy/memset even under
 * -ffreestanding, so a kernel that does not define them fails to link in
 * confusing ways.
 */
#include <kernel/string.h>

void *memset(void *dst, int c, size_t n)
{
    u8 *p = dst;
    u8 v = (u8)c;

    /* Fill byte-wise up to alignment, then a word at a time. The dword loop
     * is roughly 4x faster and memset shows up in page-table and heap paths
     * often enough to matter. */
    while (n && ((uintptr_t)p & 3)) {
        *p++ = v;
        n--;
    }

    u32 pattern = (u32)v * 0x01010101u;
    while (n >= 4) {
        *(u32 *)p = pattern;
        p += 4;
        n -= 4;
    }

    while (n--)
        *p++ = v;

    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;

    if (!(((uintptr_t)d | (uintptr_t)s) & 3)) {
        while (n >= 4) {
            *(u32 *)d = *(const u32 *)s;
            d += 4;
            s += 4;
            n -= 4;
        }
    }

    while (n--)
        *d++ = *s++;

    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    u8 *d = dst;
    const u8 *s = src;

    if (d == s || n == 0)
        return dst;

    /* Overlapping and moving up: copy backwards so each byte is read before
     * the write that would have clobbered it. */
    if (d > s && d < s + n) {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
        return dst;
    }

    /* Overlapping and moving *down* needs a forward copy, which is what
     * memcpy does - but it may not be delegated to memcpy, and the reason is
     * not pedantry.
     *
     * memcpy's arguments are specified as non-overlapping, so calling it with
     * ranges that overlap is undefined behaviour. The call works today only
     * because this file's memcpy happens to be a forward byte loop; the
     * compiler is entitled to recognise that loop and replace it with
     * something that copies in a different order, or in blocks, and either
     * would corrupt the overlapping tail. The bug would appear as data
     * corruption after an unrelated change to an optimisation flag.
     *
     * A fuzz target found this - AddressSanitizer intercepts memcpy and
     * refuses overlapping ranges, which is exactly the contract being
     * violated. The fix is to spell the forward copy out here, so memmove
     * never hands memcpy a pair of ranges it is not allowed to have. */
    if (d < s && s < d + n) {
        while (n--)
            *d++ = *s++;
        return dst;
    }

    /* Genuinely disjoint. */
    return memcpy(dst, src, n);
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;

    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (int)(u8)*a - (int)(u8)*b;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return (int)(u8)lower(*a) - (int)(u8)lower(*b);
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

/* Truncating copy that always NUL-terminates and reports the length it
 * *wanted*, so callers can detect truncation. strncpy's habit of leaving the
 * destination unterminated has no place in a kernel. */
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t srclen = strlen(src);

    if (size) {
        size_t copy = (srclen >= size) ? size - 1 : srclen;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return srclen;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++)
        if (*s == (char)c)
            return (char *)s;
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *found = NULL;
    for (; *s; s++)
        if (*s == (char)c)
            found = s;
    return (char *)found;
}

/* Parse a decimal or 0x-prefixed hexadecimal integer. Returns false on an
 * empty string, a stray character, or overflow - callers that silently accept
 * a half-parsed number are how a shell ends up poking a random address. */
bool str_to_u32(const char *s, u32 *out)
{
    u32 base = 10, val = 0;
    bool any = false;

    if (!s || !*s)
        return false;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }

    for (; *s; s++) {
        u32 digit;

        if (*s >= '0' && *s <= '9')
            digit = (u32)(*s - '0');
        else if (base == 16 && *s >= 'a' && *s <= 'f')
            digit = (u32)(*s - 'a' + 10);
        else if (base == 16 && *s >= 'A' && *s <= 'F')
            digit = (u32)(*s - 'A' + 10);
        else
            return false;

        if (val > (0xFFFFFFFFu - digit) / base)
            return false; /* would overflow */

        val = val * base + digit;
        any = true;
    }

    if (!any)
        return false;

    *out = val;
    return true;
}
