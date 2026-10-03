/* StratumOS - the RISC-V 64 kernel's entry points and its self-tests.
 *
 * Two entry points, one per privilege mode. machine_setup() runs in machine
 * mode and returns the address boot.S should mret to; supervisor_main() is
 * that address, and everything after it runs in supervisor mode with an MMU
 * available.
 *
 * The tests at the bottom are the point of this port. They are not a
 * demonstration that RISC-V works - QEMU's RISC-V works - they are a
 * measurement of how much of StratumOS came across, which is the question
 * docs/PORTING.md exists to answer. So they exercise the *shared* code:
 * kernel/core/printf.c, kernel/core/string.c and kernel/core/div64.c,
 * compiled from the same files the x86 kernel links, against the same
 * assertions.
 */
#define LOG_TAG "boot"

#include <arch/riscv64/riscv.h>

#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/string.h>

/* ---- a check harness -------------------------------------------------- */
/*
 * Deliberately not kernel/core/ktest.c. That file is one translation unit
 * holding every x86 suite - the APIC, the page tables, the ATA driver - and
 * pulling it in would mean porting all of it or carving it up. Carving it up
 * is the right answer for a real port and is a refactor of the x86 side, so
 * docs/PORTING.md lists it rather than this file pretending it was free.
 *
 * What is shared is the thing being tested, not the harness.
 */
static u32 checks, failures;

#define CHECK(cond)                                                         \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            kprintf("rvtest: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        checks++;                                                              \
        u64 _a = (u64)(a), _b = (u64)(b);                                      \
        if (_a != _b) {                                                        \
            failures++;                                                        \
            kprintf("rvtest: FAIL %s:%d: %s == %s (%llu vs %llu)\n", __FILE__, \
                    __LINE__, #a, #b, _a, _b);                                 \
        }                                                                      \
    } while (0)

/* ---- the shared formatter --------------------------------------------- */

static void test_shared_printf(void)
{
    char buf[128];

    /* kernel/core/printf.c, unmodified, on a 64-bit big-endian-agnostic
     * target with a different calling convention and a different va_list
     * implementation. If the formatter had any x86 in it, this is where it
     * would show. */
    CHECK_EQ(ksnprintf(buf, sizeof(buf), "%d", -42), 3);
    CHECK(strcmp(buf, "-42") == 0);

    ksnprintf(buf, sizeof(buf), "%u %x %X %o", 4294967295u, 0xDEADBEEFu,
              0xCAFEu, 64u);
    CHECK(strcmp(buf, "4294967295 deadbeef CAFE 100") == 0);

    ksnprintf(buf, sizeof(buf), "%08x|%-8d|%+d|% d", 0x1234u, 7, 7, 7);
    CHECK(strcmp(buf, "00001234|7       |+7| 7") == 0);

    ksnprintf(buf, sizeof(buf), "%s|%c|%%", "abc", 'z');
    CHECK(strcmp(buf, "abc|z|%") == 0);

    /* 64-bit conversions, which on the x86 build go through div64.c's
     * software division and here compile to a single `divu` - the same
     * source, two completely different instruction sequences, one answer. */
    ksnprintf(buf, sizeof(buf), "%llu", 18446744073709551615ull);
    CHECK(strcmp(buf, "18446744073709551615") == 0);
    ksnprintf(buf, sizeof(buf), "%llx", 0x0123456789ABCDEFull);
    CHECK(strcmp(buf, "123456789abcdef") == 0);

    /* %p is where a 32-bit assumption would hide: the x86 build prints eight
     * hex digits and this one has to print sixteen, from the same code. */
    ksnprintf(buf, sizeof(buf), "%p", (void *)(uptr)0x80000000ul);
    CHECK(strlen(buf) >= 10);
    /* The low eight digits are the part a 32-bit build would also produce,
     * so finding them at the *end* is what distinguishes a 64-bit pointer
     * printed correctly from one that was truncated. */
    CHECK(strcmp(buf + strlen(buf) - 8, "80000000") == 0);

    /* Truncation has to be exact, and the return value has to be the length
     * that *would* have been written - the snprintf contract, which is easy
     * to get subtly wrong and is tested identically on both architectures. */
    CHECK_EQ(ksnprintf(buf, 4, "abcdef"), 6);
    CHECK(strcmp(buf, "abc") == 0);

    /* Precision and width taken as arguments. */
    ksnprintf(buf, sizeof(buf), "%.*s|%*d", 2, "hello", 5, 42);
    CHECK(strcmp(buf, "he|   42") == 0);
}

/* ---- the shared string layer ------------------------------------------ */

static void test_shared_string(void)
{
    char a[32], b[32];

    CHECK_EQ(strlen("stratum"), 7);
    CHECK_EQ(strnlen("stratum", 3), 3);
    CHECK(strcmp("abc", "abc") == 0);
    CHECK(strcmp("abc", "abd") < 0);
    CHECK(strncmp("abcdef", "abcxxx", 3) == 0);
    CHECK(strcasecmp("StRaTuM", "stratum") == 0);

    strlcpy(a, "hello", sizeof(a));
    CHECK(strcmp(a, "hello") == 0);

    /* strlcpy must always terminate and must report the length it wanted,
     * which is the whole reason it exists instead of strncpy. */
    CHECK_EQ(strlcpy(b, "0123456789", 5), 10);
    CHECK(strcmp(b, "0123") == 0);

    memset(a, 0x5A, sizeof(a));
    CHECK_EQ((u8)a[0], 0x5Au);
    CHECK_EQ((u8)a[31], 0x5Au);

    /* memmove with overlapping ranges, in both directions. This is the
     * function the ELF fuzzer found calling memcpy with overlapping
     * arguments in phase 7; the fix was in shared code, so it is shared
     * here, and so is the test for it. */
    strlcpy(a, "0123456789", sizeof(a));
    memmove(a + 2, a, 8);
    /* "01" then the first eight characters moved up by two. Worked out by
     * hand and got wrong the first time, which the RISC-V run caught - the
     * x86 suite tests memmove differently and had never checked this exact
     * overlap. */
    CHECK(strncmp(a, "0101234567", 10) == 0);

    strlcpy(a, "0123456789", sizeof(a));
    memmove(a, a + 2, 8);
    CHECK(strncmp(a, "2345678989", 10) == 0);

    CHECK(memcmp("abc", "abc", 3) == 0);
    CHECK(memcmp("abc", "abd", 3) != 0);
    CHECK(strchr("hello", 'l') != NULL);
    CHECK(strchr("hello", 'z') == NULL);

    u32 v = 0;

    CHECK(str_to_u32("12345", &v) && v == 12345u);
    CHECK(str_to_u32("0x1f", &v) && v == 0x1Fu);
    CHECK(!str_to_u32("", &v));
    CHECK(!str_to_u32("12z", &v));
}

/* ---- the architecture's own pieces ----------------------------------- */

static void test_traps(void)
{
    u64 before = trap_total();

    /* A deliberate ecall from supervisor mode. The handler steps over it, so
     * execution continues on the next line - which is the thing being
     * tested, because sepc points *at* the trapping instruction rather than
     * past it the way an x86 trap gate's saved EIP does. */
    u64 ecalls_before = trap_count(EXC_ECALL_S);

    __asm__ volatile("ecall");

    CHECK_EQ(trap_count(EXC_ECALL_S), ecalls_before + 1);
    CHECK(trap_total() > before);

    /* And an illegal instruction, which is a different cause through the
     * same vector. `unimp` is the canonical always-illegal encoding. */
    u64 illegal_before = trap_count(EXC_ILLEGAL_INST);

    __asm__ volatile("unimp");

    CHECK_EQ(trap_count(EXC_ILLEGAL_INST), illegal_before + 1);
}

static void test_timer(void)
{
    /* mtime is a real monotonic counter, not a tick count, so this reads a
     * clock rather than a software variable. */
    u64 t0 = timer_now();
    u64 t1 = timer_now();

    CHECK(t1 >= t0);

    /* Wait for the machine-mode timer interrupt to fire at least twice. It
     * is handled in M-mode while this code runs in S-mode, so a tick
     * arriving here is evidence that the privilege transition left machine
     * interrupts working - which is the part that is easy to break. */
    u64 start_ticks = timer_ticks();
    u64 deadline = timer_now() + VIRT_TIMEBASE_HZ / 4; /* 250 ms */

    while (timer_ticks() < start_ticks + 2 && timer_now() < deadline)
        ;

    CHECK(timer_ticks() >= start_ticks + 2);

    /* And the clock advanced by roughly the time those ticks represent. Two
     * ticks at 100 Hz is 20 ms, so at 10 MHz that is 200000 counts; the
     * bound is loose because the loop above exits on the *second* tick and
     * the first could have been imminent. */
    CHECK(timer_now() - t0 >= VIRT_TIMEBASE_HZ / 1000);
}

static void test_paging(void)
{
    CHECK(paging_enabled());
    CHECK(paging_root() != 0);

    /* The identity gigapages resolve, and resolve to themselves. */
    u64 pa = 0;

    CHECK(paging_translate(VIRT_RAM_BASE, &pa));
    CHECK_EQ(pa, VIRT_RAM_BASE);
    CHECK(paging_translate(VIRT_UART0, &pa));
    CHECK_EQ(pa, VIRT_UART0);

    /* The offset within a gigapage has to come out right, which is the part
     * of a large-page translation that is easy to get wrong - the mask is
     * 30 bits wide here, not 12. */
    CHECK(paging_translate(VIRT_RAM_BASE + 0x12345, &pa));
    CHECK_EQ(pa, VIRT_RAM_BASE + 0x12345);

    /* Now the real test: a 4 KiB page at a high virtual address, which
     * exercises all three levels of the walk rather than stopping at the
     * root. The physical page is this kernel's own .bss, so a write through
     * the new virtual address has to be visible through the identity
     * mapping - which is the only way to prove the mapping points where it
     * says. */
    static u64 scratch[512] ALIGNED(4096);
    const u64 high_va = 0xFFFFFFD000001000ul;
    u64 scratch_pa = (u64)(uptr)scratch;

    CHECK(sv39_canonical(high_va));
    CHECK(paging_map_page(high_va, scratch_pa, PTE_R | PTE_W));

    CHECK(paging_translate(high_va, &pa));
    CHECK_EQ(pa, scratch_pa);

    /* Write through the high address, read through the identity one. */
    volatile u64 *through_map = (volatile u64 *)(uptr)high_va;

    scratch[0] = 0;
    *through_map = 0x5452415455004F53ull;
    CHECK_EQ(scratch[0], 0x5452415455004F53ull);

    /* And the other direction, so this is not a one-way coincidence. */
    scratch[1] = 0xFEEDFACECAFEBEEFull;
    CHECK_EQ(through_map[1], 0xFEEDFACECAFEBEEFull);

    /* The rejections below log at ERROR level on purpose, and a CI harness
     * is right to treat an ERROR line as a failure - so they go inside an
     * expected-error window, exactly as the x86 suites do.
     *
     * log_expect_errors() is in the shared kernel/core/log.c and came across
     * with it. Nothing in this file had to implement it. */
    log_expect_errors(true);

    /* A non-canonical address is refused rather than silently truncated.
     * This is the check with no x86 counterpart - 32-bit x86 has no such
     * rule, so this is new code rather than ported code. */
    CHECK(!sv39_canonical(0x0000800000000000ul));
    CHECK(!paging_map_page(0x0000800000000000ul, scratch_pa, PTE_R));

    /* An unmapped address does not translate. */
    CHECK(!paging_translate(0xFFFFFFD000002000ul, &pa));

    /* And mapping inside an existing gigapage is refused rather than
     * corrupting it, because splitting a large page is an operation this
     * VMM does not have. */
    CHECK(!paging_map_page(VIRT_RAM_BASE + 0x200000, scratch_pa, PTE_R));

    /* Two of those three had to log to be refused, so the count is part of
     * the assertion: a window that swallowed nothing would mean a rejection
     * happened silently. */
    CHECK_EQ(log_expected_errors(), 2u);
    log_expect_errors(false);
}

/* ---- machine mode ----------------------------------------------------- */

void supervisor_main(u64 hart, u64 dtb);

/* Returns the address boot.S should mret to. */
u64 machine_setup(u64 hart, u64 dtb)
{
    UNUSED(dtb);

    uart_init();
    trap_init_machine();

    /* Delegate to supervisor mode everything a supervisor kernel should
     * handle. Without this, an ecall from S-mode arrives at mtvec and the
     * supervisor's own vector is never used - which looks like the
     * supervisor handler being broken.
     *
     * The machine timer is deliberately *not* delegated: mtimecmp is an
     * M-mode device and the architecture does not permit delegating machine
     * interrupts. That is why this port has two vectors. */
    csr_write(medeleg,
              (1ul << EXC_INST_MISALIGNED) | (1ul << EXC_BREAKPOINT) |
                  (1ul << EXC_ILLEGAL_INST) | (1ul << EXC_LOAD_MISALIGNED) |
                  (1ul << EXC_STORE_MISALIGNED) | (1ul << EXC_ECALL_U) |
                  (1ul << EXC_ECALL_S) | (1ul << EXC_INST_PAGE_FAULT) |
                  (1ul << EXC_LOAD_PAGE_FAULT) | (1ul << EXC_STORE_PAGE_FAULT));
    csr_write(mideleg, 0);

    timer_init_machine();

    kprintf("\n");
    kprintf("  .-----------------------------------------------------.\n");
    kprintf("  | StratumOS %-7s -  riscv64 (rv64imac) on QEMU virt |\n",
            STRATUM_VERSION);
    kprintf("  '-----------------------------------------------------'\n");

    pr_info("hart %llu, device tree at %p", hart, (void *)(uptr)dtb);
    pr_info("machine mode: mtvec installed, CLINT timer at 100 Hz, "
            "10 exception(s) delegated to supervisor mode");
    pr_info("misa %p, mvendorid %llu", (void *)(uptr)csr_read(misa),
            csr_read(mvendorid));

    return (u64)(uptr)supervisor_main;
}

/* ---- supervisor mode -------------------------------------------------- */

void supervisor_main(u64 hart, u64 dtb)
{
    UNUSED(hart);
    UNUSED(dtb);

    pr_info("supervisor mode reached by mret; sstatus %p",
            (void *)(uptr)csr_read(sstatus));

    trap_init_supervisor();
    pr_info("stvec installed");

    if (!paging_init()) {
        pr_err("paging did not come up");
        machine_exit(false, 1);
    }

    /* Supervisor interrupts on. The machine timer is handled in M-mode
     * regardless, so this is for completeness rather than necessity - but a
     * kernel that leaves them off and then wonders why nothing arrives is a
     * real way to spend an afternoon. */
    csr_set(sstatus, MSTATUS_SIE);

    kprintf("\n");
    kprintf("rvtest: running the *shared* code, compiled from the same "
            "sources the x86 kernel links\n");

    test_shared_printf();
    kprintf("rvtest: core/printf.c ... %s\n", failures ? "FAIL" : "PASS");
    u32 after_printf = failures;

    test_shared_string();
    kprintf("rvtest: core/string.c ... %s\n",
            failures > after_printf ? "FAIL" : "PASS");
    u32 after_string = failures;

    kprintf("rvtest: running the riscv64 arch layer\n");

    test_traps();
    kprintf("rvtest: traps       ... %s\n",
            failures > after_string ? "FAIL" : "PASS");
    u32 after_traps = failures;

    test_timer();
    kprintf("rvtest: timer       ... %s\n",
            failures > after_traps ? "FAIL" : "PASS");
    u32 after_timer = failures;

    test_paging();
    kprintf("rvtest: sv39        ... %s\n",
            failures > after_timer ? "FAIL" : "PASS");

    kprintf("\n");
    pr_info("traps taken: %llu total, %llu timer, %llu ecall, %llu illegal",
            trap_total(), trap_count(IRQ_M_TIMER), trap_count(EXC_ECALL_S),
            trap_count(EXC_ILLEGAL_INST));
    pr_info("timer: %llu tick(s), mtime %llu", timer_ticks(), timer_now());

    kprintf("rvtest: summary %u check(s), %u failure(s)\n", checks, failures);

    if (failures) {
        kprintf("rvtest: THERE WERE FAILURES\n");
        machine_exit(false, 1);
    }

    kprintf("rvtest: ALL CHECKS PASSED\n");
    pr_info("StratumOS %s is up on riscv64 and shutting down", STRATUM_VERSION);
    machine_exit(true, 0);
}
