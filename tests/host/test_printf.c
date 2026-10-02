/* StratumOS - host-side unit tests for the kernel formatter and string code.
 *
 * These compile the *real* kernel sources (kernel/core/printf.c, string.c,
 * div64.c) for the host as 32-bit objects and diff their output against the
 * system libc. Snprintf bugs are miserable to find from inside a VM, and
 * there is no reason to look for them there when the code is pure and
 * testable. Runs in well under a second, so CI can run it on every push
 * before it bothers booting anything.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <kernel/printf.h>
#include <kernel/string.h>

static int failures;
static int checks;

/* The kernel formatter writes through console_putc; capture it instead. */
static char capture[8192];
static size_t capture_len;

void console_putc(char c)
{
    if (capture_len < sizeof(capture) - 1)
        capture[capture_len++] = c;
    capture[capture_len] = '\0';
}

static void fail(const char *what, const char *expect, const char *got)
{
    failures++;
    printf("  FAIL %-28s expected \"%s\" got \"%s\"\n", what, expect, got);
}

/* Compare our ksnprintf against the host's snprintf for the same arguments. */
#define CHECK_FMT(fmt, ...)                                                   \
    do {                                                                      \
        char mine[256], theirs[256];                                          \
        checks++;                                                             \
        ksnprintf(mine, sizeof(mine), fmt, __VA_ARGS__);                      \
        snprintf(theirs, sizeof(theirs), fmt, __VA_ARGS__);                   \
        if (strcmp(mine, theirs) != 0)                                        \
            fail(fmt, theirs, mine);                                          \
    } while (0)

#define CHECK_EQ_STR(label, got, expect)                                      \
    do {                                                                      \
        checks++;                                                             \
        if (strcmp((got), (expect)) != 0)                                     \
            fail(label, expect, got);                                         \
    } while (0)

#define CHECK_TRUE(label, cond)                                               \
    do {                                                                      \
        checks++;                                                             \
        if (!(cond))                                                          \
            fail(label, "true", "false");                                     \
    } while (0)

static void test_integers(void)
{
    puts("formatter: integers");
    CHECK_FMT("%d", 0);
    CHECK_FMT("%d", 1);
    CHECK_FMT("%d", -1);
    CHECK_FMT("%d", 2147483647);
    CHECK_FMT("%d", -2147483647 - 1);
    CHECK_FMT("%i", 42);
    CHECK_FMT("%u", 0u);
    CHECK_FMT("%u", 4294967295u);
    CHECK_FMT("%x", 0xdeadbeefu);
    CHECK_FMT("%X", 0xdeadbeefu);
    CHECK_FMT("%o", 0755u);
    CHECK_FMT("%hhd", (signed char)-5);
    CHECK_FMT("%hd", (short)-300);
}

static void test_width_and_flags(void)
{
    puts("formatter: width, precision and flags");
    CHECK_FMT("%5d", 42);
    CHECK_FMT("%-5d|", 42);
    CHECK_FMT("%05d", 42);
    CHECK_FMT("%05d", -42);
    CHECK_FMT("%+d", 42);
    CHECK_FMT("% d", 42);
    CHECK_FMT("%+d", -42);
    CHECK_FMT("%08x", 0x1234u);
    CHECK_FMT("%#x", 0x1234u);
    CHECK_FMT("%#010x", 0x1234u);
    CHECK_FMT("%.5d", 42);
    CHECK_FMT("%10.5d", 42);
    CHECK_FMT("%-10.5d|", 42);
    CHECK_FMT("%*d", 7, 42);
    CHECK_FMT("%-*d|", 7, 42);
    CHECK_FMT("%.*d", 6, 42);
    CHECK_FMT("%#o", 0755u);
}

/* The next two tests deliberately feed the formatter things a correct format
 * string would never contain, so GCC's format checking has to be told to look
 * the other way for exactly those lines. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-overflow"
#pragma GCC diagnostic ignored "-Wformat-extra-args"

static void test_strings_and_chars(void)
{
    puts("formatter: strings and chars");
    CHECK_FMT("%s", "hello");
    CHECK_FMT("[%10s]", "hi");
    CHECK_FMT("[%-10s]", "hi");
    CHECK_FMT("[%.2s]", "hello");
    CHECK_FMT("[%10.2s]", "hello");
    CHECK_FMT("%c", 'x');
    CHECK_FMT("[%3c]", 'x');
    CHECK_FMT("[%-3c]", 'x');
    CHECK_FMT("%s%s", "a", "b");

    char buf[64];
    ksnprintf(buf, sizeof(buf), "%s", (char *)NULL);
    CHECK_EQ_STR("%s with NULL", buf, "(null)");

    ksnprintf(buf, sizeof(buf), "100%%");
    CHECK_EQ_STR("literal percent", buf, "100%");
}

#pragma GCC diagnostic pop

static void test_64bit(void)
{
    puts("formatter: 64-bit values (exercises div64.c)");
    CHECK_FMT("%llu", 0ULL);
    CHECK_FMT("%llu", 1ULL);
    CHECK_FMT("%llu", 4294967296ULL);
    CHECK_FMT("%llu", 18446744073709551615ULL);
    CHECK_FMT("%llu", 1234567890123456789ULL);
    CHECK_FMT("%llx", 0x123456789abcdefULL);
    CHECK_FMT("%lld", -1234567890123LL);
    CHECK_FMT("%lld", 1234567890123LL);
    CHECK_FMT("%20llu", 123456789012ULL);
    CHECK_FMT("%zu", (size_t)123456);
}

static void test_pointer(void)
{
    puts("formatter: pointers");
    char buf[64];
    /* The kernel deliberately prints a fixed-width 0x%08x rather than libc's
     * variable form, so this is checked literally rather than diffed. */
    ksnprintf(buf, sizeof(buf), "%p", (void *)0x1000);
    CHECK_EQ_STR("%p padding", buf, "0x00001000");
    ksnprintf(buf, sizeof(buf), "%p", (void *)0);
    CHECK_EQ_STR("%p of NULL", buf, "0x00000000");
    ksnprintf(buf, sizeof(buf), "%p", (void *)0xdeadbeef);
    CHECK_EQ_STR("%p full width", buf, "0xdeadbeef");
}

static void test_truncation(void)
{
    puts("formatter: truncation and return value");
    char buf[8];
    int n = ksnprintf(buf, sizeof(buf), "0123456789");
    CHECK_TRUE("returns untruncated length", n == 10);
    CHECK_EQ_STR("truncates to fit", buf, "0123456");
    CHECK_TRUE("always NUL-terminates", buf[7] == '\0');

    /* size 0 must not write at all */
    char guard[4] = { 'A', 'A', 'A', 'A' };
    ksnprintf(guard, 0, "zzzz");
    CHECK_TRUE("size 0 writes nothing", guard[0] == 'A');
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-extra-args"

static void test_unknown_conversion(void)
{
    puts("formatter: unknown conversions are visible");
    char buf[32];
    ksnprintf(buf, sizeof(buf), "a%qb");
    CHECK_EQ_STR("unknown verb echoed", buf, "a%qb");
    ksnprintf(buf, sizeof(buf), "trailing%%");
    CHECK_EQ_STR("trailing percent", buf, "trailing%");
}

#pragma GCC diagnostic pop

static void test_console_path(void)
{
    puts("formatter: kprintf reaches the console sink");
    capture_len = 0;
    capture[0] = '\0';
    int n = kprintf("x=%d y=%s", 7, "ok");
    CHECK_EQ_STR("kprintf output", capture, "x=7 y=ok");
    CHECK_TRUE("kprintf length", n == 8);
}

static void test_string_funcs(void)
{
    puts("string: core routines");
    CHECK_TRUE("strlen", strlen("hello") == 5);
    CHECK_TRUE("strlen empty", strlen("") == 0);
    CHECK_TRUE("strnlen bounded", strnlen("hello", 3) == 3);
    CHECK_TRUE("strcmp equal", strcmp("abc", "abc") == 0);
    CHECK_TRUE("strcmp less", strcmp("abc", "abd") < 0);
    CHECK_TRUE("strcmp greater", strcmp("abd", "abc") > 0);
    CHECK_TRUE("strncmp prefix", strncmp("abcdef", "abcxyz", 3) == 0);
    CHECK_TRUE("strcasecmp", strcasecmp("HeLLo", "hello") == 0);

    char dst[8];
    size_t want = strlcpy(dst, "abcdefghij", sizeof(dst));
    CHECK_TRUE("strlcpy reports source length", want == 10);
    CHECK_EQ_STR("strlcpy truncates", dst, "abcdefg");

    CHECK_TRUE("strchr found", strchr("hello", 'l') != NULL);
    CHECK_TRUE("strchr missing", strchr("hello", 'z') == NULL);
    CHECK_TRUE("strrchr last", strrchr("hello", 'l')[1] == 'o');

    /* memmove must handle overlap in both directions. */
    char ov[16];
    strcpy(ov, "0123456789");
    memmove(ov + 2, ov, 8);
    CHECK_EQ_STR("memmove forward overlap", ov, "0101234567");
    strcpy(ov, "0123456789");
    memmove(ov, ov + 2, 8);
    CHECK_EQ_STR("memmove backward overlap", ov, "2345678989");

    /* memset's dword fast path must handle unaligned starts and tails. */
    unsigned char pat[32];
    memset(pat, 0, sizeof(pat));
    memset(pat + 3, 0xAB, 20);
    CHECK_TRUE("memset leaves head", pat[2] == 0x00);
    CHECK_TRUE("memset fills body", pat[3] == 0xAB && pat[22] == 0xAB);
    CHECK_TRUE("memset leaves tail", pat[23] == 0x00);

    CHECK_TRUE("memcmp equal", memcmp("abc", "abc", 3) == 0);
    CHECK_TRUE("memcmp differs", memcmp("abc", "abd", 3) < 0);
}

static void test_str_to_u32(void)
{
    puts("string: str_to_u32 parsing and overflow");
    u32 v;
    CHECK_TRUE("decimal", str_to_u32("1234", &v) && v == 1234);
    CHECK_TRUE("hex lower", str_to_u32("0xdeadbeef", &v) && v == 0xdeadbeefu);
    CHECK_TRUE("hex upper", str_to_u32("0XFF", &v) && v == 255);
    CHECK_TRUE("max u32", str_to_u32("4294967295", &v) && v == 4294967295u);
    CHECK_TRUE("rejects overflow", !str_to_u32("4294967296", &v));
    CHECK_TRUE("rejects huge hex", !str_to_u32("0x100000000", &v));
    CHECK_TRUE("rejects empty", !str_to_u32("", &v));
    CHECK_TRUE("rejects garbage", !str_to_u32("12x4", &v));
    CHECK_TRUE("rejects negative", !str_to_u32("-1", &v));
    CHECK_TRUE("rejects bare 0x", !str_to_u32("0x", &v));
}

int main(void)
{
    puts("=== StratumOS host unit tests ===");
    test_integers();
    test_width_and_flags();
    test_strings_and_chars();
    test_64bit();
    test_pointer();
    test_truncation();
    test_unknown_conversion();
    test_console_path();
    test_string_funcs();
    test_str_to_u32();

    printf("\n%d checks, %d failure%s\n", checks, failures,
           failures == 1 ? "" : "s");
    return failures != 0;
}
