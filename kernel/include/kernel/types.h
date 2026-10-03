/* StratumOS - fixed-width types and a few conveniences.
 *
 * We compile freestanding, so <stdint.h>, <stddef.h> and <stdbool.h> come
 * from the compiler itself (they are required to exist in a freestanding
 * environment) rather than from a C library.
 */
#ifndef _KERNEL_TYPES_H
#define _KERNEL_TYPES_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

/* `unsigned long long` rather than uint64_t, deliberately.
 *
 * Both are 64 bits wide on every target this kernel builds for, but on a
 * 64-bit target uint64_t is `unsigned long` - and then every `%llu` in the
 * kernel is a format mismatch that -Wformat rejects. Pinning the underlying
 * type means one format specifier works on both architectures, rather than a
 * PRIu64 macro threaded through several hundred call sites.
 *
 * Found by compiling kernel/core/log.c for riscv64: five errors, all of them
 * this. */
typedef unsigned long long u64;

/* Physical and virtual addresses are distinct ideas even when the numbers
 * happen to match, so give them distinct names - and they are *address*
 * sized, not 32-bit.
 *
 * They were `u32`, which is right on i386 and wrong on anything wider, and
 * that single definition was the root cause of every portability failure the
 * RISC-V port found: 88 errors across seven files, every one of them a cast
 * between an address and a pointer of a different width. Nothing else in
 * mm/, core/ or fs/ was architecture-specific at all.
 *
 * uintptr_t is exactly the right type and was available the whole time. See
 * docs/PORTING.md. */
typedef uintptr_t paddr_t;
typedef uintptr_t vaddr_t;

/* For the cast between an integer and a pointer, which is the one place a
 * kernel does this constantly and the one place getting the width wrong is
 * silent. Spelled out so that `(void *)(uptr)x` reads as deliberate rather
 * than as a cast someone added to quiet a warning. */
typedef uintptr_t uptr;

/* For the places that genuinely need to print one. A `%p` cast via (void *)
 * is preferred, but a width-correct integer specifier has to exist. */
#if UINTPTR_MAX > 0xFFFFFFFFu
#define PRIxADDR "llx"
#define PRIuADDR "llu"
#else
#define PRIxADDR "x"
#define PRIuADDR "u"
#endif

#define PACKED            __attribute__((packed))
#define ALIGNED(n)        __attribute__((aligned(n)))
#define NORETURN          __attribute__((noreturn))
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))
#define UNUSED(x)         ((void)(x))
#define MAYBE_UNUSED      __attribute__((unused))

#define ARRAY_SIZE(a)     (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)         ((a) < (b) ? (a) : (b))
#define MAX(a, b)         ((a) > (b) ? (a) : (b))

#define ALIGN_UP(x, a)    (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a)  ((x) & ~((a) - 1))
#define IS_ALIGNED(x, a)  (((x) & ((a) - 1)) == 0)

#define KIB               (1024u)
#define MIB               (1024u * 1024u)

#endif /* _KERNEL_TYPES_H */
