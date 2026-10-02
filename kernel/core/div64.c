/* StratumOS - 64-bit integer division support.
 *
 * On i386 there is no 64-bit divide instruction, so GCC emits calls to these
 * helpers whenever a `long long` is divided. They normally come from libgcc.
 * We provide them ourselves so the kernel links against nothing at all - the
 * whole image is code from this repository plus the compiler's own headers,
 * which makes "what is actually in my kernel" a question with a short answer.
 *
 * The algorithm is textbook restoring division: one bit of quotient per
 * iteration, using only 64-bit shift, compare and subtract - all of which GCC
 * expands inline into 32-bit instruction pairs with no recursion back into
 * this file. 64 iterations is slow in the abstract, but kernel division is
 * confined to printf and a handful of time conversions.
 */
#include <kernel/types.h>

/* These are compiler-internal ABI symbols: GCC emits calls to them, so there
 * is no header that declares them. Prototyping them here keeps the build's
 * -Wmissing-prototypes honest and documents the exact signatures libgcc uses.
 */
u64 __udivmoddi4(u64 num, u64 den, u64 *rem);
u64 __udivdi3(u64 num, u64 den);
u64 __umoddi3(u64 num, u64 den);
i64 __divdi3(i64 num, i64 den);
i64 __moddi3(i64 num, i64 den);

static u64 udivmod64(u64 num, u64 den, u64 *rem)
{
    u64 quot = 0, r = 0;

    if (den == 0) {
        /* Division by zero is undefined in C. Return the saturated value
         * rather than faulting inside printf, which is where this would most
         * likely be reached from. */
        if (rem)
            *rem = 0;
        return ~(u64)0;
    }

    /* Fast path: both operands fit in 32 bits, so the hardware can do it. */
    if ((num >> 32) == 0 && (den >> 32) == 0) {
        u32 n = (u32)num, d = (u32)den;
        if (rem)
            *rem = n % d;
        return n / d;
    }

    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((num >> i) & 1);
        if (r >= den) {
            r -= den;
            quot |= (u64)1 << i;
        }
    }

    if (rem)
        *rem = r;
    return quot;
}

u64 __udivmoddi4(u64 num, u64 den, u64 *rem)
{
    return udivmod64(num, den, rem);
}

u64 __udivdi3(u64 num, u64 den)
{
    return udivmod64(num, den, NULL);
}

u64 __umoddi3(u64 num, u64 den)
{
    u64 rem;
    udivmod64(num, den, &rem);
    return rem;
}

i64 __divdi3(i64 num, i64 den)
{
    bool negative = false;
    u64 n, d, q;

    if (num < 0) {
        num = -num;
        negative = !negative;
    }
    if (den < 0) {
        den = -den;
        negative = !negative;
    }

    n = (u64)num;
    d = (u64)den;
    q = udivmod64(n, d, NULL);

    return negative ? -(i64)q : (i64)q;
}

i64 __moddi3(i64 num, i64 den)
{
    bool negative = num < 0;
    u64 rem;

    if (num < 0)
        num = -num;
    if (den < 0)
        den = -den;

    udivmod64((u64)num, (u64)den, &rem);

    /* C99: the remainder takes the sign of the dividend. */
    return negative ? -(i64)rem : (i64)rem;
}
