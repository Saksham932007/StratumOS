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

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

/* Physical and virtual addresses are distinct ideas even when the numbers
 * happen to match, so give them distinct names. */
typedef u32 paddr_t;
typedef u32 vaddr_t;

#define PACKED          __attribute__((packed))
#define ALIGNED(n)      __attribute__((aligned(n)))
#define NORETURN        __attribute__((noreturn))
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))
#define UNUSED(x)       ((void)(x))
#define MAYBE_UNUSED    __attribute__((unused))

#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#define MAX(a, b)       ((a) > (b) ? (a) : (b))

#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((a) - 1)) == 0)

#define KIB (1024u)
#define MIB (1024u * 1024u)

#endif /* _KERNEL_TYPES_H */
