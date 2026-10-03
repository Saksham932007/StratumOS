/* StratumOS - a syscall fuzzer that runs in ring 3.
 *
 * The host targets in tests/fuzz exercise the kernel's parsers with a real
 * fuzzing engine, coverage feedback and sanitizers. They cannot exercise the
 * system call boundary, because that boundary is a privilege transition and a
 * host process has nowhere to transition from.
 *
 * So this is the other half: a program that sits in ring 3 and issues
 * syscalls with arguments chosen to be wrong, from the only seat where the
 * kernel's pointer validation can actually be attacked. Every value it passes
 * is a value a hostile program could pass.
 *
 * What it is and is not
 * ---------------------
 * It is not a coverage-guided fuzzer. There is no instrumentation in the
 * kernel, no corpus and no feedback - a ring-3 program cannot see which
 * kernel branches it reached. What it has instead is a deterministic
 * generator and a large number of iterations, which finds the shallow bugs:
 * an unvalidated pointer, a length that is not bounded, a call number that
 * indexes a table.
 *
 * Deterministic on purpose. A fuzzer whose sequence depends on the clock
 * produces a crash nobody can reproduce, and a kernel bug found once and
 * never again is barely better than not finding it. The seed is fixed, so
 * iteration 40,000 is the same call with the same arguments on every boot and
 * on every machine.
 *
 * The success condition is that the kernel is still running afterwards, which
 * is why this program reports its own completion: the kernel surviving is the
 * result, and a kernel that panicked says nothing at all.
 */
#include "syscall.h"

void _start(void);

/* xorshift32. Four lines, no multiply, and the same sequence everywhere -
 * which is the property that matters here. */
static u32 state = 0x5EED5EED;

static u32 next(void)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

/* Addresses worth passing to a kernel that is supposed to refuse them. Each
 * one is a distinct check in user_range_ok(), and a generator that only
 * produced random 32-bit numbers would hit the interesting ones by accident
 * about never. */
static u32 nasty_pointer(void)
{
    switch (next() % 12) {
    case 0:
        return 0; /* the null page                                        */
    case 1:
        return 0xC0000000u; /* exactly the kernel boundary               */
    case 2:
        return 0xBFFFFFFFu; /* one byte below it, so a length straddles  */
    case 3:
        return 0xC0100000u; /* the kernel's own text                     */
    case 4:
        return 0xFFFFF000u; /* the recursive page-table window           */
    case 5:
        return 0xFFFFFFFFu; /* so that base + len wraps                  */
    case 6:
        return 0xD0000000u; /* the kernel heap                           */
    case 7:
        return 0x00400000u; /* our own image, which is legitimate        */
    case 8:
        return 0xAFFFF000u; /* our own stack, also legitimate            */
    case 9:
        return 0x7F000000u; /* unmapped, but in user space               */
    case 10:
        return next() & 0xFFFFF000u; /* a random page                    */
    default:
        return next(); /* a random unaligned address                     */
    }
}

/* Lengths chosen the same way: the boundaries of the kernel's own bound, and
 * the values that make an addition overflow. */
static u32 nasty_length(void)
{
    switch (next() % 8) {
    case 0:
        return 0;
    case 1:
        return 1;
    case 2:
        return 1024; /* exactly SYS_WRITE_MAX                            */
    case 3:
        return 1025; /* one past it                                      */
    case 4:
        return 0xFFFFFFFFu;
    case 5:
        return 0x80000000u;
    case 6:
        return 4096;
    default:
        return next();
    }
}

/* A legitimate, short, printable buffer of our own.
 *
 * write() is the one call in the table that prints what it is given, and a
 * write the kernel *accepts* therefore puts its argument on the serial
 * console. With a pointer into our own image and a length of 1024 - both of
 * which the generator produces on purpose - that is a kilobyte of machine
 * code per call. The kernel is behaving correctly; the problem is that the
 * transcript CI reads is the same stream, and 400 such calls bury it.
 *
 * So the accepted case is aimed here instead. The rejected cases are the ones
 * actually under test and are left exactly as generated: the kernel refuses
 * them before it reads a byte, so they print nothing either way. */
static const char legit[] = ".";

#define ITERATIONS     40000

/* How often to report, in iterations. A power of two so the test is an AND. */
#define PROGRESS_EVERY 8192

void _start(void)
{
    u_puts("  [fuzz] ring-3 syscall fuzzer: ");
    u_putu(ITERATIONS);
    u_puts(" calls, deterministic seed\n");

    u32 refused = 0, accepted = 0;

    for (u32 i = 0; i < ITERATIONS; i++) {
        /* Call numbers mostly inside the table, sometimes outside it: both
         * the dispatch and the rejection are worth hammering, and a generator
         * that only produced valid numbers would never test the default
         * case. */
        u32 nr = (next() % 16 == 0) ? next() : next() % (SYS_MAX + 4);

        /* exit() would end the run on the first call, and fork() would
         * multiply the fuzzer rather than the coverage. Both are tested
         * deliberately elsewhere; here they are the two calls that make the
         * rest unreachable. */
        if (nr == SYS_EXIT || nr == SYS_FORK)
            nr = SYS_GETPID;

        /* sleep() with a random argument would spend the whole run asleep -
         * the kernel bounds it to 10 seconds, which is correct and is not
         * something to do 40,000 times. */
        u32 a = (nr == SYS_SLEEP) ? (next() % 2) : nasty_pointer();
        u32 b = nasty_length();
        u32 c = next();

        /* See `legit` above. Both the pointer and the length are replaced,
         * not just the length: sys_write() *clamps* an oversized length to
         * SYS_WRITE_MAX rather than refusing it - which is a legitimate short
         * write, and which the first version of this guard got wrong by
         * assuming a length of 0xFFFFFFFF would be rejected. It is not; it
         * becomes 1024 and prints 1024 bytes.
         *
         * The rejection path is still covered, and by more of the generator
         * than before: every other pointer it produces is refused with the
         * length it was given. */
        if (nr == SYS_WRITE && (a == 0x00400000u || a == 0xAFFFF000u)) {
            a = (u32)(unsigned long)legit;
            b = sizeof(legit) - 1;
        }

        i32 ret = syscall3(nr, a, b, c);

        if (ret < 0)
            refused++;
        else
            accepted++;

        /* Yield occasionally, so the fuzzer is preemptible and the rest of
         * the system still runs. A ring-3 loop that never yields is itself a
         * denial of service, and the scheduler's preemption should handle it
         * - but a fuzzer that relies on being preempted to let the console
         * drain is a fuzzer whose output arrives in one burst at the end. */
        if ((i & 0x3FF) == 0)
            sys_yield();

        /* Progress, with the kernel's own uptime beside it. This is the only
         * way to see from the transcript that the run is advancing rather
         * than wedged, and dividing the two numbers is how the cost per call
         * below was measured. */
        if (i && (i & (PROGRESS_EVERY - 1)) == 0) {
            u_puts("  [fuzz] ");
            u_putu(i);
            u_puts(" calls, uptime ");
            u_putu((u32)sys_uptime());
            u_puts(" s\n");
        }
    }

    u_puts("  [fuzz] survived: ");
    u_putu(accepted);
    u_puts(" accepted, ");
    u_putu(refused);
    u_puts(" refused, 0 panics\n");

    /* The one assertion this program can make about the kernel: it is still
     * here. Everything above was an attempt to make that false. */
    u_puts("  [fuzz] the kernel is still running - that is the result\n");

    sys_exit(0);
}
