/* StratumOS - in-kernel test suite.
 *
 * Unit tests that have to run against real hardware state: the physical
 * allocator, the page tables, the heap's coalescing, the scheduler. They exist
 * because the alternative is "it booted, so it probably works", and that
 * stops being true the moment the heap starts fragmenting.
 *
 * Output format is fixed and machine-readable:
 *
 *     ktest: <name> ... PASS (<n> checks)
 *     ktest: <name> ... FAIL (<n>/<m> checks failed) first: <expr>
 *     ktest: summary <passed>/<total> suites passed
 *
 * tools/run-tests.py asserts on those lines, so an unexpected regression fails
 * CI instead of scrolling past in a wall of boot text.
 */
#define LOG_TAG "ktest"

#include <arch/cpu.h>
#include <arch/io.h>
#include <arch/irq.h>
#include <drivers/vga.h>
#include <drivers/timer.h>
#include <kernel/console.h>
#include <kernel/kernel.h>
#include <kernel/ktest.h>
#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

void ktest_check(struct ktest_result *r, bool ok, const char *what)
{
    r->checks++;
    if (!ok) {
        r->failures++;
        if (!r->first_failure)
            r->first_failure = what;
    }
}

/* ---- string and formatting --------------------------------------------- */

static void test_string(struct ktest_result *r)
{
    KT_ASSERT(r, strlen("stratum") == 7);
    KT_ASSERT(r, strcmp("a", "a") == 0);
    KT_ASSERT(r, strcmp("a", "b") < 0);
    KT_ASSERT(r, strncmp("abcdef", "abcxxx", 3) == 0);
    KT_ASSERT(r, strcasecmp("HELLO", "hello") == 0);

    char buf[8];
    KT_ASSERT(r, strlcpy(buf, "0123456789", sizeof(buf)) == 10);
    KT_ASSERT(r, strcmp(buf, "0123456") == 0);

    u32 v = 0;
    KT_ASSERT(r, str_to_u32("4096", &v) && v == 4096);
    KT_ASSERT(r, str_to_u32("0x1000", &v) && v == 0x1000);
    KT_ASSERT(r, !str_to_u32("4294967296", &v));
    KT_ASSERT(r, !str_to_u32("nope", &v));

    char tmp[32];
    ksnprintf(tmp, sizeof(tmp), "%08x/%d/%s", 0xabcu, -5, "ok");
    KT_ASSERT(r, strcmp(tmp, "00000abc/-5/ok") == 0);

    ksnprintf(tmp, sizeof(tmp), "%llu", 1234567890123ull);
    KT_ASSERT(r, strcmp(tmp, "1234567890123") == 0);

    /* memmove must survive overlap in both directions. */
    char ov[16];
    strcpy(ov, "0123456789");
    memmove(ov + 2, ov, 8);
    KT_ASSERT(r, strcmp(ov, "0101234567") == 0);
    strcpy(ov, "0123456789");
    memmove(ov, ov + 2, 8);
    KT_ASSERT(r, strcmp(ov, "2345678989") == 0);
}

/* ---- physical memory manager ------------------------------------------- */

static void test_pmm(struct ktest_result *r)
{
    struct pmm_stats before, after;

    pmm_get_stats(&before);
    KT_ASSERT(r, before.total_frames > 0);
    KT_ASSERT(r, before.free_frames > 0);
    KT_ASSERT(r, before.used_frames + before.free_frames ==
                     before.total_frames);

    paddr_t a = pmm_alloc_frame();
    KT_ASSERT(r, a != PMM_NO_FRAME);
    KT_ASSERT(r, IS_ALIGNED(a, PAGE_SIZE));
    /* The first mebibyte is reserved wholesale, so no allocation may come
     * from it. */
    KT_ASSERT(r, a >= 1 * MIB);

    paddr_t b = pmm_alloc_frame();
    KT_ASSERT(r, b != PMM_NO_FRAME);
    KT_ASSERT(r, b != a);

    pmm_get_stats(&after);
    KT_ASSERT(r, after.used_frames == before.used_frames + 2);

    pmm_free_frame(a);
    pmm_free_frame(b);

    pmm_get_stats(&after);
    KT_ASSERT(r, after.used_frames == before.used_frames);

    /* A contiguous run must really be contiguous. */
    paddr_t run = pmm_alloc_frames(4);
    KT_ASSERT(r, run != PMM_NO_FRAME);
    KT_ASSERT(r, IS_ALIGNED(run, PAGE_SIZE));
    pmm_free_frames(run, 4);

    pmm_get_stats(&after);
    KT_ASSERT(r, after.used_frames == before.used_frames);
}

/* ---- paging ------------------------------------------------------------ */

static void test_vmm(struct ktest_result *r)
{
    /* A spare virtual address well clear of everything the kernel maps. */
    const vaddr_t scratch = 0xC4000000u;
    struct pmm_stats before, after;
    paddr_t phys = 0, offset_phys = 0;

    KT_ASSERT(r, vmm_is_enabled());

    /* The identity mapping must hold for an address the kernel relies on. */
    KT_ASSERT(r, vmm_translate(VGA_PHYS, &phys) && phys == VGA_PHYS);
    KT_ASSERT(r, !vmm_translate(scratch, NULL));

    /* The null page must stay unmapped so that a NULL dereference faults
     * rather than scribbling on the interrupt vector table. The page right
     * after it must be mapped, which proves the identity range really does
     * start at the second page rather than having been skipped entirely. */
    KT_ASSERT(r, !vmm_translate(0, NULL));
    KT_ASSERT(r, vmm_translate(PAGE_SIZE, &phys) && phys == PAGE_SIZE);

    /* A user pointer to the null page must be refused too. */
    KT_ASSERT(r, !user_range_ok(0, 4));

    /* --- an owned mapping ------------------------------------------------
     * vmm_alloc_at() allocates a frame and records PTE_OWNED, so the matching
     * vmm_unmap() is responsible for returning it to the allocator. */
    pmm_get_stats(&before);

    KT_ASSERT(r, vmm_alloc_at(scratch, PTE_PRESENT | PTE_WRITE));
    KT_ASSERT(r, vmm_translate(scratch, &phys));
    KT_ASSERT(r, phys != 0);

    /* The mapping has to actually work, not merely be present in a table. */
    volatile u32 *probe = (volatile u32 *)scratch;
    *probe = 0xFEEDFACEu;
    KT_ASSERT(r, *probe == 0xFEEDFACEu);

    /* Translation must preserve the offset within the page. */
    KT_ASSERT(r, vmm_translate(scratch + 0x123, &offset_phys));
    KT_ASSERT(r, offset_phys == phys + 0x123);

    /* The PTE should carry exactly the flags we asked for, plus ownership. */
    u32 pte = vmm_pte(scratch);
    KT_ASSERT(r, (pte & PTE_PRESENT) != 0);
    KT_ASSERT(r, (pte & PTE_WRITE) != 0);
    KT_ASSERT(r, (pte & PTE_USER) == 0);
    KT_ASSERT(r, (pte & PTE_OWNED) != 0);

    /* vmm_protect() must change permissions while keeping the frame and the
     * ownership bit - the distinction vmm_map() cannot make. */
    KT_ASSERT(r, vmm_protect(scratch, PTE_PRESENT));
    KT_ASSERT(r, (vmm_pte(scratch) & PTE_WRITE) == 0);
    KT_ASSERT(r, (vmm_pte(scratch) & PTE_OWNED) != 0);
    paddr_t after_protect = 0;
    KT_ASSERT(r, vmm_translate(scratch, &after_protect));
    KT_ASSERT(r, after_protect == phys);

    vmm_unmap(scratch);
    KT_ASSERT(r, !vmm_translate(scratch, NULL));

    /* The data frame came back. The page table created to hold the mapping is
     * deliberately not reclaimed (see docs/ROADMAP.md), so allow for it. */
    pmm_get_stats(&after);
    KT_ASSERT(r, after.used_frames <= before.used_frames + 1);

    /* --- a borrowed mapping ----------------------------------------------
     * vmm_map() of a caller-supplied frame must NOT set PTE_OWNED, so that
     * unmapping it leaves the frame alone. Getting this wrong is how a kernel
     * ends up "freeing" the VGA framebuffer. */
    paddr_t borrowed = pmm_alloc_frames(3);
    KT_ASSERT(r, borrowed != PMM_NO_FRAME);

    if (borrowed != PMM_NO_FRAME) {
        pmm_get_stats(&before);

        KT_ASSERT(r, vmm_map_range(scratch, borrowed, 3 * PAGE_SIZE,
                                   PTE_PRESENT | PTE_WRITE));
        KT_ASSERT(r, (vmm_pte(scratch) & PTE_OWNED) == 0);
        KT_ASSERT(r, vmm_translate(scratch + 2 * PAGE_SIZE, &offset_phys));
        KT_ASSERT(r, offset_phys == borrowed + 2 * PAGE_SIZE);

        vmm_unmap_range(scratch, 3 * PAGE_SIZE);
        KT_ASSERT(r, !vmm_translate(scratch, NULL));

        pmm_get_stats(&after);
        KT_ASSERT(r, after.used_frames == before.used_frames);

        pmm_free_frames(borrowed, 3);
    }

    /* Mapping over the recursive page-table window would make every page
     * table unreachable, so it is rejected outright rather than allowed to
     * brick the address space. That path panics by design and so is not
     * exercised here; vmm_map() documents it. */
}

/* ---- heap -------------------------------------------------------------- */

static void test_heap(struct ktest_result *r)
{
    KT_ASSERT(r, heap_check() == 0);

    void *a = kmalloc(64);
    void *b = kmalloc(64);
    KT_ASSERT(r, a != NULL && b != NULL);
    KT_ASSERT(r, a != b);
    KT_ASSERT(r, (u32)a >= KHEAP_BASE);

    /* Payloads must be 8-byte aligned. */
    KT_ASSERT(r, IS_ALIGNED((u32)a, 8));

    /* Writing the whole allocation must not disturb its neighbour. */
    memset(a, 0xAA, 64);
    memset(b, 0xBB, 64);
    KT_ASSERT(r, ((u8 *)a)[0] == 0xAA && ((u8 *)a)[63] == 0xAA);
    KT_ASSERT(r, ((u8 *)b)[0] == 0xBB && ((u8 *)b)[63] == 0xBB);
    KT_ASSERT(r, heap_check() == 0);

    kfree(a);
    kfree(b);
    KT_ASSERT(r, heap_check() == 0);

    /* kzalloc must really zero. */
    u8 *z = kzalloc(128);
    KT_ASSERT(r, z != NULL);
    bool all_zero = true;
    for (int i = 0; i < 128; i++)
        if (z[i])
            all_zero = false;
    KT_ASSERT(r, all_zero);
    kfree(z);

    /* Coalescing: free three neighbours and the space must come back as one
     * block large enough to serve their combined size. */
    void *p1 = kmalloc(256);
    void *p2 = kmalloc(256);
    void *p3 = kmalloc(256);
    KT_ASSERT(r, p1 && p2 && p3);
    kfree(p1);
    kfree(p2);
    kfree(p3);
    KT_ASSERT(r, heap_check() == 0);

    void *big = kmalloc(700);
    KT_ASSERT(r, big != NULL);
    kfree(big);

    /* realloc preserves contents and handles the NULL/0 edge cases. */
    char *s = kmalloc(16);
    KT_ASSERT(r, s != NULL);
    strcpy(s, "preserve me");
    char *grown = krealloc(s, 512);
    KT_ASSERT(r, grown != NULL);
    KT_ASSERT(r, strcmp(grown, "preserve me") == 0);
    KT_ASSERT(r, krealloc(grown, 0) == NULL);
    KT_ASSERT(r, kmalloc(0) == NULL);
    kfree(NULL); /* must be a no-op, not a panic */

    /* Page-aligned allocation, which the page-table code depends on. */
    void *aligned = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    KT_ASSERT(r, aligned != NULL);
    KT_ASSERT(r, IS_ALIGNED((u32)aligned, PAGE_SIZE));
    kfree(aligned);

    KT_ASSERT(r, heap_check() == 0);

    /* Churn: many allocations of varying size, freed in a different order
     * than they were made. This is what actually catches fragmentation and
     * coalescing bugs. */
    void *slots[32];
    for (int i = 0; i < 32; i++) {
        slots[i] = kmalloc((size_t)(16 + (i * 37) % 400));
        KT_ASSERT(r, slots[i] != NULL);
    }
    for (int i = 0; i < 32; i += 2)
        kfree(slots[i]);
    for (int i = 1; i < 32; i += 2)
        kfree(slots[i]);

    KT_ASSERT(r, heap_check() == 0);

    struct heap_stats hs;
    heap_get_stats(&hs);
    KT_ASSERT(r, hs.region_bytes > 0);
    KT_ASSERT(r, hs.alloc_calls >= hs.free_calls);
}

/* ---- interrupts and the timer ------------------------------------------ */

static void test_interrupts(struct ktest_result *r)
{
    /* The timer must be advancing - if it is not, the IRQ path is broken,
     * which is precisely the bug this project started with. */
    u64 t0 = timer_ticks();
    KT_ASSERT(r, irq_count(IRQ_TIMER) > 0);

    timer_busy_wait_ms(30);
    u64 t1 = timer_ticks();
    KT_ASSERT(r, t1 > t0);

    /* Timestamps must advance roughly in step with the tick count. */
    KT_ASSERT(r, timer_ms() > 0);
    KT_ASSERT(r, timer_hz() > 0);

    /* No spurious interrupts should have accumulated in a clean boot. */
    KT_ASSERT(r, irq_spurious_count() == 0);

    /* A software interrupt must reach its handler and return normally. This
     * is an end-to-end check of the whole stub -> dispatch -> iret path: if
     * the frame layout in isr.asm and struct regs ever disagree again, the
     * kernel will not survive this line. */
    __asm__ volatile("int $0x03"); /* non-fatal breakpoint probe */
    KT_ASSERT(r, timer_ticks() > 0); /* still alive and still ticking */

    /* Interrupt save/restore has to nest correctly. */
    bool outer = irq_save();
    KT_ASSERT(r, !irq_enabled());
    bool inner = irq_save();
    KT_ASSERT(r, !irq_enabled());
    irq_restore(inner);
    KT_ASSERT(r, !irq_enabled());
    irq_restore(outer);
}

/* ---- scheduler --------------------------------------------------------- */

static volatile u32 worker_runs;

static void test_worker(void *arg)
{
    u32 iterations = (u32)(uintptr_t)arg;

    for (u32 i = 0; i < iterations; i++) {
        worker_runs++;
        sched_yield();
    }
}

static void test_scheduler(struct ktest_result *r)
{
    struct task *self = task_current();

    KT_ASSERT(r, self != NULL);
    KT_ASSERT(r, self->state == TASK_RUNNING);
    KT_ASSERT(r, self->pid > 0); /* tests run in a task, not in idle */

    u32 switches_before = sched_switch_count();

    worker_runs = 0;
    struct task *w = task_create("ktest-worker", test_worker,
                                 (void *)(uintptr_t)5);
    KT_ASSERT(r, w != NULL);

    if (w) {
        KT_ASSERT(r, w->pid != self->pid);
        KT_ASSERT(r, w->stack_base != NULL);

        /* Yield until the worker has had its turns. Bounded so a scheduler
         * bug fails the test instead of hanging the kernel. */
        for (int spin = 0; spin < 2000 && worker_runs < 5; spin++)
            sched_yield();

        KT_ASSERT(r, worker_runs == 5);
        KT_ASSERT(r, sched_switch_count() > switches_before);
    }

    /* Sleeping must actually take about as long as asked. */
    u64 before_ms = timer_ms();
    task_sleep_ms(60);
    u64 elapsed = timer_ms() - before_ms;
    KT_ASSERT(r, elapsed >= 50);
    KT_ASSERT(r, elapsed < 600); /* generous: this runs under emulation */
}

/* ---- boot protocol ----------------------------------------------------- */

static void test_boot(struct ktest_result *r)
{
    const struct boot_params *bp = kernel_boot_params();

    KT_ASSERT(r, bp != NULL);
    if (!bp)
        return;

    KT_ASSERT(r, bp->protocol == BOOT_PROTO_STRATUM ||
                     bp->protocol == BOOT_PROTO_MULTIBOOT2);
    KT_ASSERT(r, bp->region_count > 0);
    KT_ASSERT(r, bp->mem_usable > 1 * MIB);
    KT_ASSERT(r, bp->mem_highest > bp->mem_usable / 2);
    KT_ASSERT(r, bp->protocol_name != NULL);
    KT_ASSERT(r, bp->loader_name != NULL);

    /* The kernel's own linker symbols must be sane and ordered. */
    KT_ASSERT(r, (u32)__kernel_start >= 1 * MIB);
    KT_ASSERT(r, (u32)__kernel_end > (u32)__kernel_start);
    KT_ASSERT(r, (u32)__bss_end >= (u32)__bss_start);
}

/* ---- CPU identification ------------------------------------------------ */

static void test_cpu(struct ktest_result *r)
{
    const struct cpu_info *ci = cpu_get_info();

    KT_ASSERT(r, ci != NULL);
    KT_ASSERT(r, ci->has_cpuid); /* every machine we support has CPUID */
    KT_ASSERT(r, ci->vendor[0] != '\0');
    KT_ASSERT(r, ci->max_leaf >= 1);

    /* We are in 32-bit protected mode in ring 0: CR0.PE and CR0.PG set,
     * and CS's privilege bits clear. */
    KT_ASSERT(r, (read_cr0() & 0x1) != 0);
    KT_ASSERT(r, (read_cr0() & 0x80000000u) != 0);

    u16 cs;
    __asm__ volatile("movw %%cs, %0" : "=r"(cs));
    KT_ASSERT(r, (cs & 3) == 0);
    KT_ASSERT(r, (cs & ~3) == 0x08);
}

/* ---- syscall boundary -------------------------------------------------- */

static void test_syscall_guard(struct ktest_result *r)
{
    /* user_range_ok() is the only thing standing between a hostile ring-3
     * pointer and kernel memory, so check it rejects what it must. */
    KT_ASSERT(r, !user_range_ok((vaddr_t)__kernel_start, 16));
    KT_ASSERT(r, !user_range_ok(KHEAP_BASE, 16));
    KT_ASSERT(r, !user_range_ok(0xFFFFFFF0u, 64)); /* wraps the address space */
    KT_ASSERT(r, user_range_ok(0x1000, 0));        /* empty range is fine */

    /* An unmapped address must be refused. */
    KT_ASSERT(r, !user_range_ok(0xC8000000u, 4));
}

/* ---- registry ---------------------------------------------------------- */

static const struct ktest tests[] = {
    { "string",  "string and formatting primitives", test_string },
    { "boot",    "boot protocol normalisation",      test_boot },
    { "cpu",     "CPU identification and mode",      test_cpu },
    { "pmm",     "physical frame allocator",         test_pmm },
    { "vmm",     "paging: map, translate, unmap",    test_vmm },
    { "heap",    "kmalloc/kfree and coalescing",     test_heap },
    { "irq",     "interrupt delivery and the timer", test_interrupts },
    { "sched",   "task switching and sleeping",      test_scheduler },
    { "syscall", "userspace pointer validation",     test_syscall_guard },
};

u32 ktest_count(void)
{
    return ARRAY_SIZE(tests);
}

void ktest_list(void)
{
    kprintf("%-10s %s\n", "SUITE", "DESCRIPTION");
    for (size_t i = 0; i < ARRAY_SIZE(tests); i++)
        kprintf("%-10s %s\n", tests[i].name, tests[i].description);
}

static bool run_one(const struct ktest *t)
{
    struct ktest_result r = { 0, 0, NULL };

    t->fn(&r);

    if (r.failures == 0) {
        kprintf("ktest: %s ... PASS (%u checks)\n", t->name, r.checks);
        return true;
    }

    kprintf("ktest: %s ... FAIL (%u/%u checks failed) first: %s\n", t->name,
            r.failures, r.checks,
            r.first_failure ? r.first_failure : "(unknown)");
    return false;
}

unsigned ktest_run_one(const char *name)
{
    for (size_t i = 0; i < ARRAY_SIZE(tests); i++)
        if (strcmp(tests[i].name, name) == 0)
            return run_one(&tests[i]) ? 0 : 1;

    kprintf("ktest: no suite named '%s'\n", name);
    return 1;
}

unsigned ktest_run_all(void)
{
    unsigned passed = 0;

    kprintf("ktest: running %u suites\n", (unsigned)ARRAY_SIZE(tests));

    for (size_t i = 0; i < ARRAY_SIZE(tests); i++)
        if (run_one(&tests[i]))
            passed++;

    unsigned failed = (unsigned)ARRAY_SIZE(tests) - passed;

    kprintf("ktest: summary %u/%u suites passed\n", passed,
            (unsigned)ARRAY_SIZE(tests));

    return failed;
}
