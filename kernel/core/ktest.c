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
#include <arch/harden.h>
#include <arch/io.h>
#include <arch/irq.h>

#include <drivers/ata.h>
#include <drivers/timer.h>
#include <drivers/vga.h>

#include <kernel/console.h>
#include <kernel/elf.h>
#include <kernel/kernel.h>
#include <kernel/ksyms.h>
#include <kernel/ktest.h>
#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/profile.h>
#include <kernel/sched.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/usermode.h>

#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#include <fs/blockdev.h>
#include <fs/fat16.h>

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
    KT_ASSERT(r,
              before.used_frames + before.free_frames == before.total_frames);

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

    /* The kernel reaches low physical memory through its linear map at
     * KERNEL_VIRT_BASE, not through an identity mapping. */
    KT_ASSERT(r, vmm_translate(VGA_VIRT, &phys) && phys == VGA_PHYS);
    KT_ASSERT(r, vmm_translate(KERNEL_VIRT_BASE + PAGE_SIZE, &phys) &&
                     phys == PAGE_SIZE);
    KT_ASSERT(r, !vmm_translate(scratch, NULL));

    /* The identity mapping _start installed must be gone. If it survived,
     * user space would be unusable and a NULL dereference would land on the
     * real-mode interrupt vector table instead of faulting. */
    KT_ASSERT(r, !vmm_translate(0, NULL));
    KT_ASSERT(r, !vmm_translate(PAGE_SIZE, NULL));
    KT_ASSERT(r, !vmm_translate(KERNEL_PHYS_BASE, NULL));

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
    __asm__ volatile("int $0x03");   /* non-fatal breakpoint probe */
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
    struct task *w =
        task_create("ktest-worker", test_worker, (void *)(uintptr_t)5);
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

    /* Sleeping must not return early, and must return.
     *
     * Only the lower bound is a real correctness property: a sleep that wakes
     * before its deadline is a broken sleep. The upper bound exists purely to
     * catch a hang, so it is deliberately loose. An earlier version capped it
     * at 600 ms, which turned host contention - several QEMU instances sharing
     * a CI runner - into an intermittent failure. A tight wall-clock bound
     * inside an emulator is not a test of the kernel; it is a test of the
     * machine the emulator is running on. */
    u64 before_ms = timer_ms();
    task_sleep_ms(60);
    u64 elapsed = timer_ms() - before_ms;
    KT_ASSERT(r, elapsed >= 50);
    KT_ASSERT(r, elapsed < 30000);
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

    /* The loader's strings must live in kernel memory, not in the loader's.
     * Merely pointing at them works for exactly as long as the identity
     * mapping survives, and then faults the first time anything prints them -
     * which is the bug this assertion exists to catch. */
    KT_ASSERT(r, is_kernel_address((u32)bp->protocol_name));
    KT_ASSERT(r, is_kernel_address((u32)bp->loader_name));
    KT_ASSERT(r, is_kernel_address((u32)bp->cmdline));

    /* ...and must still be readable, which is the part a pointer check
     * alone would not establish. */
    KT_ASSERT(r, strlen(bp->loader_name) > 0);
    KT_ASSERT(r, strlen(bp->loader_name) < 192);
    KT_ASSERT(r, strlen(bp->cmdline) < 192);

    /* The kernel's own linker symbols must be sane and ordered. */
    KT_ASSERT(r, (u32)__kernel_end > (u32)__kernel_start);
    KT_ASSERT(r, (u32)__bss_end >= (u32)__bss_start);

    /* The kernel is linked in the higher half and loaded low. Both halves of
     * that statement are checkable at run time. */
    KT_ASSERT(r, (u32)__kernel_start >= KERNEL_VIRT_BASE);
    KT_ASSERT(r, (u32)__text_start >= KERNEL_VIRT_BASE);
    KT_ASSERT(r, (u32)__kernel_phys_start == KERNEL_PHYS_BASE);
    KT_ASSERT(r, (u32)__kernel_phys_end > (u32)__kernel_phys_start);
    /* .boot is the one section that is not relocated, because it runs with
     * paging off. */
    KT_ASSERT(r, (u32)__boot_start == KERNEL_PHYS_BASE);
    KT_ASSERT(r, (u32)__boot_end > (u32)__boot_start);
    KT_ASSERT(r, (u32)__boot_end < KERNEL_VIRT_BASE);

    /* phys/virt translation must round-trip across the linear map. */
    KT_ASSERT(r, virt_to_phys(phys_to_virt(0x1234000)) == 0x1234000);
    KT_ASSERT(r, phys_to_virt(0) == (void *)KERNEL_VIRT_BASE);
    KT_ASSERT(r, is_kernel_address(KERNEL_VIRT_BASE));
    KT_ASSERT(r, !is_kernel_address(KERNEL_VIRT_BASE - 1));
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

    /* Anything at or above KERNEL_VIRT_BASE is the kernel's. */
    KT_ASSERT(r, !user_range_ok(KERNEL_VIRT_BASE, 4));
    KT_ASSERT(r, !user_range_ok(0xC8000000u, 4));

    /* A range that *straddles* the boundary must be refused on the strength
     * of its end, not just its start - otherwise a user pointer a few bytes
     * below the kernel becomes a kernel write of arbitrary length. */
    KT_ASSERT(r, !user_range_ok(KERNEL_VIRT_BASE - 2, 8));
    KT_ASSERT(r, !user_range_ok(KERNEL_VIRT_BASE - PAGE_SIZE, 2 * PAGE_SIZE));
}

/* ---- symbol table and profiler ---------------------------------------- */

/* A function whose address is taken, so the linker cannot discard it and the
 * symbol table is guaranteed to contain it. */
static void ktest_known_function(void)
{
    __asm__ volatile("" ::: "memory");
}

static void test_ksyms(struct ktest_result *r)
{
    KT_ASSERT(r, ksyms_available());
    KT_ASSERT(r, ksym_count > 100); /* a real kernel has hundreds */

    /* The table must be sorted by address, or the binary search is wrong. */
    bool sorted = true;
    for (u32 i = 1; i < ksym_count; i++)
        if (ksym_table[i].addr < ksym_table[i - 1].addr)
            sorted = false;
    KT_ASSERT(r, sorted);

    /* An exact function address resolves to that function at offset 0. */
    u32 offset = 0xFFFFFFFFu;
    const char *name = ksym_lookup((u32)ktest_known_function, &offset);
    KT_ASSERT(r, name != NULL);
    KT_ASSERT(r, offset == 0);
    if (name)
        KT_ASSERT(r, strcmp(name, "ktest_known_function") == 0);

    /* An address a few bytes in resolves to the same function, at an
     * offset - this is what makes a backtrace readable. */
    offset = 0;
    name = ksym_lookup((u32)ktest_known_function + 2, &offset);
    KT_ASSERT(r, name != NULL);
    KT_ASSERT(r, offset == 2);

    /* Addresses outside every executable section belong to no function.
     * Resolving them anyway would make a backtrace confidently wrong. */
    KT_ASSERT(r, ksym_lookup(0, NULL) == NULL);
    KT_ASSERT(r, ksym_lookup(0xFFFFF000u, NULL) == NULL);
    KT_ASSERT(r, ksym_index((u32)__kernel_start - 0x1000) < 0);

    /* Every symbol must resolve to itself. */
    bool all_self = true;
    for (u32 i = 0; i < ksym_count; i++) {
        int idx = ksym_index(ksym_table[i].addr);
        if (idx < 0 || ksym_table[idx].addr != ksym_table[i].addr)
            all_self = false;
    }
    KT_ASSERT(r, all_self);
}

static void test_profile(struct ktest_result *r)
{
    struct profile_stats before, after;

    KT_ASSERT(r, profile_start());
    if (!profile_active())
        return;

    profile_reset();
    profile_get_stats(&before);
    KT_EQ(r, before.samples, 0u);

    /* Feed the attribution path synthetic frames rather than waiting for real
     * timer ticks: that tests the logic in microseconds instead of seconds,
     * and makes the expected counts exact. */
    struct regs fake;
    memset(&fake, 0, sizeof(fake));

    /* A ring-0 frame inside a known function must be attributed. */
    fake.cs = 0x08;
    fake.eip = (u32)ktest_known_function;
    profile_tick(&fake);

    profile_get_stats(&after);
    KT_EQ(r, after.samples, 1u);

    /* A ring-3 frame must be counted separately - its EIP means nothing in
     * the kernel's symbol table. */
    fake.cs = 0x1B; /* user code selector, RPL 3 */
    profile_tick(&fake);
    profile_get_stats(&after);
    KT_EQ(r, after.user_samples, 1u);
    KT_EQ(r, after.samples, 1u); /* unchanged */

    /* A ring-0 frame outside .text is unattributable, not misattributed. */
    fake.cs = 0x08;
    fake.eip = 0x20;
    profile_tick(&fake);
    profile_get_stats(&after);
    KT_EQ(r, after.unknown, 1u);
    KT_EQ(r, after.samples, 1u);

    profile_stop();
    KT_ASSERT(r, !profile_active());

    /* A stopped profiler must ignore ticks entirely. */
    fake.eip = (u32)ktest_known_function;
    profile_tick(&fake);
    profile_get_stats(&after);
    KT_EQ(r, after.samples, 1u);

    profile_reset();
}

/* ---- the user ELF loader ---------------------------------------------- */

extern const u8 _binary_init_elf_start[];
extern const u8 _binary_init_elf_end[];

static void test_elf(struct ktest_result *r)
{
    const char *why = NULL;
    size_t size = (size_t)(_binary_init_elf_end - _binary_init_elf_start);

    /* The program the kernel actually ships must validate. */
    KT_ASSERT(r, size > sizeof(struct elf32_header));
    KT_ASSERT(r, elf_validate(_binary_init_elf_start, size, &why));

    /* Rejections. Each of these is a shape of malformed or hostile image the
     * loader must refuse rather than index into. */
    KT_ASSERT(r, !elf_validate(NULL, 0, &why));
    KT_ASSERT(r, !elf_validate(_binary_init_elf_start, 8, &why));

    /* A copy we can corrupt. */
    struct elf32_header *bad = kmalloc(sizeof(*bad) + 256);
    KT_ASSERT(r, bad != NULL);
    if (!bad)
        return;

    memcpy(bad, _binary_init_elf_start, sizeof(*bad));

    u32 saved = bad->magic;
    bad->magic = 0xDEADBEEF;
    KT_ASSERT(r, !elf_validate(bad, sizeof(*bad) + 256, &why));
    bad->magic = saved;

    bad->class = 2; /* ELFCLASS64 */
    KT_ASSERT(r, !elf_validate(bad, sizeof(*bad) + 256, &why));
    bad->class = 1;

    bad->machine = 0x3E; /* EM_X86_64 */
    KT_ASSERT(r, !elf_validate(bad, sizeof(*bad) + 256, &why));
    bad->machine = EM_386;

    bad->type = 3; /* ET_DYN - a PIE needs a dynamic loader */
    KT_ASSERT(r, !elf_validate(bad, sizeof(*bad) + 256, &why));
    bad->type = ET_EXEC;

    /* A program header table that claims to extend past the image. This is
     * the classic way a loader is made to read out of bounds. */
    bad->phnum = 60000;
    KT_ASSERT(r, !elf_validate(bad, sizeof(*bad) + 256, &why));

    bad->phnum = 1;
    bad->phoff = 0xFFFF0000u;
    KT_ASSERT(r, !elf_validate(bad, sizeof(*bad) + 256, &why));

    bad->phoff = 52;
    bad->phentsize = 4; /* smaller than a program header */
    KT_ASSERT(r, !elf_validate(bad, sizeof(*bad) + 256, &why));

    /* Valid again, to confirm the checks above were the only objections. */
    bad->phentsize = sizeof(struct elf32_phdr);
    KT_ASSERT(r, elf_validate(bad, sizeof(*bad) + 256, &why));

    kfree(bad);

    /* Every rejection must have produced a reason; a loader that refuses an
     * image without saying why is a loader nobody can debug. */
    KT_ASSERT(r, why != NULL);
}

/* ---- registry ---------------------------------------------------------- */

/* ---- address spaces and copy-on-write ---------------------------------
 *
 * The user-visible version of this lives in user/init.c, where a forked child
 * writes to an inherited page and the parent proves its own copy is intact.
 * That test is the one that matters, but it can only observe the outcome.
 * This one runs inside the kernel, where the page table entries, the frame
 * reference counts and the fault counters are all visible, so it can check
 * the mechanism rather than the result: that the clone marked the page in
 * *both* address spaces, that the reference count rose, that the write
 * allocated exactly one new frame, and that tearing the clone down handed the
 * shared frame back.
 *
 * It runs in an address space of its own, created and destroyed here, for the
 * obvious reason that a test which clones the kernel's address space and then
 * writes to the result is not a test anybody should run twice.
 */
/* Read and write a *user* page from ring 0.
 *
 * Both have to declare the access, because with SMAP enabled the CPU refuses
 * an undeclared one - which is the entire point of the feature, and which it
 * demonstrated by faulting this test the first time SMAP was switched on.
 * The kernel's own user accesses are wrapped the same way; see
 * arch/harden.h.
 *
 * The write may fault, if the page is copy-on-write. That nests a window
 * inside a window, which is fine: AC is part of EFLAGS, so the fault pushes
 * it and the IRET at the end of the handler restores it. */
static u32 upeek(const volatile u32 *p)
{
    user_access_begin();
    u32 v = *p;
    user_access_end();
    return v;
}

static void upoke(volatile u32 *p, u32 v)
{
    user_access_begin();
    *p = v;
    user_access_end();
}

static void test_address_spaces(struct ktest_result *r)
{
    struct task *self = task_current();
    const paddr_t kernel_pd = vmm_kernel_pd_phys();
    const vaddr_t up = USER_IMAGE_BASE;
    struct pmm_stats pm_before, pm_after;
    struct vmm_stats vs_before, vs_after;

    /* The shell task running this is a kernel thread, so it starts in the
     * kernel's address space. If that is not true the rest of the test would
     * be destroying somebody's process. */
    KT_ASSERT(r, self->page_dir == kernel_pd);
    if (self->page_dir != kernel_pd)
        return;

    pmm_get_stats(&pm_before);

    paddr_t pd = vmm_create_address_space();
    KT_ASSERT(r, pd != 0);
    KT_ASSERT(r, pd != kernel_pd);
    if (!pd)
        return;

    /* Record it before loading CR3. A preemption between the two would
     * otherwise put us back in the kernel's address space without the code
     * below noticing. */
    self->page_dir = pd;
    vmm_switch_address_space(pd);

    /* The kernel half came across. The strongest evidence is that this
     * function is still executing, but check the mappings the kernel reaches
     * by physical address too - those are the ones a half-copied directory
     * would lose. */
    paddr_t phys = 0;
    KT_ASSERT(r, vmm_translate(VGA_VIRT, &phys) && phys == VGA_PHYS);
    KT_ASSERT(r, vmm_translate((vaddr_t)&test_address_spaces, NULL));
    KT_ASSERT(r, vmm_translate(KHEAP_BASE, NULL));

    /* The user half did not. */
    KT_EQ(r, vmm_count_user_pages(), 0u);
    KT_ASSERT(r, !vmm_translate(up, NULL));

    /* --- one user page, with something recognisable in it ---------------- */
    KT_ASSERT(r, vmm_alloc_at(up, PTE_PRESENT | PTE_WRITE | PTE_USER));
    volatile u32 *probe = (volatile u32 *)up;
    upoke(probe, 0x00C0FFEEu);
    KT_EQ(r, vmm_count_user_pages(), 1u);

    paddr_t shared = 0;
    KT_ASSERT(r, vmm_translate(up, &shared));
    KT_EQ(r, pmm_frame_refs(shared), 1);

    vmm_get_stats(&vs_before);

    /* --- clone it ------------------------------------------------------- */
    paddr_t child = vmm_clone_current();
    KT_ASSERT(r, child != 0);
    KT_ASSERT(r, child != pd);

    if (child) {
        /* The clone had to make the page read-only in the *parent* as well.
         * Marking only the child would mean the parent's next write went
         * straight through into a page the child is still reading - the
         * single most likely way to get copy-on-write subtly wrong. */
        u32 pte = vmm_pte(up);
        KT_ASSERT(r, (pte & PTE_PRESENT) != 0);
        KT_ASSERT(r, (pte & PTE_WRITE) == 0);
        KT_ASSERT(r, (pte & PTE_COW) != 0);
        KT_ASSERT(r, (pte & PTE_USER) != 0);

        /* Two address spaces hold the frame, and it is still readable. */
        KT_EQ(r, pmm_frame_refs(shared), 2);
        KT_EQ(r, upeek(probe), 0x00C0FFEEu);

        /* --- the write that forces the copy ----------------------------- */
        upoke(probe, 0x0BADC0DEu);

        vmm_get_stats(&vs_after);
        KT_ASSERT(r, vs_after.cow_faults > vs_before.cow_faults);
        KT_ASSERT(r, vs_after.cow_copies > vs_before.cow_copies);
        KT_ASSERT(r, vs_after.address_spaces > vs_before.address_spaces);

        paddr_t private_frame = 0;
        KT_ASSERT(r, vmm_translate(up, &private_frame));
        KT_ASSERT(r, private_frame != shared);
        KT_EQ(r, upeek(probe), 0x0BADC0DEu);

        /* The copy is ours outright now: writable, no longer COW, and the
         * only reference to its frame. The old frame belongs to the child
         * alone. */
        pte = vmm_pte(up);
        KT_ASSERT(r, (pte & PTE_WRITE) != 0);
        KT_ASSERT(r, (pte & PTE_COW) == 0);
        KT_ASSERT(r, (pte & PTE_OWNED) != 0);
        KT_EQ(r, pmm_frame_refs(private_frame), 1);
        KT_EQ(r, pmm_frame_refs(shared), 1);

        /* A second write must not fault again. A COW implementation that
         * forgets to clear the bit or to flush the TLB entry copies the page
         * on every single write, which is correct and useless. */
        vmm_get_stats(&vs_before);
        upoke(probe, 0x0BADC0DEu + 1);
        vmm_get_stats(&vs_after);
        KT_EQ(r, vs_after.cow_faults, vs_before.cow_faults);
        KT_EQ(r, upeek(probe), 0x0BADC0DEu + 1);

        /* Tearing the child down releases the last reference to the frame
         * the parent used to share. */
        vmm_destroy_address_space(child);
        KT_EQ(r, pmm_frame_refs(shared), 0);
        KT_EQ(r, upeek(probe), 0x0BADC0DEu + 1);
    }

    /* --- exec's half: clear the user half, keep the kernel's ------------- */
    vmm_clear_user_space();
    KT_EQ(r, vmm_count_user_pages(), 0u);
    KT_ASSERT(r, !vmm_translate(up, NULL));
    KT_ASSERT(r, vmm_translate(VGA_VIRT, NULL));
    KT_ASSERT(r, vmm_translate(KHEAP_BASE, NULL));

    /* --- back to the kernel's address space ------------------------------ */
    self->page_dir = kernel_pd;
    vmm_switch_address_space(kernel_pd);
    vmm_destroy_address_space(pd);

    KT_ASSERT(r, !vmm_translate(up, NULL));
    KT_ASSERT(r, vmm_translate(VGA_VIRT, NULL));

    /* Nothing leaked. Everything this test allocated - the two directories,
     * their page tables, the shared frame and the private copy - came back. */
    pmm_get_stats(&pm_after);
    KT_EQ(r, pm_after.used_frames, pm_before.used_frames);
}

/* ---- fork and wait from the kernel side -------------------------------
 *
 * fork() needs a trap frame to copy, and a kernel thread has no user frame to
 * give it, so the duplication itself is tested from ring 3 in user/init.c.
 * What can be checked here is the bookkeeping either side of it: that the
 * process table accounts for what it holds, that wait() refuses to invent a
 * child, and that fork() rejects a caller with nothing to copy rather than
 * reading whatever happened to be on the stack.
 */
static void test_processes(struct ktest_result *r)
{
    struct task *self = task_current();

    KT_ASSERT(r, self != NULL);
    KT_ASSERT(r, task_count() >= 2); /* at least idle and this one */
    KT_ASSERT(r, task_count() <= TASK_MAX);

    /* A kernel thread shares the kernel's address space; only a process gets
     * one of its own. Conflating the two is how fork() ends up cloning the
     * kernel's user half. */
    KT_ASSERT(r, self->page_dir == vmm_kernel_pd_phys());
    KT_ASSERT(r, !self->user);

    /* No frame, no fork. The alternative - trusting a NULL - would have
     * fork() copy 68 bytes from address zero into a child's stack. */
    KT_EQ(r, task_fork(NULL), -1);

    /* Kernel threads are children of whoever created them, and the earlier
     * suites create several, so this task has a queue of collectable
     * children before wait() can be asked what it does with none. Draining
     * them is itself the test that wait() returns a plausible pid and a
     * plausible exit code for each, and the bound is there because a wait()
     * that invented children would otherwise spin here forever. */
    u32 collected = 0;

    for (u32 i = 0; i < TASK_MAX; i++) {
        int st = 0x7F7F;
        int pid = task_wait(&st);

        if (pid < 0)
            break;

        collected++;
        KT_ASSERT(r, pid > 0);
        KT_ASSERT(r, st != 0x7F7F); /* wait() wrote a real status */
    }

    KT_ASSERT(r, collected < TASK_MAX);

    /* Now there are none, and wait() must say so instead of blocking on a
     * child that does not exist. */
    int status = 0x5A5A;
    KT_EQ(r, task_wait(&status), -1);
    KT_EQ(r, status, 0x5A5A); /* untouched on failure */

    /* exec()'s namespace. `init` has to be in it - that is the program the
     * kernel starts - and the table has to end rather than run on. */
    bool found_init = false;
    u32 n = 0;

    for (u32 i = 0; i < 16; i++) {
        const char *name = usermode_program_name(i);

        if (!name)
            break;

        n++;
        if (strcmp(name, "init") == 0)
            found_init = true;
    }

    KT_ASSERT(r, found_init);
    KT_ASSERT(r, n >= 2); /* init, plus something for exec to switch to */
    KT_ASSERT(r, usermode_program_name(n) == NULL);
}

/* ---- the hardening switches -------------------------------------------
 *
 * Each of these is a property that is easy to claim and easy to lose. A
 * refactor that maps the kernel's text writable again, or widens a kernel
 * page directory entry to USER, or allocates a task stack from the heap,
 * breaks a mitigation without breaking anything a functional test would
 * notice - so the mitigation itself has to be an assertion.
 *
 * The negative side of these properties - that a write to the kernel's text
 * really does fault, that a stack overflow really does hit the guard page -
 * cannot be checked from inside a passing test, because the correct outcome
 * is a panic. Those are driven from the shell by `fault text` and
 * `fault stackguard`, and CI boots a kernel for each and asserts on the
 * panic.
 */
static void test_hardening(struct ktest_result *r)
{
    const struct harden_state *h = harden_get_state();

    /* CR0.WP is what makes a read-only kernel page mean anything at all. The
     * x86 default is that ring 0 may write any present page regardless of
     * its write bit, which would make the rest of this suite decorative. */
    KT_ASSERT(r, (read_cr0() & (1u << 16)) != 0);
    KT_ASSERT(r, h->wp_enabled);

    /* --- W^X for the kernel's own image -------------------------------- */
    KT_ASSERT(r, h->kernel_text_ro);

    /* Counted rather than asserted per page: one failing page is one broken
     * property, and 35 passing ones are not 35 pieces of evidence. The count
     * keeps the suite's totals meaningful. */
    u32 text_pages = 0, text_writable = 0, text_user = 0, text_absent = 0;

    for (vaddr_t va = PAGE_TRUNC((vaddr_t)__text_start);
         va < PAGE_ALIGN((vaddr_t)__rodata_end); va += PAGE_SIZE) {
        u32 pte = vmm_pte(va);

        text_pages++;
        if (!(pte & PTE_PRESENT))
            text_absent++;
        if (pte & PTE_WRITE)
            text_writable++;
        if (pte & PTE_USER)
            text_user++;
    }

    KT_ASSERT(r, text_pages > 16); /* the kernel is bigger than 64 KiB */
    KT_EQ(r, text_absent, 0u);
    KT_EQ(r, text_writable, 0u);
    KT_EQ(r, text_user, 0u);

    /* .data must still be writable, or the kernel could not run. Checking it
     * is how this suite proves it narrowed the right ranges rather than
     * everything. */
    KT_ASSERT(r, (vmm_pte((vaddr_t)__data_start) & PTE_WRITE) != 0);

    /* --- the kernel half is unreachable from ring 3 --------------------- */
    /* A directory entry's USER bit gates its whole 4 MiB, and ensure_table()
     * widens one when a page inside becomes user-accessible. That must never
     * have happened to a kernel slot: it would expose 4 MiB of kernel space
     * to ring 3 in one bit. */
    u32 kernel_slots = 0, slots_absent = 0, slots_user = 0;

    for (u32 pdi = KERNEL_PDE_FIRST; pdi < 1023; pdi++) {
        u32 pde = vmm_pde_raw(pdi);

        kernel_slots++;
        /* Every kernel slot must be backed, so that no address space can be
         * missing one. See vmm_reserve_kernel_tables(). */
        if (!(pde & PTE_PRESENT))
            slots_absent++;
        if (pde & PTE_USER)
            slots_user++;
    }

    KT_EQ(r, kernel_slots, 1023u - KERNEL_PDE_FIRST);
    KT_EQ(r, slots_absent, 0u);
    KT_EQ(r, slots_user, 0u);

    /* --- stack guard pages --------------------------------------------- */
    KT_ASSERT(r, h->stack_guard_pages);
    KT_ASSERT(r, h->stack_canaries);

    /* This task's own stack is in the guarded region, and the page below it
     * is not mapped. */
    struct task *self = task_current();

    KT_ASSERT(r, (vaddr_t)self->stack_base >= KSTACK_BASE);
    KT_ASSERT(r, (vaddr_t)self->stack_base < KSTACK_BASE + KSTACK_REGION);
    KT_ASSERT(r, vmm_translate((vaddr_t)self->stack_base, NULL));
    KT_ASSERT(r, !vmm_translate((vaddr_t)self->stack_base - PAGE_SIZE, NULL));

    /* And the page above its top, which is the next slot's guard. */
    KT_ASSERT(r, !vmm_translate(self->kernel_esp0, NULL));

    /* No task has broken its canary; the check panics, so a non-zero count
     * here would mean the counter is being incremented without the panic. */
    KT_EQ(r, sched_canary_failures(), 0u);

    /* --- SMEP and SMAP -------------------------------------------------- */
    /* Availability depends on the CPU, so what is asserted is consistency:
     * a feature the CPU has must be switched on, and one it does not have
     * must not be claimed. Under an emulator without them, this is the
     * branch that gets tested - which is worth having, since the fallback is
     * what runs on any pre-2012 machine. */
    if (h->smep_available)
        KT_ASSERT(r, h->smep_enabled && (read_cr4() & CR4_SMEP));
    else
        KT_ASSERT(r, !h->smep_enabled);

    if (h->smap_available)
        KT_ASSERT(r, h->smap_enabled && (read_cr4() & CR4_SMAP));
    else
        KT_ASSERT(r, !h->smap_enabled);

    /* A declared access to user memory works. There is nothing mapped in
     * user space in this address space, so what is actually checked is that
     * the window opens and closes and the counter moves - the refusal of an
     * *undeclared* access is what faulted this suite's sibling the first
     * time SMAP was enabled, and cannot be asserted without panicking. */
    u32 windows = h->user_access_windows;

    user_access_begin();
    user_access_end();

    if (h->smap_enabled)
        KT_ASSERT(r, h->user_access_windows == windows + 1);
    else
        KT_ASSERT(r, h->user_access_windows == windows);

    /* --- the null page, one more time ---------------------------------- */
    /* Belongs here as much as in the vmm suite: an unmapped page zero is a
     * mitigation, not an implementation detail. */
    KT_ASSERT(r, !vmm_translate(0, NULL));
    KT_ASSERT(r, (vmm_pde_raw(0) & PTE_PRESENT) == 0);
}

/* ---- the disk, the partition table and the filesystem ------------------
 *
 * These run against whatever the kernel actually booted from, which is the
 * point: the raw disk image has an ATA drive with a real MBR and a FAT16
 * partition, and the GRUB ISO has an ATAPI CD-ROM and no partition at all.
 * One suite has to pass on both, so each group checks either the real thing
 * or the absence of it - never "skip", which is how a test suite quietly
 * stops testing anything.
 */
static void test_storage(struct ktest_result *r)
{
    struct ata_stats as;

    ata_get_stats(&as);

    /* Nothing should have failed getting this far: the kernel read sector 0
     * of every drive during the partition scan. */
    KT_EQ(r, as.errors, 0u);
    KT_EQ(r, as.timeouts, 0u);

    KT_ASSERT(r, ata_drive_count() <= ATA_MAX_DRIVES);
    KT_ASSERT(r, ata_get_drive(ATA_MAX_DRIVES) == NULL);

    if (ata_drive_count() == 0) {
        /* The ISO path. The CD-ROM is ATAPI, which IDENTIFY refuses, so
         * there is no block device to read - and the block layer has to say
         * so rather than registering something unusable. */
        KT_EQ(r, blockdev_count(), 0u);
        KT_ASSERT(r, blockdev_first_fs() == NULL);
        KT_ASSERT(r, !fat16_mounted());
        return;
    }

    /* --- the drive ------------------------------------------------------ */
    const struct ata_drive *d = NULL;
    u32 found = 0;

    for (u32 i = 0; i < ATA_MAX_DRIVES; i++)
        if (ata_get_drive(i)) {
            found++;
            if (!d)
                d = ata_get_drive(i);
        }

    KT_EQ(r, found, ata_drive_count());
    KT_ASSERT(r, d != NULL);
    KT_ASSERT(r, d->sectors > 0);
    KT_ASSERT(r, d->model[0] != '\0');

    /* --- reading --------------------------------------------------------- */
    u8 *buf = kmalloc(2 * ATA_SECTOR_SIZE);

    KT_ASSERT(r, buf != NULL);
    if (!buf)
        return;

    /* Sector 0 is the MBR this kernel booted from, so its signature is known
     * before the read - which makes this a test of the read rather than of
     * whatever happened to be on the disk. */
    memset(buf, 0, ATA_SECTOR_SIZE);
    KT_ASSERT(r, ata_read(0, 0, 1, buf));
    KT_EQ(r, buf[510], 0x55);
    KT_EQ(r, buf[511], 0xAA);

    /* A multi-sector read must agree with two single-sector reads. Getting
     * the per-sector DRQ handshake wrong is the classic PIO bug, and it
     * shows up as the second sector being a copy of the first. */
    u8 *one = kmalloc(ATA_SECTOR_SIZE);

    if (one) {
        KT_ASSERT(r, ata_read(0, 0, 2, buf));
        KT_ASSERT(r, ata_read(0, 1, 1, one));
        KT_ASSERT(r, memcmp(buf + ATA_SECTOR_SIZE, one, ATA_SECTOR_SIZE) == 0);
        /* ...and the two sectors must differ, or the comparison above would
         * pass on a driver that returned the same sector twice. */
        KT_ASSERT(r, memcmp(buf, one, ATA_SECTOR_SIZE) != 0);
        kfree(one);
    }

    /* Refusals. Each of these is a request a corrupt filesystem could
     * generate, and each must be refused here rather than by the drive.
     *
     * Each also logs an error, correctly - and CI treats an unexpected ERROR
     * line as a failure, also correctly. So the window below counts them
     * instead, and the count is asserted afterwards: a refusal that happened
     * silently would be as wrong as one that did not happen. */
    log_expect_errors(true);

    KT_ASSERT(r, !ata_read(0, d->sectors, 1, buf));     /* past the end */
    KT_ASSERT(r, !ata_read(0, d->sectors - 1, 2, buf)); /* straddles it */
    KT_ASSERT(r, !ata_read(0, 0xFFFFFFFFFFFFFFFFull, 2, buf)); /* wraps   */
    KT_ASSERT(r, !ata_read(ATA_MAX_DRIVES, 0, 1, buf)); /* no such disk */
    KT_ASSERT(r, !ata_read(0, 0, 1, NULL));             /* no buffer    */

    u32 complaints = log_expected_errors();

    log_expect_errors(false);

    /* Five refusals, five complaints. An exact count rather than "more than
     * zero", because a driver that refused silently would pass the weaker
     * check and leave whoever hit it with nothing to read. */
    KT_EQ(r, complaints, 5u);

    /* Zero sectors is a no-op, not an error - so it must not have logged. */
    KT_ASSERT(r, ata_read(0, 0, 0, buf));

    /* --- the partition table ------------------------------------------- */
    KT_ASSERT(r, blockdev_count() >= 1);
    KT_ASSERT(r, blockdev_find("hd0") != NULL);
    KT_ASSERT(r, blockdev_find("nonsuch") == NULL);
    KT_ASSERT(r, blockdev_find(NULL) == NULL);

    const struct blockdev *whole = blockdev_find("hd0");

    if (whole) {
        KT_EQ(r, whole->first_lba, 0ull);
        KT_EQ(r, whole->sectors, d->sectors);
        KT_EQ(r, whole->partition_type, 0);

        log_expect_errors(true);
        KT_ASSERT(r, !blockdev_read(whole, whole->sectors, 1, buf));
        KT_ASSERT(r, !blockdev_read(NULL, 0, 1, buf));
        complaints = log_expected_errors();
        log_expect_errors(false);
        KT_EQ(r, complaints, 2u);
    }

    const struct blockdev *part = blockdev_first_fs();

    if (!part) {
        /* A disk with no FAT partition. Then nothing may be mounted, and
         * exec must be falling back to the embedded programs. */
        KT_ASSERT(r, !fat16_mounted());
        kfree(buf);
        return;
    }

    /* The partition is inside the disk, and its reads are offset by its
     * start. Forgetting that offset is the bug a single-disk layout exists
     * to catch: the filesystem would read the kernel's sectors and blame its
     * own metadata. */
    KT_ASSERT(r, part->first_lba > 0);
    KT_ASSERT(r, part->first_lba + part->sectors <= d->sectors);

    KT_ASSERT(r, blockdev_read(part, 0, 1, buf));

    u8 *direct = kmalloc(ATA_SECTOR_SIZE);

    if (direct) {
        KT_ASSERT(r, ata_read(part->drive, part->first_lba, 1, direct));
        KT_ASSERT(r, memcmp(buf, direct, ATA_SECTOR_SIZE) == 0);
        kfree(direct);
    }

    /* The partition's first sector is the FAT boot sector, not the MBR. */
    KT_EQ(r, buf[510], 0x55);
    KT_EQ(r, buf[511], 0xAA);
    KT_ASSERT(r, memcmp(buf + 54, "FAT16   ", 8) == 0);

    /* And a read past the partition's end must be refused by the block
     * layer, even though those sectors exist on the disk. This is the check
     * that keeps a filesystem inside its own partition. */
    log_expect_errors(true);
    KT_ASSERT(r, !blockdev_read(part, part->sectors, 1, buf));
    KT_ASSERT(r, !blockdev_read(part, part->sectors - 1, 2, buf));
    complaints = log_expected_errors();
    log_expect_errors(false);
    KT_EQ(r, complaints, 2u);

    kfree(buf);
}

static void test_filesystem(struct ktest_result *r)
{
    if (!fat16_mounted()) {
        /* The ISO path again. Everything has to refuse cleanly rather than
         * dereference a filesystem that was never mounted. */
        struct fat_dirent e;

        KT_ASSERT(r, !fat16_stat("/", &e));
        KT_ASSERT(r, !fat16_readdir("/", 0, &e));
        KT_ASSERT(r, fat16_read_whole("/bin/INIT", 1 * MIB, NULL) == NULL);
        KT_ASSERT(r, strcmp(fat16_device_name(), "(none)") == 0);
        return;
    }

    const struct fat_info *f = fat16_get_info();

    /* --- the geometry the BPB described -------------------------------- */
    KT_EQ(r, f->bytes_per_sector, (u16)BLOCK_SIZE);
    KT_ASSERT(r, f->sectors_per_cluster > 0);
    KT_ASSERT(r, f->cluster_bytes == (u32)f->sectors_per_cluster * BLOCK_SIZE);
    KT_ASSERT(r, f->fat_start >= f->reserved_sectors);
    KT_ASSERT(r, f->root_start > f->fat_start);
    KT_ASSERT(r, f->data_start > f->root_start);
    KT_ASSERT(r, f->data_start < f->total_sectors);
    /* FAT16's defining property is the cluster count, not the fs_type
     * string - which is a comment and is routinely wrong. */
    KT_ASSERT(r, f->cluster_count >= 4085 && f->cluster_count <= 65524);

    /* --- the root ------------------------------------------------------- */
    struct fat_dirent root;

    KT_ASSERT(r, fat16_stat("/", &root));
    KT_ASSERT(r, root.is_dir);
    KT_EQ(r, root.first_cluster, 0); /* FAT16's root has no cluster number */

    /* --- a directory, and a file inside it ----------------------------- */
    struct fat_dirent bin, init;

    KT_ASSERT(r, fat16_stat("/bin", &bin));
    KT_ASSERT(r, bin.is_dir);
    KT_ASSERT(r, bin.first_cluster >= 2);

    KT_ASSERT(r, fat16_stat("/bin/INIT", &init));
    KT_ASSERT(r, !init.is_dir);
    KT_ASSERT(r, init.size > 0);
    KT_ASSERT(r, init.first_cluster >= 2);

    /* Case-insensitive, because FAT is - and because `exec("/bin/init")`
     * has to find `INIT`. */
    struct fat_dirent lower;

    KT_ASSERT(r, fat16_stat("/bin/init", &lower));
    KT_EQ(r, lower.first_cluster, init.first_cluster);
    KT_EQ(r, lower.size, init.size);

    /* Redundant separators are not a syntax error. */
    KT_ASSERT(r, fat16_stat("//bin//INIT", &lower));
    KT_EQ(r, lower.first_cluster, init.first_cluster);

    /* --- refusals ------------------------------------------------------- */
    KT_ASSERT(r, !fat16_stat("/nosuchfile", &lower));
    KT_ASSERT(r, !fat16_stat("/bin/nosuchfile", &lower));
    KT_ASSERT(r, !fat16_stat("/bin/INIT/nonsense", &lower)); /* not a dir   */
    KT_ASSERT(r, !fat16_stat("bin/INIT", &lower));           /* relative    */
    KT_ASSERT(r, !fat16_stat("/averylongnamethatcannotfit", &lower));
    KT_ASSERT(r, !fat16_stat(NULL, &lower));
    KT_ASSERT(r, !fat16_stat("/", NULL));
    /* Reading a directory as a file, and a file as a directory. */
    KT_ASSERT(r, fat16_read(&bin, 0, &lower, 4) == -1);
    KT_ASSERT(r, !fat16_readdir("/bin/INIT", 0, &lower));

    /* --- reading --------------------------------------------------------- */
    u8 *whole = fat16_read_whole("/bin/INIT", 1 * MIB, NULL);
    u32 size = 0;

    kfree(whole);
    whole = fat16_read_whole("/bin/INIT", 1 * MIB, &size);

    KT_ASSERT(r, whole != NULL);
    KT_EQ(r, size, init.size);

    if (whole) {
        /* It is an ELF, because it is the program this kernel runs. Reading
         * the right length of the wrong file would pass every check above. */
        KT_EQ(r, whole[0], 0x7F);
        KT_ASSERT(r, memcmp(whole + 1, "ELF", 3) == 0);

        /* A chunked read must agree with the whole-file read, including
         * across a cluster boundary - which is where following the chain
         * either works or silently repeats a cluster. */
        u32 chunk_size = 100; /* deliberately not a divisor of anything */
        u8 *chunked = kmalloc(size);

        KT_ASSERT(r, chunked != NULL);

        if (chunked) {
            u32 offset = 0;
            bool ok = true;

            while (offset < size) {
                u32 want =
                    size - offset < chunk_size ? size - offset : chunk_size;
                i32 got = fat16_read(&init, offset, chunked + offset, want);

                if (got != (i32)want) {
                    ok = false;
                    break;
                }
                offset += (u32)got;
            }

            KT_ASSERT(r, ok);
            KT_EQ(r, offset, size);
            KT_ASSERT(r, memcmp(chunked, whole, size) == 0);

            /* An unaligned read straddling a cluster boundary has to return
             * the same bytes as the whole-file read at that offset. */
            if (size > f->cluster_bytes + 8) {
                u8 straddle[16];
                u32 at = f->cluster_bytes - 8;

                KT_ASSERT(r,
                          fat16_read(&init, at, straddle, sizeof(straddle)) ==
                              (i32)sizeof(straddle));
                KT_ASSERT(r,
                          memcmp(straddle, whole + at, sizeof(straddle)) == 0);
            }

            kfree(chunked);
        }

        /* Reading at and past the end. A short read at the end is correct; a
         * read past it is zero bytes, not an error. */
        u8 tail[8];

        KT_EQ(r, fat16_read(&init, size - 4, tail, sizeof(tail)), 4);
        KT_EQ(r, fat16_read(&init, size, tail, sizeof(tail)), 0);
        KT_EQ(r, fat16_read(&init, size + 1000, tail, sizeof(tail)), 0);
        KT_EQ(r, fat16_read(&init, 0, tail, 0), 0);

        kfree(whole);
    }

    /* A size bound that the file exceeds must refuse rather than truncate:
     * the length comes off the disk, and a caller that asked for at most N
     * bytes cannot be handed a buffer of N+1. */
    KT_ASSERT(r, fat16_read_whole("/bin/INIT", init.size - 1, &size) == NULL);
    KT_ASSERT(r, fat16_read_whole("/bin/INIT", init.size, &size) != NULL ||
                     init.size == 0);

    /* --- readdir -------------------------------------------------------- */
    u32 entries = 0;
    bool saw_bin = false;

    for (u32 i = 0; i < 64; i++) {
        if (!fat16_readdir("/", i, &lower))
            break;

        entries++;
        if (strcmp(lower.name, "BIN") == 0) {
            saw_bin = true;
            KT_ASSERT(r, lower.is_dir);
        }
    }

    KT_ASSERT(r, entries > 0);
    KT_ASSERT(r, saw_bin);
    /* The volume label is a root directory entry with ATTR_VOLUME_ID, and it
     * is not a file. A driver that reported it would list a phantom. */
    KT_ASSERT(r, !fat16_stat("/STRATUM", &lower));

    /* --- the single-sector cache ---------------------------------------
     *
     * Measured rather than asserted in the abstract. Reading a file in
     * 64-byte chunks touches each 512-byte sector eight times in a row, so
     * seven of every eight reads must come from the cache. Anything much
     * below that means the cache is being invalidated between calls, which
     * is the failure mode that makes a FAT walk one disk read per directory
     * entry examined.
     */
    u32 reads_before = f->reads;
    u32 hits_before = f->cache_hits;
    u8 scratch[64];
    u32 calls = 0;

    for (u32 at = 0; at + sizeof(scratch) <= init.size; at += sizeof(scratch)) {
        if (fat16_read(&init, at, scratch, sizeof(scratch)) !=
            (i32)sizeof(scratch))
            break;
        calls++;
    }

    u32 reads = f->reads - reads_before;
    u32 hits = f->cache_hits - hits_before;

    KT_ASSERT(r, calls > 16); /* the file is big enough for this to mean
                                 something */
    KT_ASSERT(r, hits > 0);
    /* Eight 64-byte reads per 512-byte sector, so misses should be about an
     * eighth of the calls. Allow a factor of two for the FAT sector reads
     * that the chain walk mixes in. */
    KT_ASSERT(r, reads < calls / 4);
    KT_ASSERT(r, hits > reads * 2);
}

static const struct ktest tests[] = {
    {"string", "string and formatting primitives", test_string},
    {"boot", "boot protocol normalisation", test_boot},
    {"cpu", "CPU identification and mode", test_cpu},
    {"pmm", "physical frame allocator", test_pmm},
    {"vmm", "paging: map, translate, unmap", test_vmm},
    {"heap", "kmalloc/kfree and coalescing", test_heap},
    {"irq", "interrupt delivery and the timer", test_interrupts},
    {"sched", "task switching and sleeping", test_scheduler},
    {"syscall", "userspace pointer validation", test_syscall_guard},
    {"elf", "user ELF validation and rejection", test_elf},
    {"vmspace", "address spaces and copy-on-write", test_address_spaces},
    {"proc", "process table, fork guards, exec namespace", test_processes},
    {"harden", "W^X, guard pages, SMEP/SMAP, kernel/user split",
     test_hardening},
    {"storage", "ATA PIO reads and the MBR partition table", test_storage},
    {"fs", "FAT16: geometry, paths, cluster chains", test_filesystem},
    {"ksyms", "embedded symbol table lookup", test_ksyms},
    {"profile", "sampling profiler attribution", test_profile},
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
    struct ktest_result r = {0, 0, NULL};

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
