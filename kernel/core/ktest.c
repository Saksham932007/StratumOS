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

#include <arch/acpi.h>
#include <arch/apic.h>
#include <arch/cpu.h>
#include <arch/harden.h>
#include <arch/io.h>
#include <arch/irq.h>
#include <arch/longmode.h>

#include <drivers/ata.h>
#include <drivers/e1000.h>
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
#include <kernel/smp.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>
#include <kernel/syscall.h>
#include <kernel/usermode.h>

#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#include <fs/blockdev.h>
#include <fs/fat16.h>
#include <fs/vfs.h>
#include <net/inet.h>
#include <net/net.h>
#include <net/socket.h>

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

    /* --- the no-execute bit, which is why this kernel uses PAE ----------
     *
     * Tested on a scratch mapping rather than on the kernel's own pages,
     * because what is being checked here is the rule vmm_map() applies: a
     * caller that says nothing gets a non-executable page, and only
     * PTE_EXEC produces an executable one. The kernel's .text and .rodata
     * are checked in the `harden` suite, which is where the policy lives.
     *
     * Both branches are real tests. On a processor without the bit,
     * vmm_nx_enabled() is false and every entry must come back *without*
     * NX set - because setting bit 63 there would make it a reserved bit
     * and fault every walk through the entry. The legacy-cpu scenario is
     * what runs that side. */
    KT_ASSERT(r, vmm_map(scratch, PAGE_SIZE, PTE_PRESENT | PTE_WRITE));

    pte_t data_pte = vmm_pte(scratch);

    KT_ASSERT(r, data_pte & PTE_PRESENT);
    KT_EQ(r, (data_pte & PTE_NX) ? 1u : 0u, vmm_nx_enabled() ? 1u : 0u);

    /* PTE_EXEC is a positive permission, so asking for it clears the
     * hardware bit. */
    KT_ASSERT(r, vmm_protect(scratch, PTE_PRESENT | PTE_EXEC));
    KT_EQ(r, (vmm_pte(scratch) & PTE_NX) ? 1u : 0u, 0u);
    KT_EQ(r, (vmm_pte(scratch) & PTE_WRITE) ? 1u : 0u, 0u);

    /* And it is not sticky: a page made writable again loses it. This is the
     * assertion that would catch vmm_protect() preserving the old flags
     * instead of replacing them, which would let a page be writable and
     * executable at once without anyone asking for that. */
    KT_ASSERT(r, vmm_protect(scratch, PTE_PRESENT | PTE_WRITE));
    KT_EQ(r, (vmm_pte(scratch) & PTE_NX) ? 1u : 0u, vmm_nx_enabled() ? 1u : 0u);

    /* The frame survived all three, because protect changes permissions and
     * nothing else. */
    KT_ASSERT(r, vmm_translate(scratch, &phys) && phys == PAGE_SIZE);

    vmm_unmap(scratch);
    KT_ASSERT(r, !vmm_translate(scratch, NULL));

    /* The directory entry above it must never carry NX: the bit is OR-ed
     * down the walk, so an NX directory entry would make its whole 2 MiB
     * non-executable whatever the page entries say. */
    KT_EQ(r,
          (vmm_pde_raw(PDPT_INDEX(scratch), PDE_INDEX(scratch)) & PTE_NX) ? 1u
                                                                          : 0u,
          0u);

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

    /* Interrupts off for the whole sequence below.
     *
     * The profiler is driven by the timer interrupt, and this test asserts
     * *exact* sample counts after feeding it synthetic frames - so a real
     * tick landing between the reset and an assertion adds a sample the test
     * did not ask for and the count is wrong.
     *
     * The window is a few microseconds and the timer fires every 10 ms, so
     * the odds were always good; the test was passing on luck rather than on
     * exclusion. Adding the network thread - which wakes 100 times a second
     * and makes the machine preempt far more often - turned that luck into a
     * reliable failure, which is the useful kind of flake: one that becomes
     * deterministic before it reaches someone else.
     *
     * A test that asserts an exact count against an asynchronous producer
     * has to exclude the producer. */
    bool irqs = irq_save();

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
    irq_restore(irqs);
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
    const paddr_t kernel_pd = vmm_kernel_root_phys();
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
    KT_ASSERT(r, self->page_dir == vmm_kernel_root_phys());
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

    /* --- W^X, the other half ---------------------------------------------
     *
     * Read-only .text was always checkable; executable-only .text was not,
     * because the 32-bit page table entry has nowhere to say it. This is
     * what PAE bought, and it is asserted as an exclusive-or rather than as
     * two separate facts: for every page of the kernel's linear map, at most
     * one of "writable" and "executable" may be true.
     *
     * On a processor with no NX bit the exclusion cannot hold - every page is
     * executable - so the assertion is made conditional on the feature and
     * the count is reported either way. The legacy-cpu scenario runs that
     * side, and `hardening` says which it took. */
    u32 wx_pages = 0, exec_pages = 0, linear_pages = 0;

    for (vaddr_t va = KERNEL_VIRT_BASE; va < KERNEL_VIRT_BASE + VMM_LINEAR_SIZE;
         va += PAGE_SIZE) {
        pte_t pte = vmm_pte(va);

        if (!(pte & PTE_PRESENT))
            continue;

        linear_pages++;
        if (!(pte & PTE_NX))
            exec_pages++;
        if ((pte & PTE_WRITE) && !(pte & PTE_NX))
            wx_pages++;
    }

    /* The window is fully backed, so this is also a check that the linear
     * map survived the W^X pass intact. */
    KT_EQ(r, linear_pages, (u32)(VMM_LINEAR_SIZE / PAGE_SIZE));

    if (vmm_nx_enabled()) {
        KT_EQ(r, wx_pages, 0u);

        /* And .text is the executable part - all of it, and not much more.
         * ".boot" holds the tables _start built and stays writable, so it is
         * non-executable too; what is left executable is .text alone. */
        KT_EQ(r, exec_pages,
              (u32)((PAGE_ALIGN((vaddr_t)__text_end) -
                     PAGE_TRUNC((vaddr_t)__text_start)) /
                    PAGE_SIZE));

        KT_EQ(r, (vmm_pte((vaddr_t)__text_start) & PTE_NX) ? 1u : 0u, 0u);
        KT_ASSERT(r, vmm_pte((vaddr_t)__rodata_start) & PTE_NX);
        KT_ASSERT(r, vmm_pte((vaddr_t)__data_start) & PTE_NX);
        KT_ASSERT(r, vmm_pte(KHEAP_BASE) & PTE_NX);

        /* This task's own kernel stack. A stack that is executable is the
         * single most useful page in the address space to an attacker who
         * can overflow one, and it is outside the linear map, so the loop
         * above does not cover it. */
        KT_ASSERT(r, vmm_pte((vaddr_t)task_current()->stack_base) & PTE_NX);
    } else {
        /* Every page is executable, including .text - which is the honest
         * statement of what this processor can enforce, and is exactly what
         * `hardening` and the W^X log line say out loud.
         *
         * The reason to assert it rather than skip is that "no NX" has to
         * mean the bit is never set anywhere. A kernel that set bit 63 on a
         * processor without the feature would be setting a *reserved* bit,
         * and every walk through that entry would fault for good. */
        KT_EQ(r, exec_pages, linear_pages);
        KT_EQ(r, (vmm_pte((vaddr_t)__data_start) & PTE_NX) ? 1u : 0u, 0u);
        KT_EQ(r, (vmm_pte(KHEAP_BASE) & PTE_NX) ? 1u : 0u, 0u);
        KT_ASSERT(r, wx_pages > 0); /* and that is the whole problem */
    }

    /* --- the kernel half is unreachable from ring 3 --------------------- */
    /* A directory entry's USER bit gates its whole 2 MiB, and ensure_table()
     * widens one when a page inside becomes user-accessible. That must never
     * have happened to a slot in the kernel's directory: it would expose
     * 2 MiB of kernel space to ring 3 in one bit.
     *
     * The slots no longer have to all be *present* - the kernel's page
     * directory is shared by pointer between address spaces now, so a table
     * created at runtime appears in all of them and there is nothing to
     * pre-reserve. What matters is that the ones that do exist are
     * supervisor-only. */
    u32 kernel_slots = 0, slots_user = 0;

    for (u32 pdi = 0; pdi < PD_SELF_SLOT; pdi++) {
        pte_t pde = vmm_pde_raw(KERNEL_PDPT_INDEX, pdi);

        if (!(pde & PTE_PRESENT))
            continue;

        kernel_slots++;
        if (pde & PTE_USER)
            slots_user++;
    }

    /* The linear map alone is eight slots, so a plausible floor. */
    KT_ASSERT(r, kernel_slots >= VMM_LINEAR_SIZE / (2 * MIB));
    KT_EQ(r, slots_user, 0u);

    /* The self-map is the one slot in the directory that points at the
     * directory, and it must be no-execute: it is writable, and every page
     * table in the gigabyte is reachable through it. */
    pte_t self_map = vmm_pde_raw(KERNEL_PDPT_INDEX, PD_SELF_SLOT);

    KT_ASSERT(r, self_map & PTE_PRESENT);
    KT_ASSERT(r, self_map & PTE_WRITE);
    KT_EQ(r, (self_map & PTE_USER) ? 1u : 0u, 0u);
    if (vmm_nx_enabled())
        KT_ASSERT(r, self_map & PTE_NX);

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
    KT_ASSERT(r, (vmm_pde_raw(0, 0) & PTE_PRESENT) == 0);
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
    if (!d)
        return; /* KT_ASSERT records and continues, so this has to be real */
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

/* ---- processors, the APIC and locks -----------------------------------
 *
 * Runs on one processor and on four, so every group checks either the real
 * thing or the absence of it. A machine with no ACPI at all is a case this
 * has to pass on too: the GRUB ISO path has ACPI, but a kernel that assumed
 * it would be there would be wrong about a 486.
 */
static void test_smp(struct ktest_result *r)
{
    const struct acpi_info *ai = acpi_get_info();

    /* --- ACPI ------------------------------------------------------------ */
    if (ai->available) {
        /* The root table parsed, so these are facts rather than guesses. */
        KT_ASSERT(r, ai->tables_seen > 0);
        KT_ASSERT(r, ai->oem_id[0] != '\0');

        if (ai->madt_found) {
            KT_ASSERT(r, ai->cpus_reported >= 1);
            KT_ASSERT(r, ai->local_apic_phys != 0);
            /* The local APIC's registers are a page, and the architecture
             * requires that page to be aligned. A misaligned base means the
             * MADT was misread. */
            KT_ASSERT(r, (ai->local_apic_phys & (PAGE_SIZE - 1)) == 0);
            KT_ASSERT(r, ai->cpu_count <= ai->cpus_reported);

            /* Every processor ACPI described has a distinct APIC id. Two
             * processors sharing one would make the id-to-index map
             * ambiguous, and the symptom would be two CPUs believing they
             * are the same CPU. */
            for (u32 i = 0; i < ai->cpu_count; i++)
                for (u32 j = i + 1; j < ai->cpu_count; j++)
                    KT_ASSERT(r, ai->cpus[i].apic_id != ai->cpus[j].apic_id);
        }

        /* A table that is not there must come back NULL rather than as
         * whatever the walk last looked at. */
        KT_ASSERT(r, acpi_find_table("ZZZZ") == NULL);
        KT_ASSERT(r, acpi_find_table(NULL) == NULL);

        /* The FADT is in every ACPI implementation, so finding it proves the
         * walk reaches more than the one table SMP needs. */
        const struct acpi_sdt_header *fadt = acpi_find_table("FACP");

        if (fadt) {
            KT_ASSERT(r, memcmp(fadt->signature, "FACP", 4) == 0);
            KT_ASSERT(r, fadt->length >= sizeof(*fadt));
        }
    }

    /* --- the local APIC -------------------------------------------------- */
    if (apic_available()) {
        KT_ASSERT(r, apic_version() != 0);

        /* The task priority register has to be zero, or this processor
         * refuses interrupts below some priority - and the symptom is a CPU
         * that comes up and then never takes one. */
        KT_EQ(r, apic_read(APIC_TPR), 0u);

        /* The software enable, and the spurious vector, exactly as
         * configure_local() wrote them. */
        u32 svr = apic_read(APIC_SPURIOUS);

        KT_ASSERT(r, (svr & APIC_SW_ENABLE) != 0);
        KT_EQ(r, svr & 0xFF, (u32)APIC_VECTOR_SPURIOUS);

        /* This is the boot processor, so LINT0 must be ExtINT and not masked.
         * Masking it is what cut the 8259s off from the CPU and stopped the
         * timer; the symptom was every log line sharing one timestamp, which
         * is not something a functional test would have noticed. */
        u32 lint0 = apic_read(APIC_LVT_LINT0);

        KT_EQ(r, lint0 & APIC_LVT_MASKED, 0u);
        KT_EQ(r, lint0 & 0x700, (u32)APIC_LVT_EXTINT);

        /* ...and the timer really is advancing, which is the property that
         * mattered. */
        u64 before = timer_ticks();
        u32 spins = 0;

        while (timer_ticks() == before && spins < 100000000u)
            spins++;

        KT_ASSERT(r, timer_ticks() > before);

        /* No errors latched. The error register needs a write before a read
         * to push the current state into it. */
        apic_write(APIC_ESR, 0);
        KT_EQ(r, apic_read(APIC_ESR), 0u);
    }

    /* --- the processor table --------------------------------------------- */
    KT_ASSERT(r, smp_cpu_count() >= 1);
    KT_ASSERT(r, smp_cpu_count() <= SMP_MAX_CPUS);
    KT_ASSERT(r, smp_cpus_present() >= smp_cpu_count());
    KT_ASSERT(r, smp_cpu(SMP_MAX_CPUS) == NULL);

    u32 me = smp_cpu_index();

    KT_ASSERT(r, me < SMP_MAX_CPUS);
    KT_ASSERT(r, smp_this_cpu() == (struct cpu *)smp_cpu(me));

    const struct cpu *self = smp_cpu(me);

    KT_ASSERT(r, self != NULL);

    if (self) {
        KT_ASSERT(r, self->present);
        KT_ASSERT(r, self->online);
        /* The test suite runs on the boot processor, because that is where
         * the scheduler runs. */
        KT_ASSERT(r, self->is_bsp);
        KT_EQ(r, self->index, me);

        if (apic_available())
            KT_EQ(r, (u32)self->apic_id, apic_id());
    }

    u32 online = 0, distinct = 1;

    for (u32 i = 0; i < SMP_MAX_CPUS; i++) {
        const struct cpu *c = smp_cpu(i);

        if (!c)
            continue;

        online++;
        KT_EQ(r, c->index, i);

        for (u32 j = i + 1; j < SMP_MAX_CPUS; j++) {
            const struct cpu *o = smp_cpu(j);

            if (o && o->apic_id == c->apic_id)
                distinct = 0;
        }

        if (c->is_bsp)
            continue;

        /* Every application processor's stack is in the per-CPU region, with
         * an unmapped guard page below it - the same arrangement as a task
         * stack, for the same reason. */
        vaddr_t base = c->stack_top - (16 * KIB);

        KT_ASSERT(r, base >= CPUSTACK_BASE);
        KT_ASSERT(r, c->stack_top <= CPUSTACK_BASE + CPUSTACK_REGION);
        KT_ASSERT(r, vmm_translate(base, NULL));
        KT_ASSERT(r, !vmm_translate(base - PAGE_SIZE, NULL));

        /* It reached its idle loop, which is the only thing an application
         * processor does in this phase. */
        KT_ASSERT(r, c->idle_loops > 0);

        /* And its *own* local APIC timer is ticking. This is the assertion
         * that distinguishes "the processor is awake" from "the processor can
         * be preempted": the 8254 has one output wired to one CPU, so on a
         * kernel without the APIC timer every entry here but the boot
         * processor's stays at zero forever. */
        if (apic_timer_running())
            KT_ASSERT(r, c->timer_ticks > 0);
    }

    KT_EQ(r, online, smp_cpu_count());
    KT_EQ(r, distinct, 1u);

    /* The timer's own accounting. The calibration is measured, so the figure
     * cannot be asserted exactly - what can be asserted is that it is inside
     * the range the calibration itself refuses to leave, and that the summed
     * tick count across processors is at least as large as this processor's
     * share of it. */
    if (apic_timer_running()) {
        struct apic_stats as;

        apic_get_stats(&as);
        KT_ASSERT(r, apic_timer_hz_measured() >= 1000000u);
        KT_ASSERT(r, as.timer_ticks >= smp_this_cpu()->timer_ticks);
        KT_ASSERT(r, smp_this_cpu()->timer_ticks > 0);
    }

    /* --- locks ----------------------------------------------------------- */
    static spinlock_t probe = SPINLOCK_INIT("ktest-probe");

    KT_ASSERT(r, !spin_is_locked(&probe));
    KT_ASSERT(r, spin_trylock(&probe));
    KT_ASSERT(r, spin_is_locked(&probe));

    /* A second acquisition from this processor must fail rather than spin.
     * spin_lock() would panic here, correctly - which is why this is the
     * trylock, and why the panic is not something a passing test can
     * check. */
    KT_ASSERT(r, !spin_trylock(&probe));

    u32 acquisitions = probe.acquisitions;

    spin_unlock(&probe);
    KT_ASSERT(r, !spin_is_locked(&probe));

    /* Uncontended, so it counts an acquisition and no contention. */
    KT_ASSERT(r, spin_trylock(&probe));
    KT_EQ(r, probe.acquisitions, acquisitions + 1);
    spin_unlock(&probe);

    /* A lock taken with interrupts enabled must give them back, and one
     * taken with them disabled must leave them disabled. Getting this
     * backwards re-enables interrupts inside a critical section somebody
     * else opened. */
    KT_ASSERT(r, irq_enabled());
    spin_lock(&probe);
    KT_ASSERT(r, !irq_enabled());
    spin_unlock(&probe);
    KT_ASSERT(r, irq_enabled());

    bool saved = irq_save();

    KT_ASSERT(r, !irq_enabled());
    spin_lock(&probe);
    spin_unlock(&probe);
    KT_ASSERT(r, !irq_enabled());
    irq_restore(saved);
    KT_ASSERT(r, irq_enabled());

    /* --- the lock hierarchy ---------------------------------------------- */

    /* A level-0 lock is exempt from the ordering check and must leave the
     * recorded level alone, so that the locks that predate the hierarchy -
     * the TLB shootdown's - keep working under one that has a level. */
    u8 outer = spin_level_held();

    spin_lock(&probe);
    KT_EQ(r, (u32)spin_level_held(), (u32)outer);
    spin_unlock(&probe);
    KT_EQ(r, (u32)spin_level_held(), (u32)outer);

    /* A levelled lock records its level while held and restores exactly what
     * was there before on release - not zero, which would let a release
     * inside a nest silently permit any order afterwards. */
    static spinlock_t low = SPINLOCK_INIT_AT("ktest-low", LOCK_LEVEL_FS);
    static spinlock_t high = SPINLOCK_INIT_AT("ktest-high", LOCK_LEVEL_PMM);

    spin_lock(&low);
    KT_EQ(r, (u32)spin_level_held(), (u32)LOCK_LEVEL_FS);

    /* Upwards is legal, and this is the whole point: the nest the kernel
     * actually performs, fs then pmm, must be allowed. */
    spin_lock(&high);
    KT_EQ(r, (u32)spin_level_held(), (u32)LOCK_LEVEL_PMM);
    spin_unlock(&high);
    KT_EQ(r, (u32)spin_level_held(), (u32)LOCK_LEVEL_FS);
    spin_unlock(&low);
    KT_EQ(r, (u32)spin_level_held(), (u32)outer);

    /* The level-0 lock must still be takeable from inside a levelled one.
     * The inversion itself cannot be tested from a passing test: taking
     * `low` while holding `high` panics, which is the intended behaviour and
     * is exercised by the fault scenarios, not here. */
    spin_lock(&high);
    KT_ASSERT(r, spin_trylock(&probe));
    KT_EQ(r, (u32)spin_level_held(), (u32)LOCK_LEVEL_PMM);
    spin_unlock(&probe);
    spin_unlock(&high);
    KT_EQ(r, (u32)spin_level_held(), (u32)outer);

    /* And `spin_held_by_self`, which the console depends on to print from
     * inside a panic it took while already holding the console. */
    KT_ASSERT(r, !spin_held_by_self(&high));
    spin_lock(&high);
    KT_ASSERT(r, spin_held_by_self(&high));
    KT_ASSERT(r, !spin_held_by_self(&low));
    spin_unlock(&high);
    KT_ASSERT(r, !spin_held_by_self(&high));

    /* --- inter-processor interrupts --------------------------------------- */
    struct smp_stats ss;

    smp_get_stats(&ss);
    KT_EQ(r, ss.start_failures, 0u);

    if (!smp_enabled()) {
        /* One processor. Both paths still have to be callable and both still
         * have to invalidate locally - a caller should not need to ask how
         * many processors there are to get correct behaviour on its own. */
        smp_ping_others();
        smp_tlb_shootdown(KERNEL_VIRT_BASE);

        struct smp_stats after;

        smp_get_stats(&after);
        KT_EQ(r, after.pings_sent, ss.pings_sent);
        KT_EQ(r, after.shootdowns_sent, ss.shootdowns_sent);
        return;
    }

    /* More than one. Every other processor must answer a ping. */
    u32 before_ping[SMP_MAX_CPUS];

    for (u32 i = 0; i < SMP_MAX_CPUS; i++) {
        const struct cpu *c = smp_cpu(i);

        before_ping[i] = c ? c->ipi_ping : 0;
    }

    smp_ping_others();

    /* Delivery is asynchronous, so wait - bounded, because an unanswered IPI
     * is a result and not a reason to hang. */
    for (u32 ms = 0; ms < 200; ms++) {
        u32 answered = 0;

        for (u32 i = 0; i < SMP_MAX_CPUS; i++) {
            const struct cpu *c = smp_cpu(i);

            if (c && !c->is_bsp && c->ipi_ping > before_ping[i])
                answered++;
        }

        if (answered >= smp_cpu_count() - 1)
            break;

        task_sleep_ms(1);
    }

    u32 answered = 0;

    for (u32 i = 0; i < SMP_MAX_CPUS; i++) {
        const struct cpu *c = smp_cpu(i);

        if (c && !c->is_bsp && c->ipi_ping > before_ping[i])
            answered++;
    }

    KT_EQ(r, answered, smp_cpu_count() - 1);

    /* And a TLB shootdown, which is the one that matters: it waits for every
     * other processor to confirm it has dropped the translation, so a count
     * that comes back short means a stale TLB entry survived somewhere. */
    smp_get_stats(&ss);
    smp_tlb_shootdown(KERNEL_VIRT_BASE + PAGE_SIZE);

    struct smp_stats after;

    smp_get_stats(&after);
    KT_EQ(r, after.shootdowns_sent, ss.shootdowns_sent + 1);
    KT_EQ(r, after.shootdowns_served - ss.shootdowns_served,
          smp_cpu_count() - 1);
}

/* ---- the bootstrap page directory, and an ordering bug it found --------
 *
 * This exercises vmm_create_bootstrap_pd() and vmm_destroy_bootstrap_pd(),
 * which exist for the application-processor trampoline. It is also a
 * regression test, and the regression is worth describing because no
 * functional test would have caught it.
 *
 * Both functions edit a page directory through a temporary mapping: map the
 * frame at a fixed kernel address, write it, unmap. The compiler sees a store
 * to the page table at one absolute address and accesses to the mapped frame
 * at another, concludes they cannot alias, and reorders them - so the store
 * that *tore down* the mapping was hoisted above the code still using it, and
 * the result was a page fault on a page the kernel had just mapped.
 *
 * It had been latent since the copy-on-write work, where the same pattern
 * happens to survive whatever GCC decides about the surrounding code. The fix
 * is that page table entries are volatile and the map/unmap pair carries
 * compiler barriers; this test is what notices if either is removed, because
 * the failure is immediate and fatal rather than subtle.
 */
static void test_bootstrap_pd(struct ktest_result *r)
{
    struct pmm_stats before, after;

    pmm_get_stats(&before);

    paddr_t pd = vmm_create_bootstrap_pd();

    KT_ASSERT(r, pd != 0);
    KT_ASSERT(r, pd != vmm_kernel_root_phys());

    if (!pd)
        return;

    /* Read it back through a temporary mapping, which is the access pattern
     * that was being reordered. vmm_inspect_entry() copies entries out under
     * the same temp-slot discipline, so a reordering bug shows up here as a
     * fault. */
    paddr_t gigabytes[PDPT_ENTRIES] = {0};
    bool all_present = true;

    for (u32 k = 0; k < PDPT_ENTRIES; k++) {
        pte_t entry = 0;

        KT_ASSERT(r, vmm_inspect_entry(pd, k, &entry));
        if (!(entry & PTE_PRESENT))
            all_present = false;
        gigabytes[k] = (paddr_t)(entry & PTE_ADDR_MASK);

        /* Bits 1, 2 and 5-8 of a PDPT entry are reserved, and a non-zero one
         * faults every walk through it. This is the assertion that would
         * catch writing a WRITE or USER bit here out of habit. */
        KT_EQ(r, (u32)(entry & 0x1E6u), 0u);
    }

    /* Every gigabyte is present, including the two empty ones: the
     * page-table window is used without checking, so that is an invariant
     * and not a detail. */
    KT_ASSERT(r, all_present);

    /* The kernel's gigabyte is the kernel's own page directory, by pointer
     * and not by copy. That single identity is what used to take 255 copied
     * directory entries and a test that compared all of them. */
    KT_EQ(r, gigabytes[KERNEL_PDPT_INDEX], vmm_kernel_pd_phys());

    /* Each user gigabyte has its own directory, and they are distinct. */
    for (u32 k = 0; k < USER_PDPT_COUNT; k++) {
        KT_ASSERT(r, gigabytes[k] != 0);
        KT_ASSERT(r, gigabytes[k] != vmm_kernel_pd_phys());
        for (u32 j = 0; j < k; j++)
            KT_ASSERT(r, gigabytes[k] != gigabytes[j]);
    }

    /* Gigabyte 0 maps the first 4 MiB and nothing else; 1 and 2 hold nothing
     * but their self-maps. This address space is for a processor, not a
     * process. Counted rather than asserted per slot - 1533 passing slots are
     * one property, not 1533 pieces of evidence. */
    u32 identity_slots = 0, unexpected = 0, self_maps = 0;

    for (u32 k = 0; k < USER_PDPT_COUNT; k++) {
        for (u32 pdi = 0; pdi < PD_ENTRIES; pdi++) {
            pte_t got = 0;

            if (!vmm_inspect_entry(gigabytes[k], pdi, &got)) {
                unexpected++;
                continue;
            }

            if (pdi == PD_SELF_SLOT) {
                /* Points at its own directory, not at the kernel's: a copied
                 * self-map would give this address space a window onto
                 * somebody else's page tables. */
                if ((got & PTE_PRESENT) &&
                    (paddr_t)(got & PTE_ADDR_MASK) == gigabytes[k])
                    self_maps++;
                else
                    unexpected++;
            } else if (k == 0 && pdi < VMM_BOOT_MAPPED / (2 * MIB)) {
                /* The identity map: the whole reason this address space
                 * exists, because a processor enabling paging while executing
                 * at a low physical address needs the next instruction to
                 * still be fetchable. */
                if ((got & PTE_PRESENT) && (got & PTE_WRITE) &&
                    (got & PTE_ADDR_MASK) != 0)
                    identity_slots++;
                else
                    unexpected++;
            } else if (got != 0) {
                unexpected++;
            }
        }
    }

    KT_EQ(r, self_maps, (u32)USER_PDPT_COUNT);
    KT_EQ(r, identity_slots, (u32)(VMM_BOOT_MAPPED / (2 * MIB)));
    KT_EQ(r, unexpected, 0u);

    /* The identity pages are executable, and have to be: the instruction
     * after `mov cr0` on an application processor is fetched through them. */
    pte_t identity_pde = 0;

    KT_ASSERT(r, vmm_inspect_entry(gigabytes[0], 0, &identity_pde));

    pte_t identity_pte = 0;

    KT_ASSERT(r, vmm_inspect_entry((paddr_t)(identity_pde & PTE_ADDR_MASK), 1,
                                   &identity_pte));
    KT_ASSERT(r, identity_pte & PTE_PRESENT);
    KT_EQ(r, (u32)(identity_pte & PTE_ADDR_MASK), (u32)PAGE_SIZE);
    KT_EQ(r, (identity_pte & PTE_NX) ? 1u : 0u, 0u);

    vmm_destroy_bootstrap_pd(pd);

    /* Six frames in, six frames out: a PDPT, three user page directories and
     * two identity page tables. The kernel's own page directory is shared and
     * must not have been touched, which is what the frame count proves - a
     * destroy that followed the kernel's PDPT entry would free page tables
     * the running kernel is using, and the count would come back *lower* than
     * it started. */
    pmm_get_stats(&after);
    KT_EQ(r, after.used_frames, before.used_frames);
}

/* ---- 64-bit long mode -------------------------------------------------
 *
 * Written so that it is a real test on both kinds of machine, which is the
 * only way it is worth having. QEMU's default i386 model has no long mode -
 * not even with -cpu max, which masks it for a 32-bit target - so on the
 * images CI boots by default this suite asserts that the kernel *declines*
 * correctly and leaves the machine alone. The `long-mode` scenario boots the
 * same image under qemu-system-x86_64, where the transition actually runs
 * and every assertion below has teeth.
 *
 * That is the same arrangement as the `hardened-cpu` scenario, for the same
 * reason: a feature test that silently skips is a feature test that stops
 * being read.
 */
static void test_longmode(struct ktest_result *r)
{
    const char *why = NULL;
    bool supported = longmode_supported();
    bool available = longmode_available(&why);

    /* Detection has to agree with itself: it is pure CPUID, so two calls in
     * a row cannot disagree, and "available" cannot be true where
     * "supported" is false. */
    KT_EQ(r, supported, longmode_supported());
    KT_ASSERT(r, available == false || supported == true);
    KT_ASSERT(r, available || why != NULL);

    /* Whatever the processor can do, the kernel's own state is 32-bit before
     * and after - which is the invariant the whole feature is judged on.
     * CR4.PAE is set and stays set: the kernel is PAE either way, and long
     * mode's four-level walk is PAE's entry format with one more level on
     * top. What must not survive the trip is EFER.LME. */
    KT_ASSERT(r, (read_cr4() & CR4_PAE_BIT) != 0);
    KT_ASSERT(r, (read_cr0() & 0x80000000u) != 0);

    paddr_t cr3_before = read_cr3();
    u32 pages_before = vmm_count_user_pages();

    struct longmode_result lm;
    bool ok = longmode_round_trip(&lm);

    /* The address space is exactly as it was. The transition switches CR3
     * twice and turns paging off and on twice; a caller must not be able to
     * tell that any of it happened. */
    KT_EQ(r, read_cr3(), cr3_before);
    KT_EQ(r, vmm_count_user_pages(), pages_before);
    KT_ASSERT(r, (read_cr4() & CR4_PAE_BIT) != 0);
    KT_ASSERT(r, (read_cr0() & 0x80000000u) != 0);
    KT_ASSERT(r, irq_enabled()); /* restored, not left masked */

    KT_EQ(r, lm.supported, supported);
    KT_EQ(r, lm.attempted, available);
    KT_EQ(r, ok, lm.round_trip);

    if (!available) {
        /* The declining path. Nothing attempted, nothing built, and the
         * reason is a sentence rather than a flag. */
        KT_ASSERT(r, !lm.round_trip);
        KT_EQ(r, lm.flags, 0u);
        KT_ASSERT(r, lm.failure != NULL);
        KT_EQ(r, lm.pml4_phys, 0u);
        return;
    }

    /* --- the real thing -------------------------------------------------- */
    KT_ASSERT(r, ok);
    KT_ASSERT(r, lm.failure == NULL);

    /* Every stage, individually, so a partial transition names its own
     * stopping point instead of failing one aggregate assertion. */
    KT_ASSERT(r, (lm.flags & LM_F_ENTERED) != 0);
    KT_ASSERT(r, (lm.flags & LM_F_COMPAT) != 0);
    KT_ASSERT(r, (lm.flags & LM_F_LONG) != 0);
    KT_ASSERT(r, (lm.flags & LM_F_VERIFIED) != 0);
    KT_ASSERT(r, (lm.flags & LM_F_BACK32) != 0);
    KT_ASSERT(r, (lm.flags & LM_F_RETURNED) != 0);
    KT_EQ(r, lm.flags, LM_F_ALL);

    /* What only 64-bit mode can do. */
    KT_ASSERT(r, lm.wide == LM_WIDE_VALUE);
    KT_ASSERT(r, lm.crossed == LM_CROSSED_VALUE);
    KT_ASSERT(r, lm.crossed > 0xFFFFFFFFull); /* the point of that one */
    KT_ASSERT(r, lm.r15 == LM_R15_VALUE);
    KT_ASSERT(r, lm.rip_lea == (u64)lm.rip_expected);
    KT_ASSERT(r, lm.probe_value == LM_PROBE_MAGIC);

    /* The processor's own statement that it was in long mode, rather than
     * the kernel's belief that it put it there. */
    KT_ASSERT(r, (lm.efer & EFER_LMA_BIT) != 0);
    KT_ASSERT(r, (lm.efer & EFER_LME_BIT) != 0);
    KT_ASSERT(r, (lm.cr4 & CR4_PAE_BIT) != 0);
    KT_EQ(r, lm.cs, LM_SEL_CODE64); /* the descriptor with L set */

    /* The probe frame has to be outside the 4 MiB identity map the
     * transition runs under, or the pointer read proved nothing. This is the
     * assertion the first run failed, when it landed inside. */
    KT_ASSERT(r, lm.probe_phys >= 4 * MIB);
    KT_ASSERT(r, lm.probe_phys < (paddr_t)lm.identity_mib * MIB);

    /* Three distinct, page-aligned frames, none of them the probe. */
    KT_ASSERT(r, lm.pml4_phys && lm.pdpt_phys && lm.pd_phys);
    KT_ASSERT(r, lm.pml4_phys != lm.pdpt_phys);
    KT_ASSERT(r, lm.pdpt_phys != lm.pd_phys);
    KT_ASSERT(r, lm.pml4_phys != lm.pd_phys);
    KT_ASSERT(r, lm.probe_phys != lm.pml4_phys);
    KT_ASSERT(r, IS_ALIGNED(lm.pml4_phys, PAGE_SIZE));
    KT_ASSERT(r, IS_ALIGNED(lm.pdpt_phys, PAGE_SIZE));
    KT_ASSERT(r, IS_ALIGNED(lm.pd_phys, PAGE_SIZE));

    KT_ASSERT(r, lm.cycles > 0);
    KT_ASSERT(r, lm.trampoline_bytes > 0 && lm.trampoline_bytes <= 2048);

    /* Twice. The tables are built once and reused, so a second run takes a
     * path the first does not - and a transition that only works from a cold
     * start is a transition with a bug in its cleanup. */
    struct longmode_result again;

    KT_ASSERT(r, longmode_round_trip(&again));
    KT_EQ(r, again.flags, LM_F_ALL);
    KT_EQ(r, again.pml4_phys, lm.pml4_phys); /* reused, not rebuilt */
    KT_EQ(r, again.probe_phys, lm.probe_phys);
    KT_ASSERT(r, again.probe_value == LM_PROBE_MAGIC);
    KT_EQ(r, read_cr3(), cr3_before);
    KT_ASSERT(r, (read_cr4() & CR4_PAE_BIT) != 0);
}

/* ---- networking --------------------------------------------------------
 *
 * Split the way the stack is: the parts that are pure functions of their
 * input are tested unconditionally, and the parts that need a controller are
 * tested only when there is one. A machine with no network card still
 * exercises every checksum, every parser and the whole ARP cache.
 *
 * The checksum vectors are real packets, lifted out of a pcap of this kernel
 * talking to QEMU's gateway. That matters: a checksum tested only against
 * its own output is a checksum tested against itself, and the two most
 * likely bugs - a byte-order slip and mishandling an odd length - both
 * survive that. These headers were generated by somebody else's stack.
 */
static void test_net(struct ktest_result *r)
{
    /* --- the checksum --------------------------------------------------- */

    /* A real IPv4 header from a UDP datagram QEMU's gateway sent us. A
     * correct header, including its checksum field, sums to zero - that is
     * the property every receiver relies on. */
    static const u8 real_udp_ip_hdr[] = {
        0x45, 0x00, 0x00, 0x35, 0x00, 0x00, 0x00, 0x00, 0x40, 0x11,
        0x62, 0xa8, 0x0a, 0x00, 0x02, 0x02, 0x0a, 0x00, 0x02, 0x0f,
    };

    /* And one from a TCP SYN, which has a different protocol number and a
     * different length - so a checksum that only worked for one would show. */
    static const u8 real_tcp_ip_hdr[] = {
        0x45, 0x00, 0x00, 0x2c, 0x00, 0x01, 0x00, 0x00, 0x40, 0x06,
        0x62, 0xbb, 0x0a, 0x00, 0x02, 0x02, 0x0a, 0x00, 0x02, 0x0f,
    };

    KT_EQ(r, checksum(real_udp_ip_hdr, sizeof(real_udp_ip_hdr)), 0u);
    KT_EQ(r, checksum(real_tcp_ip_hdr, sizeof(real_tcp_ip_hdr)), 0u);

    /* Recomputing with the field zeroed must reproduce the value that was in
     * it. This is the direction the stack uses when it *sends*, and it is a
     * different code path from verifying. */
    {
        u8 h[sizeof(real_udp_ip_hdr)];
        u16 original;

        memcpy(h, real_udp_ip_hdr, sizeof(h));
        memcpy(&original, h + 10, 2);
        h[10] = h[11] = 0;
        KT_EQ(r, checksum(h, sizeof(h)), original);
    }

    /* A single byte flipped anywhere must break it. Checked across the whole
     * header rather than at one offset, because a checksum that ignores the
     * last word passes a test that only flips the first. */
    for (size_t i = 0; i < sizeof(real_udp_ip_hdr); i++) {
        u8 h[sizeof(real_udp_ip_hdr)];

        memcpy(h, real_udp_ip_hdr, sizeof(h));
        h[i] ^= 0x01;
        KT_ASSERT(r, checksum(h, sizeof(h)) != 0);
    }

    /* The odd-length case, which is the other classic bug: a trailing byte
     * is the *high* half of a word whose low half is zero. Appending a zero
     * byte to an even-length buffer must therefore not change the sum. */
    {
        u8 odd[sizeof(real_udp_ip_hdr) + 1];

        memcpy(odd, real_udp_ip_hdr, sizeof(real_udp_ip_hdr));
        odd[sizeof(real_udp_ip_hdr)] = 0;
        KT_EQ(r, checksum(odd, sizeof(odd)),
              checksum(real_udp_ip_hdr, sizeof(real_udp_ip_hdr)));

        /* ... and a non-zero trailing byte must change it. */
        odd[sizeof(real_udp_ip_hdr)] = 0xA5;
        KT_ASSERT(r, checksum(odd, sizeof(odd)) !=
                         checksum(real_udp_ip_hdr, sizeof(real_udp_ip_hdr)));
    }

    /* The incremental form has to agree with the one-shot form, since the
     * transports use the first and IP uses the second. */
    {
        u32 part = checksum_partial(real_udp_ip_hdr, 8, 0);

        part = checksum_partial(real_udp_ip_hdr + 8,
                                sizeof(real_udp_ip_hdr) - 8, part);
        KT_EQ(r, checksum_finish(part),
              checksum(real_udp_ip_hdr, sizeof(real_udp_ip_hdr)));
    }

    /* --- byte order ----------------------------------------------------- */
    KT_EQ(r, ntohs(htons(0x1234)), 0x1234u);
    KT_EQ(r, ntohl(htonl(0x12345678u)), 0x12345678u);
    KT_EQ(r, htons(0x0100), 0x0001u); /* an actual swap, not the identity */
    KT_EQ(r, htonl(0x01000000u), 0x00000001u);

    /* --- addresses ------------------------------------------------------- */
    {
        ipv4_addr ip = 0;
        char buf[IPV4_STR_LEN];

        KT_ASSERT(r, ipv4_parse("10.0.2.15", &ip));
        KT_EQ(r, ip, IPV4(10, 0, 2, 15));
        KT_ASSERT(r, strcmp(ipv4_str(ip, buf, sizeof(buf)), "10.0.2.15") == 0);

        KT_ASSERT(r, ipv4_parse("255.255.255.255", &ip));
        KT_EQ(r, ip, 0xFFFFFFFFu);
        KT_ASSERT(r, ipv4_parse("0.0.0.0", &ip));
        KT_EQ(r, ip, 0u);

        /* Every rejection is a packet that would otherwise go somewhere
         * unintended, so each malformed form is checked rather than assumed. */
        KT_ASSERT(r, !ipv4_parse("10.0.2", &ip));
        KT_ASSERT(r, !ipv4_parse("10.0.2.15.7", &ip));
        KT_ASSERT(r, !ipv4_parse("10.0.2.256", &ip));
        KT_ASSERT(r, !ipv4_parse("10.0.2.1000", &ip));
        KT_ASSERT(r, !ipv4_parse("10..2.15", &ip));
        KT_ASSERT(r, !ipv4_parse("10.0.2.15x", &ip));
        KT_ASSERT(r, !ipv4_parse("", &ip));
        KT_ASSERT(r, !ipv4_parse("hello", &ip));
    }

    {
        struct mac_addr a = {{0x52, 0x54, 0x00, 0x12, 0x34, 0x56}};
        struct mac_addr b = a;
        char buf[MAC_STR_LEN];

        KT_ASSERT(r, mac_equal(&a, &b));
        b.b[5] = 0x57;
        KT_ASSERT(r, !mac_equal(&a, &b));
        KT_ASSERT(r, !mac_is_multicast(&a));
        KT_ASSERT(r, mac_is_broadcast(&mac_broadcast));
        KT_ASSERT(r, mac_is_multicast(&mac_broadcast)); /* broadcast is one */
        KT_ASSERT(r, !mac_is_broadcast(&a));
        KT_ASSERT(
            r, strcmp(mac_str(&a, buf, sizeof(buf)), "52:54:00:12:34:56") == 0);
    }

    /* --- the pseudo-header ---------------------------------------------- */

    /* The real TCP SYN above, checksummed with its pseudo-header, must come
     * out to zero - which is the only way to know the pseudo-header is built
     * from the right twelve bytes in the right order. The segment is the
     * 24 bytes after the IPv4 header. */
    {
        static const u8 real_tcp_seg[] = {
            0xb3, 0xd2, 0x00, 0x07, 0x00, 0x01, 0xf4, 0x01,
            0x00, 0x00, 0x00, 0x00, 0x60, 0x02, 0xff, 0xff,
            0xd8, 0x39, 0x00, 0x00, 0x02, 0x04, 0x05, 0xb4,
        };
        u32 sum = ipv4_pseudo_sum(IPV4(10, 0, 2, 2), IPV4(10, 0, 2, 15),
                                  IPPROTO_TCP, sizeof(real_tcp_seg));

        KT_EQ(r,
              checksum_finish(
                  checksum_partial(real_tcp_seg, sizeof(real_tcp_seg), sum)),
              0u);

        /* The wrong destination must fail it. That is the entire purpose of
         * the pseudo-header, so it is worth proving rather than trusting. */
        sum = ipv4_pseudo_sum(IPV4(10, 0, 2, 2), IPV4(10, 0, 2, 16),
                              IPPROTO_TCP, sizeof(real_tcp_seg));
        KT_ASSERT(r, checksum_finish(checksum_partial(
                         real_tcp_seg, sizeof(real_tcp_seg), sum)) != 0);

        /* So must the wrong protocol number. */
        sum = ipv4_pseudo_sum(IPV4(10, 0, 2, 2), IPV4(10, 0, 2, 15),
                              IPPROTO_UDP, sizeof(real_tcp_seg));
        KT_ASSERT(r, checksum_finish(checksum_partial(
                         real_tcp_seg, sizeof(real_tcp_seg), sum)) != 0);
    }

    /* --- routing, such as it is ----------------------------------------- */
    {
        const struct net_config *cfg = net_get_config();

        if (cfg->addr) {
            KT_ASSERT(r, net_is_local(cfg->addr));
            KT_ASSERT(r, net_is_local(cfg->gateway));
            KT_ASSERT(r, !net_is_local(IPV4(8, 8, 8, 8)));
            KT_EQ(r, net_next_hop(cfg->gateway), cfg->gateway);
            KT_EQ(r, net_next_hop(IPV4(8, 8, 8, 8)), cfg->gateway);
        }
    }

    /* --- the ARP cache --------------------------------------------------- */
    {
        struct mac_addr out;

        arp_cache_clear();
        KT_EQ(r, arp_cache_count(), 0u);
        KT_ASSERT(r, arp_cache_at(0) == NULL);

        /* A miss sends a request and reports failure rather than queueing -
         * see the comment on arp_resolve(). Without a device it cannot even
         * send, and must still not claim success. */
        KT_ASSERT(r, !arp_resolve(IPV4(192, 168, 99, 1), &out));
    }

    /* --- TCP's state names ----------------------------------------------- */
    KT_ASSERT(r, strcmp(tcp_state_name(TCP_LISTEN), "LISTEN") == 0);
    KT_ASSERT(r, strcmp(tcp_state_name(TCP_ESTABLISHED), "ESTABLISHED") == 0);
    KT_ASSERT(r, strcmp(tcp_state_name(TCP_TIME_WAIT), "TIME_WAIT") == 0);
    /* SYN_SENT exists now, because the stack can be the side that starts a
     * connection. It had no name before because it had no use. */
    KT_ASSERT(r, strcmp(tcp_state_name(TCP_SYN_SENT), "SYN_SENT") == 0);
    KT_ASSERT(r, strcmp(tcp_state_name(TCP_CLOSE_WAIT), "CLOSE_WAIT") == 0);
    KT_ASSERT(r, strcmp(tcp_state_name(TCP_FIN_WAIT_2), "FIN_WAIT_2") == 0);

    /* --- the congestion state, at rest ----------------------------------
     *
     * Asserted as invariants rather than as literals. "cwnd starts at two
     * segments" is a tuning choice that may well change; "slow start has
     * somewhere to grow into, and the retransmission timeout is inside its
     * bounds" is what has to hold whatever the numbers are, and a test that
     * breaks when a constant is retuned is a test people learn to edit
     * without reading. */
    {
        struct tcp_status ts;

        tcp_get_status(&ts);

        /* A congestion window below one segment could never send anything. */
        KT_ASSERT(r, ts.cwnd >= 1460);
        /* Above ssthresh and the connection would open in congestion
         * avoidance, never having done slow start at all. */
        KT_ASSERT(r, ts.ssthresh > ts.cwnd);
        KT_ASSERT(r, ts.rto_ms >= 200 && ts.rto_ms <= 8000);
        KT_ASSERT(r, !ts.in_recovery);
        KT_ASSERT(r, ts.fast_retransmits == 0);
        /* Nothing is connected, so this is the listener's own state. */
        KT_ASSERT(r, ts.state == TCP_LISTEN || ts.state == TCP_CLOSED);
    }

    /* --- handles -------------------------------------------------------
     *
     * A handle is an index plus a generation counter, and 0 is deliberately
     * not a valid one, so that a zeroed field fails closed. Every call that
     * takes a handle has to reject a bad one the same way; a single missing
     * check is a read of someone else's connection. */
    KT_ASSERT(r, tcp_handle_state(0) == TCP_CLOSED);
    KT_ASSERT(r, tcp_handle_state(-1) == TCP_CLOSED);
    KT_ASSERT(r, tcp_handle_state(0x7FFFFFFF) == TCP_CLOSED);
    {
        char junk[4] = {0};

        KT_EQ(r, tcp_send(0, junk, sizeof(junk)), SYS_EBADF);
        KT_EQ(r, tcp_recv(0, junk, sizeof(junk)), SYS_EBADF);
    }
    KT_EQ(r, tcp_recv_available(0), 0u);
    /* A handle that names nothing has nothing left to read, which is the
     * same answer as a connection the peer has closed - and is the answer a
     * caller can act on without a special case. */
    KT_ASSERT(r, tcp_recv_eof(0));
    KT_EQ(r, tcp_send_room(0), 0u);
    /* None of these may fault on a handle that names nothing. */
    tcp_shutdown(0);
    tcp_close(0);
    tcp_abort(0);
    KT_ASSERT(r, tcp_handle_state(0) == TCP_CLOSED);

    /* --- passive opens for the socket layer ------------------------------ */
    {
        /* The echo port is in use, and a second listener on it would make
         * the destination port ambiguous. */
        KT_ASSERT(r, !tcp_bind_listener(tcp_listen_port()));
        KT_ASSERT(r, !tcp_bind_listener(0));
        KT_ASSERT(r, tcp_bind_listener(8080));
        KT_ASSERT(r, !tcp_bind_listener(8080)); /* already bound */
        /* Nothing has connected, so there is nothing to accept - and that is
         * EAGAIN, not an error, because the caller is expected to wait. */
        KT_EQ(r, tcp_accept(8080), SYS_EAGAIN);
        tcp_unbind_listener(8080);
        /* Unbinding has to actually free the port, or a program that opened
         * and closed a listening socket could never open it again. */
        KT_ASSERT(r, tcp_bind_listener(8080));
        tcp_unbind_listener(8080);
        /* A port nobody bound has nothing to accept either. */
        KT_EQ(r, tcp_accept(8081), SYS_EAGAIN);
    }

    /* --- ephemeral ports ------------------------------------------------- */
    {
        u16 a = tcp_ephemeral_port();
        u16 b = tcp_ephemeral_port();

        KT_ASSERT(r, a >= 49152);
        KT_ASSERT(r, b >= 49152);
        /* Two in a row must differ, or every outgoing connection from this
         * kernel would share a four-tuple with the last one. */
        KT_ASSERT(r, a != b);
        /* And neither may collide with a port already listening. */
        KT_ASSERT(r, a != tcp_listen_port() && b != tcp_listen_port());
    }

    /* --- connect's argument checks --------------------------------------
     *
     * Port 0 is not a port. Checked here rather than left to the handshake,
     * because a SYN to port 0 is a segment nothing will ever answer and the
     * connection would sit in SYN_SENT for the full timeout. */
    KT_EQ(r, tcp_connect(IPV4(10, 0, 2, 2), 0, 0), SYS_EINVAL);

    /* A status read on a handle that names nothing must say so rather than
     * filling the caller's struct with a previous connection's numbers. */
    {
        struct tcp_status ts;

        KT_ASSERT(r, !tcp_get_status_at(0, &ts));
        KT_ASSERT(r, !tcp_get_status_at(0x7FFFFFFF, &ts));
    }

    /* --- the hardware, if there is any ---------------------------------- */

    /* Which path this suite took is worth stating in the transcript. Without
     * it, a run on a machine with no controller and a run that reached the
     * wire are indistinguishable except by counting assertions, and the
     * whole point of the `network` CI scenario is that the second one
     * happened. */
    pr_info("ktest net: controller %s, link %s",
            e1000_present() ? "present" : "absent", net_up() ? "up" : "down");

    if (!e1000_present()) {
        /* No controller. Every assertion above still ran; the stack simply
         * has nothing to send through. Asserting the absence is reported
         * consistently is the only thing left to check. */
        KT_ASSERT(r, net_device() == NULL);
        KT_ASSERT(r, !net_up());
        return;
    }

    struct e1000_info ei;

    e1000_get_info(&ei);

    KT_ASSERT(r, ei.present);
    KT_ASSERT(r, net_device() != NULL);

    /* The MAC must be real, and it must be the one the stack puts in frames.
     * Those are two different copies, and their being different was a bug:
     * every frame went out with a source MAC of zero while the log printed
     * the right one. */
    KT_ASSERT(r, !mac_equal(&ei.mac, &mac_zero));
    KT_ASSERT(r, !mac_is_multicast(&ei.mac));
    KT_ASSERT(r, mac_equal(&ei.mac, &net_device()->mac));

    /* The rings exist, are page-aligned where the hardware requires it, and
     * the device's head is inside them. */
    KT_EQ(r, ei.rx_ring_entries, 32u);
    KT_EQ(r, ei.tx_ring_entries, 32u);
    KT_ASSERT(r, ei.rx_ring_phys && ei.tx_ring_phys);
    KT_ASSERT(r, ei.rx_ring_phys != ei.tx_ring_phys);
    KT_ASSERT(r, IS_ALIGNED(ei.rx_ring_phys, 16));
    KT_ASSERT(r, IS_ALIGNED(ei.tx_ring_phys, 16));
    KT_ASSERT(r, ei.rx_head < ei.rx_ring_entries);
    KT_ASSERT(r, ei.tx_head < ei.tx_ring_entries);
    KT_ASSERT(r, ei.mmio_phys != 0);

    if (!ei.link_up)
        return; /* a card with no cable: nothing below can be tested */

    KT_ASSERT(r, net_up());

    /* --- the wire ------------------------------------------------------- */
    const struct net_config *cfg = net_get_config();
    struct net_stats *st = net_stats();
    u32 tx_before = st->tx_frames;

    /* Resolve the gateway. The first attempt always fails - a miss sends the
     * request and drops the caller's packet - so this is a poll, which is
     * also exactly how the shell's `arp` command behaves. */
    struct mac_addr gw;
    bool resolved = false;

    arp_cache_clear();

    for (u32 i = 0; i < 200 && !resolved; i++) {
        resolved = arp_resolve(cfg->gateway, &gw);
        if (resolved)
            break;
        net_poll(8);
        task_sleep_ms(5);
    }

    KT_ASSERT(r, resolved);
    KT_ASSERT(r, st->tx_frames > tx_before); /* something went out  */
    KT_ASSERT(r, st->arp_requests_sent > 0);

    if (!resolved)
        return;

    KT_ASSERT(r, !mac_equal(&gw, &mac_zero));
    KT_EQ(r, arp_cache_count(), 1u);

    const struct arp_entry *e = arp_cache_at(0);

    KT_ASSERT(r, e != NULL);
    if (e) {
        KT_EQ(r, e->ip, cfg->gateway);
        KT_ASSERT(r, mac_equal(&e->mac, &gw));
    }

    /* Ping the gateway. This is the end-to-end assertion: a datagram this
     * kernel built, checksummed and transmitted, which something else
     * parsed, accepted and answered. Nothing in the stack can be quietly
     * wrong and still produce a reply. */
    u32 replies_before = st->icmp_echo_replies_received;
    bool got_reply = false;

    icmp_clear_last_reply();
    KT_ASSERT(r, icmp_echo_request(cfg->gateway, 0xBEEF, 1, 32));

    for (u32 i = 0; i < 200 && !got_reply; i++) {
        const struct icmp_reply *rep = icmp_last_reply();

        if (rep->valid && rep->id == 0xBEEF && rep->seq == 1) {
            got_reply = true;
            KT_EQ(r, rep->from, cfg->gateway);
            break;
        }
        net_poll(8);
        task_sleep_ms(5);
    }

    KT_ASSERT(r, got_reply);
    KT_ASSERT(r, st->icmp_echo_replies_received > replies_before);
    KT_ASSERT(r, st->ip_rx > 0);
    KT_ASSERT(r, st->ip_tx > 0);

    /* Nothing was mangled on the way in: a bad checksum anywhere would have
     * been counted rather than silently accepted. */
    KT_EQ(r, st->ip_bad_checksum, 0u);
    KT_EQ(r, st->icmp_bad_checksum, 0u);
    KT_EQ(r, st->rx_dropped_short, 0u);
}

/* ------------------------------------------------------------------------- */
/* The VFS and the descriptor table                                         */
/* ------------------------------------------------------------------------- */

/* Path resolution gets its own block, first, and gets the most assertions.
 *
 * It is the only part of this subsystem that is a security boundary: every
 * path a ring-3 program passes goes through vfs_resolve() and nothing else
 * re-checks the result. A ".." that escaped here would be a filesystem
 * escape, and it would be one in every backend at once.
 *
 * Note what is asserted about escape attempts: not that they are *rejected*,
 * but that they resolve to a path inside the root. There is no syntax for
 * "above the root" in the output, so there is nothing for a backend to have
 * to refuse - which is a stronger property than a rejection, because it
 * cannot be forgotten.
 */
static void test_vfs_paths(struct ktest_result *r)
{
    char out[VFS_PATH_MAX];

    /* Absolute paths, canonicalised and folded to upper case. */
    KT_ASSERT(r, vfs_resolve("/", "/bin/init", out, sizeof(out)));
    KT_ASSERT(r, strcmp(out, "/BIN/INIT") == 0);

    KT_ASSERT(r, vfs_resolve("/", "/BIN///INIT", out, sizeof(out)));
    KT_ASSERT(r, strcmp(out, "/BIN/INIT") == 0);

    KT_ASSERT(r, vfs_resolve("/", "/bin/./init", out, sizeof(out)));
    KT_ASSERT(r, strcmp(out, "/BIN/INIT") == 0);

    KT_ASSERT(r, vfs_resolve("/", "/", out, sizeof(out)));
    KT_ASSERT(r, strcmp(out, "/") == 0);

    /* A trailing slash names the same thing as no trailing slash. */
    KT_ASSERT(r, vfs_resolve("/", "/bin/", out, sizeof(out)));
    KT_ASSERT(r, strcmp(out, "/BIN") == 0);

    /* Relative, against a working directory. */
    KT_ASSERT(r, vfs_resolve("/BIN", "init", out, sizeof(out)));
    KT_ASSERT(r, strcmp(out, "/BIN/INIT") == 0);

    KT_ASSERT(r, vfs_resolve("/BIN", ".", out, sizeof(out)));
    KT_ASSERT(r, strcmp(out, "/BIN") == 0);

    KT_ASSERT(r, vfs_resolve("/BIN", "..", out, sizeof(out)));
    KT_ASSERT(r, strcmp(out, "/") == 0);

    KT_ASSERT(r, vfs_resolve("/BIN", "../etc/motd.txt", out, sizeof(out)));
    KT_ASSERT(r, strcmp(out, "/ETC/MOTD.TXT") == 0);

    /* --- escapes. Every one of these must land at or under the root. --- */
    KT_ASSERT(r, vfs_resolve("/", "..", out, sizeof(out)));
    KT_ASSERT(r, strcmp(out, "/") == 0);

    KT_ASSERT(r, vfs_resolve("/", "../../../..", out, sizeof(out)));
    KT_ASSERT(r, strcmp(out, "/") == 0);

    KT_ASSERT(r, vfs_resolve("/", "/../../etc/passwd", out, sizeof(out)));
    KT_ASSERT(r, strcmp(out, "/ETC/PASSWD") == 0);

    KT_ASSERT(
        r, vfs_resolve("/BIN", "../../../../../../BIN/INIT", out, sizeof(out)));
    KT_ASSERT(r, strcmp(out, "/BIN/INIT") == 0);

    /* "..." is a name, not two dots and a dot. A resolver that stripped dots
     * by counting them rather than by comparing whole components would get
     * this wrong, and would then treat "....//" as two levels up. */
    KT_ASSERT(r, vfs_resolve("/", "/...", out, sizeof(out)));
    KT_ASSERT(r, strcmp(out, "/...") == 0);

    /* Every output begins with '/' - asserted directly, because it is the
     * invariant every backend is entitled to assume. */
    static const char *const nasty[] = {
        "..", "../..", "./..", "/..", "a/../..", "../a/../..", "", ".",
    };

    for (size_t i = 0; i < ARRAY_SIZE(nasty); i++) {
        KT_ASSERT(r, vfs_resolve("/", nasty[i], out, sizeof(out)));
        KT_ASSERT(r, out[0] == '/');
        /* And no "/.." survives anywhere in the result. */
        KT_ASSERT(r,
                  strlen(out) < 3 || strcmp(out + strlen(out) - 3, "/..") != 0);
    }

    /* Bounds. An overlong path fails rather than being truncated: a
     * truncated path names a different file, and silently opening the wrong
     * file is worse than failing to open one. */
    char longpath[VFS_PATH_MAX * 2];

    longpath[0] = '/';
    for (size_t i = 1; i < sizeof(longpath) - 1; i++)
        longpath[i] = (i % 9 == 0) ? '/' : 'A';
    longpath[sizeof(longpath) - 1] = '\0';

    KT_ASSERT(r, !vfs_resolve("/", longpath, out, sizeof(out)));

    /* A single component longer than any name a backend here can hold. */
    KT_ASSERT(
        r, !vfs_resolve("/", "/ABCDEFGHIJKLMNOPQRSTUVWXYZ", out, sizeof(out)));

    /* A buffer too small to hold even "/" is refused rather than written to. */
    char tiny[1];

    KT_ASSERT(r, !vfs_resolve("/", "/bin", tiny, sizeof(tiny)));
}

/* ------------------------------------------------------------------------- */
/* Writing to the filesystem                                                */
/* ------------------------------------------------------------------------- */

/* Everything this suite creates, it removes. That is not tidiness - it is
 * what makes the suite runnable twice.
 *
 * A write test mutates the disk image, and the image is a build artefact that
 * is not regenerated between boots. Six CI scenarios boot the same
 * stratum-test.img and run these assertions, one after another. A suite that
 * created /TEST.TXT and left it there would pass in the first scenario and
 * fail in the second with EEXIST - and would then pass again after a rebuild,
 * which is the worst possible failure mode: a test that depends on how
 * recently `make` ran.
 *
 * So the first thing it does is delete anything a previous run left behind,
 * ignoring the result, and the last thing it does is delete what it made.
 */
#define TEST_FILE  "/KTEST.TXT"
#define TEST_FILE2 "/KTEST2.TXT"
#define TEST_DIR   "/KTESTDIR"

static void vfs_write_cleanup(void)
{
    /* Order matters: the directory has to be emptied before it can go. */
    (void)vfs_unlink(TEST_DIR "/INNER.TXT");
    (void)vfs_rmdir(TEST_DIR);
    (void)vfs_unlink(TEST_FILE);
    (void)vfs_unlink(TEST_FILE2);
}

static void test_vfs_write(struct ktest_result *r)
{
    if (!fat16_mounted()) {
        /* On the GRUB ISO path there is nothing to write to, and every write
         * call has to say so rather than fail in a way that suggests the
         * attempt nearly worked. */
        log_expect_errors(true);
        KT_EQ(r, fd_open(TEST_FILE, O_WRONLY | O_CREAT), SYS_ENOENT);
        KT_EQ(r, vfs_mkdir(TEST_DIR), SYS_ENOENT);
        KT_EQ(r, vfs_unlink(TEST_FILE), SYS_ENOENT);
        log_expect_errors(false);
        return;
    }

    KT_ASSERT(r, fat16_writable());

    u32 open_before = vfs_open_count();

    log_expect_errors(true);
    vfs_write_cleanup();
    log_expect_errors(false);

    /* --- free space is a real number, read off the disk ---------------- */
    u32 free_before = 0, total = 0;

    KT_ASSERT(r, fat16_space(&free_before, &total));
    KT_ASSERT(r, total > 0);
    KT_ASSERT(r, free_before > 0);
    KT_ASSERT(r, free_before <= total);

    /* --- create, write, close, reopen, read back ----------------------- */
    static const char payload[] =
        "StratumOS wrote this through the VFS, onto a FAT16 partition, "
        "with a cluster allocated from the allocation table and both "
        "copies of that table updated.\n";
    const u32 payload_len = (u32)(sizeof(payload) - 1);

    i32 fd = fd_open(TEST_FILE, O_WRONLY | O_CREAT | O_EXCL);

    KT_ASSERT(r, fd >= 0);

    if (fd < 0) {
        log_expect_errors(true);
        vfs_write_cleanup();
        log_expect_errors(false);
        return;
    }

    KT_EQ(r, fd_write(fd, payload, payload_len), (i32)payload_len);

    /* A write-only descriptor refuses a read, by access mode. */
    log_expect_errors(true);
    KT_EQ(r, fd_read(fd, (char[8]){0}, 8), SYS_EINVAL);
    log_expect_errors(false);

    struct sys_stat st;

    KT_EQ(r, fd_stat(fd, &st), 0);
    KT_EQ(r, st.size, payload_len);
    KT_ASSERT(r, st.blocks >= 1);
    KT_EQ(r, fd_close(fd), 0);

    /* Reopened from scratch, so what is read back comes off the disk via a
     * fresh directory lookup rather than out of the descriptor that wrote
     * it. That is the difference between testing the write and testing the
     * buffer it was written from. */
    char back[256];

    KT_ASSERT(r, payload_len <= sizeof(back));

    fd = fd_open(TEST_FILE, O_RDONLY);
    KT_ASSERT(r, fd >= 0);

    if (fd >= 0) {
        KT_EQ(r, fd_stat(fd, &st), 0);
        KT_EQ(r, st.size, payload_len);
        KT_EQ(r, fd_read(fd, back, payload_len), (i32)payload_len);
        KT_EQ(r, memcmp(back, payload, payload_len), 0);
        /* And nothing beyond it. */
        KT_EQ(r, fd_read(fd, back, sizeof(back)), 0);
        KT_EQ(r, fd_close(fd), 0);
    }

    /* It is visible by path and in its directory, not just by descriptor. */
    KT_EQ(r, vfs_stat(TEST_FILE, &st), 0);
    KT_EQ(r, st.size, payload_len);
    KT_ASSERT(r, (st.mode & S_MODE_WRITE) != 0);

    i32 rootfd = fd_open("/", O_RDONLY);
    bool listed = false;

    if (rootfd >= 0) {
        struct sys_dirent de;

        for (u32 i = 0; i < 256; i++) {
            if (fd_getdent(rootfd, i, &de) != 1)
                break;
            if (strcasecmp(de.name, "KTEST.TXT") == 0) {
                listed = true;
                KT_EQ(r, de.size, payload_len);
            }
        }
        KT_EQ(r, fd_close(rootfd), 0);
    }
    KT_ASSERT(r, listed);

    /* --- O_EXCL, O_TRUNC, O_APPEND ------------------------------------- */
    log_expect_errors(true);
    KT_EQ(r, fd_open(TEST_FILE, O_WRONLY | O_CREAT | O_EXCL), SYS_EEXIST);
    /* O_TRUNC without write access is a contradiction, not a request. */
    KT_EQ(r, fd_open(TEST_FILE, O_RDONLY | O_TRUNC), SYS_EINVAL);
    log_expect_errors(false);

    fd = fd_open(TEST_FILE, O_WRONLY | O_APPEND);
    KT_ASSERT(r, fd >= 0);

    if (fd >= 0) {
        /* O_APPEND positioned at the end, so this lands after the payload
         * rather than over it. */
        KT_EQ(r, fd_seek(fd, 0, SEEK_CUR), (i32)payload_len);
        KT_EQ(r, fd_write(fd, "appended", 8), 8);
        KT_EQ(r, fd_close(fd), 0);
    }

    KT_EQ(r, vfs_stat(TEST_FILE, &st), 0);
    KT_EQ(r, st.size, payload_len + 8);

    fd = fd_open(TEST_FILE, O_RDONLY);
    if (fd >= 0) {
        KT_EQ(r, fd_seek(fd, (i32)payload_len, SEEK_SET), (i32)payload_len);
        KT_EQ(r, fd_read(fd, back, 8), 8);
        KT_EQ(r, memcmp(back, "appended", 8), 0);
        KT_EQ(r, fd_close(fd), 0);
    }

    /* O_TRUNC empties it, and an empty file holds no clusters at all - which
     * is why first_cluster goes to 0 rather than keeping one allocated. */
    fd = fd_open(TEST_FILE, O_WRONLY | O_TRUNC);
    KT_ASSERT(r, fd >= 0);
    if (fd >= 0) {
        KT_EQ(r, fd_stat(fd, &st), 0);
        KT_EQ(r, st.size, 0u);
        KT_EQ(r, fd_close(fd), 0);
    }
    KT_EQ(r, vfs_stat(TEST_FILE, &st), 0);
    KT_EQ(r, st.size, 0u);

    /* --- a write that crosses a cluster boundary ----------------------- */
    const struct fat_info *fi = fat16_get_info();
    u32 big_len = fi->cluster_bytes + 777;

    KT_ASSERT(r, fi->cluster_bytes > 0);

    u8 *big = kmalloc(big_len);

    KT_ASSERT(r, big != NULL);

    if (big) {
        /* A pattern that is position-dependent, so that a chain followed in
         * the wrong order, or a sector written to the wrong place, shows up
         * as a mismatch rather than as more of the same byte. */
        for (u32 i = 0; i < big_len; i++)
            big[i] = (u8)(i * 7 + (i >> 8));

        fd = fd_open(TEST_FILE, O_RDWR | O_TRUNC);
        KT_ASSERT(r, fd >= 0);

        if (fd >= 0) {
            u32 done = 0;

            /* Written in pieces that do not divide the sector size, so the
             * read-modify-write path for a partial sector is exercised
             * rather than assumed. */
            while (done < big_len) {
                u32 n = big_len - done;

                if (n > 333)
                    n = 333;

                i32 w = fd_write(fd, big + done, n);

                KT_ASSERT(r, w > 0);
                if (w <= 0)
                    break;
                done += (u32)w;
            }

            KT_EQ(r, done, big_len);
            KT_EQ(r, fd_close(fd), 0);
        }

        /* More than one cluster was allocated, which is what makes this a
         * test of the chain rather than of a single cluster. */
        KT_EQ(r, vfs_stat(TEST_FILE, &st), 0);
        KT_EQ(r, st.size, big_len);
        KT_ASSERT(r, st.blocks >= 2);

        u8 *check = kmalloc(big_len);

        KT_ASSERT(r, check != NULL);

        if (check) {
            memset(check, 0, big_len);

            fd = fd_open(TEST_FILE, O_RDONLY);
            KT_ASSERT(r, fd >= 0);

            if (fd >= 0) {
                u32 got = 0;

                while (got < big_len) {
                    i32 n = fd_read(fd, check + got, big_len - got);

                    if (n <= 0)
                        break;
                    got += (u32)n;
                }

                KT_EQ(r, got, big_len);
                KT_EQ(r, memcmp(check, big, big_len), 0);
                KT_EQ(r, fd_close(fd), 0);
            }

            kfree(check);
        }

        /* --- truncate, both directions --------------------------------- */
        fd = fd_open(TEST_FILE, O_RDWR);
        KT_ASSERT(r, fd >= 0);

        if (fd >= 0) {
            /* Shrink to inside the first cluster: the tail of the chain has
             * to be freed, and the surviving part must still read correctly
             * up to the new end. */
            KT_EQ(r, vfs_truncate_file(fd_get(fd), 100), 0);
            KT_EQ(r, fd_stat(fd, &st), 0);
            KT_EQ(r, st.size, 100u);

            KT_EQ(r, fd_seek(fd, 0, SEEK_SET), 0);
            KT_EQ(r, fd_read(fd, back, 100), 100);
            KT_EQ(r, memcmp(back, big, 100), 0);

            /* Grow by truncate: the gap has to read as zeros, not as
             * whatever those clusters held before. */
            KT_EQ(r, vfs_truncate_file(fd_get(fd), 300), 0);
            KT_EQ(r, fd_stat(fd, &st), 0);
            KT_EQ(r, st.size, 300u);

            KT_EQ(r, fd_seek(fd, 100, SEEK_SET), 100);
            KT_EQ(r, fd_read(fd, back, 200), 200);

            bool zeros = true;
            for (u32 i = 0; i < 200; i++)
                if (back[i] != 0)
                    zeros = false;
            KT_ASSERT(r, zeros);

            KT_EQ(r, fd_close(fd), 0);
        }

        kfree(big);
    }

    /* --- a write past the end leaves a readable hole ------------------- */
    fd = fd_open(TEST_FILE2, O_RDWR | O_CREAT | O_EXCL);
    KT_ASSERT(r, fd >= 0);

    if (fd >= 0) {
        KT_EQ(r, fd_seek(fd, 2000, SEEK_SET), 2000);
        KT_EQ(r, fd_write(fd, "end", 3), 3);
        KT_EQ(r, fd_stat(fd, &st), 0);
        KT_EQ(r, st.size, 2003u);

        KT_EQ(r, fd_seek(fd, 0, SEEK_SET), 0);
        KT_EQ(r, fd_read(fd, back, 256), 256);

        bool hole_is_zero = true;
        for (u32 i = 0; i < 256; i++)
            if (back[i] != 0)
                hole_is_zero = false;
        KT_ASSERT(r, hole_is_zero);

        KT_EQ(r, fd_close(fd), 0);
    }

    /* --- directories --------------------------------------------------- */
    KT_EQ(r, vfs_mkdir(TEST_DIR), 0);

    log_expect_errors(true);
    KT_EQ(r, vfs_mkdir(TEST_DIR), SYS_EEXIST);
    /* A directory is not a file, in both directions. */
    KT_EQ(r, vfs_unlink(TEST_DIR), SYS_EISDIR);
    KT_EQ(r, vfs_rmdir(TEST_FILE), SYS_ENOTDIR);
    log_expect_errors(false);

    KT_EQ(r, vfs_stat(TEST_DIR, &st), 0);
    KT_EQ(r, st.type, (unsigned)S_TYPE_DIR);

    /* A new directory contains exactly "." and "..", which have to be there
     * for it to be a directory at all - and ".." of a directory in the root
     * is cluster 0, which is how FAT spells "the root". */
    i32 dfd2 = fd_open(TEST_DIR, O_RDONLY | O_DIRECTORY);

    KT_ASSERT(r, dfd2 >= 0);

    if (dfd2 >= 0) {
        struct sys_dirent de;
        bool dot = false, dotdot = false;
        u32 others = 0;

        for (u32 i = 0; i < 64; i++) {
            if (fd_getdent(dfd2, i, &de) != 1)
                break;
            if (strcmp(de.name, ".") == 0)
                dot = true;
            else if (strcmp(de.name, "..") == 0)
                dotdot = true;
            else
                others++;
        }

        KT_ASSERT(r, dot);
        KT_ASSERT(r, dotdot);
        KT_EQ(r, others, 0u);
        KT_EQ(r, fd_close(dfd2), 0);
    }

    /* A file inside the new directory, so that the path walk goes through an
     * entry this kernel wrote rather than one mkfat.py wrote. */
    fd = fd_open(TEST_DIR "/INNER.TXT", O_WRONLY | O_CREAT);
    KT_ASSERT(r, fd >= 0);

    if (fd >= 0) {
        KT_EQ(r, fd_write(fd, "nested", 6), 6);
        KT_EQ(r, fd_close(fd), 0);
    }

    KT_EQ(r, vfs_stat(TEST_DIR "/INNER.TXT", &st), 0);
    KT_EQ(r, st.size, 6u);

    /* chdir into it, and a relative open resolves there. */
    KT_EQ(r, fd_chdir(TEST_DIR), 0);
    fd = fd_open("INNER.TXT", O_RDONLY);
    KT_ASSERT(r, fd >= 0);
    if (fd >= 0) {
        KT_EQ(r, fd_read(fd, back, 6), 6);
        KT_EQ(r, memcmp(back, "nested", 6), 0);
        KT_EQ(r, fd_close(fd), 0);
    }
    KT_EQ(r, fd_chdir("/"), 0);

    /* A directory with something in it cannot be removed. */
    log_expect_errors(true);
    KT_EQ(r, vfs_rmdir(TEST_DIR), SYS_ENOTEMPTY);
    log_expect_errors(false);

    /* --- rename -------------------------------------------------------- */
    KT_EQ(r, vfs_rename(TEST_FILE2, TEST_DIR "/MOVED.TXT"), 0);
    log_expect_errors(true);
    KT_EQ(r, vfs_stat(TEST_FILE2, &st), SYS_ENOENT);
    log_expect_errors(false);
    KT_EQ(r, vfs_stat(TEST_DIR "/MOVED.TXT", &st), 0);
    KT_EQ(r, st.size, 2003u);

    /* Back again, and a rename onto an existing name is refused rather than
     * silently replacing it. */
    KT_EQ(r, vfs_rename(TEST_DIR "/MOVED.TXT", TEST_FILE2), 0);
    log_expect_errors(true);
    KT_EQ(r, vfs_rename(TEST_FILE2, TEST_FILE), SYS_EEXIST);
    KT_EQ(r, vfs_rename("/NOSUCH.TXT", "/ALSONO.TXT"), SYS_ENOENT);
    log_expect_errors(false);

    /* --- names FAT cannot express are refused, not truncated ---------- */
    log_expect_errors(true);
    /* 8.3 cannot hold an eleven-character stem. ENAMETOOLONG, not ENOSPC:
     * the disk is fine, the name is not, and the two suggest different
     * fixes. This started as ENOSPC because the backend mapped every create
     * failure to one code. */
    KT_EQ(r, fd_open("/TOOLONGNAME.TXT", O_WRONLY | O_CREAT), SYS_ENAMETOOLONG);
    KT_EQ(r, vfs_mkdir("/NOSUCHDIR/CHILD"), SYS_ENOENT);
    log_expect_errors(false);

    /* --- removal, and the space coming back --------------------------- */
    KT_EQ(r, vfs_unlink(TEST_DIR "/INNER.TXT"), 0);
    KT_EQ(r, vfs_rmdir(TEST_DIR), 0);
    KT_EQ(r, vfs_unlink(TEST_FILE), 0);
    KT_EQ(r, vfs_unlink(TEST_FILE2), 0);

    log_expect_errors(true);
    KT_EQ(r, vfs_stat(TEST_FILE, &st), SYS_ENOENT);
    KT_EQ(r, vfs_stat(TEST_DIR, &st), SYS_ENOENT);
    KT_EQ(r, vfs_unlink(TEST_FILE), SYS_ENOENT);
    KT_EQ(r, vfs_rmdir(TEST_DIR), SYS_ENOENT);
    log_expect_errors(false);

    /* Every cluster that was allocated has been returned. This is the
     * assertion that catches a leak in the allocator, and it is why the
     * free count was taken before anything was created: the number is read
     * off the allocation table both times, so it cannot agree by accident
     * with a running total the driver kept. */
    u32 free_after = 0;

    KT_ASSERT(r, fat16_space(&free_after, NULL));
    KT_EQ(r, free_after, free_before);

    /* And the directory entries are gone, not merely unreachable: a scan of
     * the root must not find the name. */
    rootfd = fd_open("/", O_RDONLY);
    if (rootfd >= 0) {
        struct sys_dirent de;
        bool still_there = false;

        for (u32 i = 0; i < 256; i++) {
            if (fd_getdent(rootfd, i, &de) != 1)
                break;
            if (strcasecmp(de.name, "KTEST.TXT") == 0)
                still_there = true;
        }

        KT_ASSERT(r, !still_there);
        KT_EQ(r, fd_close(rootfd), 0);
    }

    KT_EQ(r, fat16_sync(), true);
    KT_EQ(r, vfs_open_count(), open_before);
}

static void test_vfs(struct ktest_result *r)
{
    test_vfs_paths(r);

    u32 open_before = vfs_open_count();

    /* --- /dev/zero and /dev/null: the character-device backend --------- */
    i32 err = 0;
    struct vfs_file *z = vfs_open("/DEV/ZERO", O_RDONLY, &err);

    KT_ASSERT(r, z != NULL);
    KT_EQ(r, err, 0);

    if (z) {
        u8 buf[16];

        memset(buf, 0xAA, sizeof(buf));
        KT_EQ(r, vfs_read(z, buf, sizeof(buf)), (i32)sizeof(buf));

        bool all_zero = true;
        for (size_t i = 0; i < sizeof(buf); i++)
            if (buf[i] != 0)
                all_zero = false;
        KT_ASSERT(r, all_zero);

        /* A character device has no size and no position. Asserting the
         * refusal matters as much as asserting the read: a seek that
         * silently succeeded would let a caller believe it had moved. */
        struct sys_stat st;

        KT_EQ(r, vfs_stat_file(z, &st), 0);
        KT_EQ(r, st.type, (unsigned)S_TYPE_CHAR);
        KT_EQ(r, st.size, 0u);
        KT_EQ(r, vfs_seek(z, 0, SEEK_SET), SYS_EINVAL);

        /* Opened read-only, so a write is refused by the access mode - in
         * the dispatcher, not in the backend, which is the point of checking
         * it in one place. */
        KT_EQ(r, vfs_write(z, "x", 1), SYS_EINVAL);

        vfs_put(z);
    }

    struct vfs_file *n = vfs_open("/DEV/NULL", O_WRONLY, &err);

    KT_ASSERT(r, n != NULL);
    if (n) {
        KT_EQ(r, vfs_write(n, "discarded", 9), 9);
        KT_EQ(r, vfs_read(n, (char[16]){0}, 16), SYS_EINVAL); /* write-only */
        vfs_put(n);
    }

    /* Read-write, so both directions are permitted on one descriptor. */
    struct vfs_file *rw = vfs_open("/DEV/NULL", O_RDWR, &err);

    KT_ASSERT(r, rw != NULL);
    if (rw) {
        char tmp[4];

        KT_EQ(r, vfs_write(rw, "ab", 2), 2);
        KT_EQ(r, vfs_read(rw, tmp, sizeof(tmp)), 0); /* /dev/null is EOF */
        vfs_put(rw);
    }

    /* --- a missing device, and a missing filesystem -------------------- */
    log_expect_errors(true);
    KT_ASSERT(r, vfs_open("/DEV/NOSUCHDEVICE", O_RDONLY, &err) == NULL);
    KT_EQ(r, err, SYS_ENOENT);

    /* A relative path never reaches a backend: vfs_open() takes absolute
     * paths only, because resolution happens before it. */
    KT_ASSERT(r, vfs_open("relative/path", O_RDONLY, &err) == NULL);
    KT_EQ(r, err, SYS_EINVAL);
    log_expect_errors(false);

    /* --- /dev as a directory ------------------------------------------- */
    struct vfs_file *d = vfs_open("/DEV", O_RDONLY, &err);

    KT_ASSERT(r, d != NULL);
    if (d) {
        struct sys_dirent de;

        KT_EQ(r, d->type, (u8)S_TYPE_DIR);
        /* Reading bytes from a directory is refused on the type, by the
         * dispatcher - so a backend cannot forget to refuse it. */
        KT_EQ(r, vfs_read(d, (char[8]){0}, 8), SYS_EISDIR);

        KT_EQ(r, vfs_getdent(d, 0, &de), 1);
        KT_ASSERT(r, de.name[0] != '\0');

        /* Walk to the end and confirm it stops rather than repeating. */
        u32 count = 0;
        while (vfs_getdent(d, count, &de) == 1 && count < 64)
            count++;
        KT_ASSERT(r, count >= 4); /* console, tty, kbd, null, zero */
        KT_EQ(r, vfs_getdent(d, count, &de), 0);
        KT_EQ(r, vfs_getdent(d, 10000, &de), 0);

        vfs_put(d);
    }

    /* --- reference counting -------------------------------------------- */
    struct vfs_file *shared = vfs_open("/DEV/ZERO", O_RDONLY, &err);

    KT_ASSERT(r, shared != NULL);
    if (shared) {
        KT_EQ(r, shared->refcount, 1u);
        vfs_ref(shared);
        KT_EQ(r, shared->refcount, 2u);
        vfs_put(shared);
        KT_EQ(r, shared->refcount, 1u);
        /* Still usable after the extra reference went away. */
        KT_EQ(r, vfs_read(shared, (char[4]){0}, 4), 4);
        vfs_put(shared);
        /* And the slot is genuinely released, not merely marked. */
        KT_EQ(r, shared->refcount, 0u);
    }

    /* Every open above is matched by a put, so the pool is where it was.
     * This is the assertion that catches a leak, and it is the reason each
     * block above closes what it opened even though the kernel would survive
     * either way. */
    KT_EQ(r, vfs_open_count(), open_before);

    /* --- descriptors --------------------------------------------------- */
    i32 fd = fd_open("/dev/zero", O_RDONLY);

    KT_ASSERT(r, fd >= 0);

    if (fd >= 0) {
        char buf[8];

        KT_ASSERT(r, fd_get(fd) != NULL);
        KT_EQ(r, fd_read(fd, buf, sizeof(buf)), (i32)sizeof(buf));

        /* dup gives a second descriptor onto the *same* open file. The
         * refcount is the observable part; the shared offset is tested on a
         * real file below, where a position exists to share. */
        i32 dup = fd_dup(fd);

        KT_ASSERT(r, dup >= 0);
        KT_ASSERT(r, dup != fd);
        KT_ASSERT(r, fd_get(dup) == fd_get(fd));
        KT_EQ(r, fd_get(fd)->refcount, 2u);

        /* Closing one leaves the other working - which is the whole reason
         * descriptors and open files are separate objects. */
        KT_EQ(r, fd_close(dup), 0);
        KT_ASSERT(r, fd_get(dup) == NULL);
        KT_EQ(r, fd_get(fd)->refcount, 1u);
        KT_EQ(r, fd_read(fd, buf, sizeof(buf)), (i32)sizeof(buf));

        /* dup2 onto a specific number, and the no-op case. A dup2(fd, fd)
         * that closed before copying would free the last reference and then
         * duplicate a released slot. */
        KT_EQ(r, fd_dup2(fd, fd), fd);
        KT_ASSERT(r, fd_get(fd) != NULL);
        KT_EQ(r, fd_get(fd)->refcount, 1u);

        i32 target = (i32)SYS_FDS_MAX - 1;

        KT_EQ(r, fd_dup2(fd, target), target);
        KT_ASSERT(r, fd_get(target) == fd_get(fd));
        KT_EQ(r, fd_close(target), 0);

        /* Out-of-range descriptors, on both sides. */
        KT_EQ(r, fd_dup2(fd, (i32)SYS_FDS_MAX), SYS_EBADF);
        KT_EQ(r, fd_dup2(fd, -1), SYS_EBADF);

        KT_EQ(r, fd_close(fd), 0);
        /* Closing twice is EBADF, not a double free: fd_close clears the
         * table slot before dropping the reference. */
        KT_EQ(r, fd_close(fd), SYS_EBADF);
    }

    /* --- the bounds a hostile caller would probe ----------------------- */
    KT_ASSERT(r, fd_get(-1) == NULL);
    KT_ASSERT(r, fd_get((i32)SYS_FDS_MAX) == NULL);
    KT_ASSERT(r, fd_get(0x7FFFFFFF) == NULL);
    KT_EQ(r, fd_read(-1, (char[4]){0}, 4), SYS_EBADF);
    KT_EQ(r, fd_write((i32)SYS_FDS_MAX, "x", 1), SYS_EBADF);
    KT_EQ(r, fd_seek(99, 0, SEEK_SET), SYS_EBADF);
    KT_EQ(r, fd_close(99), SYS_EBADF);
    KT_EQ(r, fd_dup(99), SYS_EBADF);

    /* An unknown whence is refused even on a seekable descriptor. */
    i32 zfd = fd_open("/dev/zero", O_RDONLY);

    if (zfd >= 0) {
        KT_EQ(r, fd_seek(zfd, 0, 99), SYS_EINVAL);
        KT_EQ(r, fd_close(zfd), 0);
    }

    /* --- exhausting the descriptor table, and recovering -------------- */
    log_expect_errors(true);
    {
        i32 held[SYS_FDS_MAX];
        u32 got = 0;

        for (u32 i = 0; i < SYS_FDS_MAX; i++) {
            held[i] = fd_open("/dev/null", O_RDONLY);
            if (held[i] < 0)
                break;
            got++;
        }

        /* The next one must fail cleanly rather than overrun the table. */
        KT_EQ(r, fd_open("/dev/null", O_RDONLY), SYS_EMFILE);

        for (u32 i = 0; i < got; i++)
            KT_EQ(r, fd_close(held[i]), 0);

        /* And the table is usable again - an EMFILE that leaked the open
         * file behind it would have consumed pool slots permanently, which
         * is precisely what a ring-3 loop would exploit. */
        KT_EQ(r, vfs_open_count(), open_before);

        i32 again = fd_open("/dev/null", O_RDONLY);

        KT_ASSERT(r, again >= 0);
        if (again >= 0)
            KT_EQ(r, fd_close(again), 0);
    }
    log_expect_errors(false);

    /* --- the filesystem, through the same interface -------------------- */
    if (!fat16_mounted()) {
        /* The GRUB ISO path. The backend is registered but has nothing
         * mounted, and must say so rather than dereference it. */
        log_expect_errors(true);
        KT_EQ(r, fd_open("/bin/INIT", O_RDONLY), SYS_ENOENT);
        log_expect_errors(false);

        /* /dev still works, which is the reason the VFS comes up whether or
         * not a disk mounted. */
        i32 c = fd_open("/dev/console", O_WRONLY);

        KT_ASSERT(r, c >= 0);
        if (c >= 0)
            KT_EQ(r, fd_close(c), 0);

        KT_EQ(r, vfs_open_count(), open_before);
        return;
    }

    i32 ffd = fd_open("/bin/init", O_RDONLY);

    KT_ASSERT(r, ffd >= 0);

    if (ffd >= 0) {
        struct sys_stat st;
        u8 hdr[4];

        KT_EQ(r, fd_stat(ffd, &st), 0);
        KT_EQ(r, st.type, (unsigned)S_TYPE_FILE);
        KT_ASSERT(r, st.size > 0);
        KT_ASSERT(r, st.blocks > 0);

        /* It is an ELF, read through the VFS rather than through fat16_read
         * directly - which is what proves the two layers are actually
         * connected. */
        KT_EQ(r, fd_read(ffd, hdr, 4), 4);
        KT_EQ(r, hdr[0], 0x7Fu);
        KT_EQ(r, hdr[1], (u8)'E');
        KT_EQ(r, hdr[2], (u8)'L');
        KT_EQ(r, hdr[3], (u8)'F');

        /* The offset advanced, and seeking moves it. */
        KT_EQ(r, fd_seek(ffd, 0, SEEK_CUR), 4);
        KT_EQ(r, fd_seek(ffd, 0, SEEK_SET), 0);
        KT_EQ(r, fd_seek(ffd, 0, SEEK_END), (i32)st.size);
        KT_EQ(r, fd_read(ffd, hdr, 4), 0); /* at the end: EOF, not an error */

        /* Seeks outside the file are refused, in both directions. */
        KT_EQ(r, fd_seek(ffd, -1, SEEK_SET), SYS_EINVAL);
        KT_EQ(r, fd_seek(ffd, (i32)st.size + 1, SEEK_SET), SYS_EINVAL);
        KT_EQ(r, fd_seek(ffd, 1, SEEK_END), SYS_EINVAL);

        /* A short read at the end of the file, which is the one place a
         * short read is expected rather than an error. */
        KT_EQ(r, fd_seek(ffd, (i32)st.size - 2, SEEK_SET), (i32)st.size - 2);
        KT_EQ(r, fd_read(ffd, hdr, 4), 2);

        /* The shared offset, demonstrated where there is one to share. */
        i32 dfd = fd_dup(ffd);

        KT_ASSERT(r, dfd >= 0);
        if (dfd >= 0) {
            KT_EQ(r, fd_seek(ffd, 8, SEEK_SET), 8);
            /* Seen through the other descriptor, because both point at one
             * open file. */
            KT_EQ(r, fd_seek(dfd, 0, SEEK_CUR), 8);
            KT_EQ(r, fd_close(dfd), 0);
        }

        /* A descriptor opened O_RDONLY refuses a write regardless of
         * whether the filesystem could take one - the access mode is
         * checked by the dispatcher, before any backend is asked. */
        log_expect_errors(true);
        KT_EQ(r, fd_write(ffd, "x", 1), SYS_EINVAL);
        log_expect_errors(false);

        KT_EQ(r, fd_close(ffd), 0);
    }

    /* A directory on disk, listed through the same getdent as /dev. */
    i32 dfd = fd_open("/bin", O_RDONLY | O_DIRECTORY);

    KT_ASSERT(r, dfd >= 0);

    if (dfd >= 0) {
        struct sys_dirent de;
        bool found_init = false;
        u32 i = 0;

        while (fd_getdent(dfd, i, &de) == 1 && i < 64) {
            if (strcasecmp(de.name, "INIT") == 0)
                found_init = true;
            i++;
        }

        KT_ASSERT(r, i > 0);
        KT_ASSERT(r, found_init);
        KT_EQ(r, fd_read(dfd, (char[8]){0}, 8), SYS_EISDIR);
        KT_EQ(r, fd_close(dfd), 0);
    }

    /* A file opened with O_DIRECTORY, and a directory read as a file: the
     * two type confusions the flag exists to prevent. */
    log_expect_errors(true);
    KT_EQ(r, fd_open("/bin/init", O_RDONLY | O_DIRECTORY), SYS_ENOTDIR);
    KT_EQ(r, fd_open("/nosuchfile", O_RDONLY), SYS_ENOENT);
    KT_EQ(r, fd_open("/bin/nosuchfile", O_RDONLY), SYS_ENOENT);
    log_expect_errors(false);

    /* --- stat by path, and the working directory ----------------------- */
    struct sys_stat st;

    KT_EQ(r, vfs_stat("/BIN", &st), 0);
    KT_EQ(r, st.type, (unsigned)S_TYPE_DIR);
    KT_EQ(r, vfs_stat("/BIN/INIT", &st), 0);
    KT_EQ(r, st.type, (unsigned)S_TYPE_FILE);
    KT_EQ(r, vfs_stat("/DEV/NULL", &st), 0);
    KT_EQ(r, st.type, (unsigned)S_TYPE_CHAR);

    log_expect_errors(true);
    KT_EQ(r, vfs_stat("/NOSUCH", &st), SYS_ENOENT);
    log_expect_errors(false);

    /* chdir changes what a relative path means, and refuses a non-directory
     * rather than leaving the process somewhere that cannot be listed. */
    KT_EQ(r, fd_chdir("/bin"), 0);
    KT_ASSERT(r, strcmp(fd_cwd(), "/BIN") == 0);

    i32 rel = fd_open("init", O_RDONLY);

    KT_ASSERT(r, rel >= 0);
    if (rel >= 0)
        KT_EQ(r, fd_close(rel), 0);

    KT_EQ(r, fd_chdir(".."), 0);
    KT_ASSERT(r, strcmp(fd_cwd(), "/") == 0);

    log_expect_errors(true);
    KT_EQ(r, fd_chdir("/bin/init"), SYS_ENOTDIR);
    KT_EQ(r, fd_chdir("/nosuchdir"), SYS_ENOENT);
    log_expect_errors(false);

    /* The failed chdirs left the working directory alone. */
    KT_ASSERT(r, strcmp(fd_cwd(), "/") == 0);

    test_vfs_write(r);

    /* Nothing leaked across the whole suite. */
    KT_EQ(r, vfs_open_count(), open_before);
}

/* ---------------------------------------------------------------------------
 * The three protocols that reach the internet: their parsers, not their
 * wires.
 *
 * The wire is tested by the `internet` CI scenario, which boots this kernel
 * against QEMU's own DHCP server and a real HTTP server on the host. What is
 * tested here is everything that can be got wrong without a network, and the
 * most important of it is the DNS name parser - because that one has a
 * remote attacker for an input and a denial of service for a bug.
 * ------------------------------------------------------------------------- */

static void test_inet(struct ktest_result *r)
{
    /* --- URLs ------------------------------------------------------------ */
    {
        char host[HTTP_HOST_MAX], path[HTTP_URL_MAX];
        u16 port;
        bool tls;

        KT_ASSERT(r, http_parse_url("http://example.com/", host, sizeof(host),
                                    &port, path, sizeof(path), &tls));
        KT_ASSERT(r, strcmp(host, "example.com") == 0);
        KT_EQ(r, (u32)port, 80u);
        KT_ASSERT(r, strcmp(path, "/") == 0);
        KT_ASSERT(r, !tls);

        /* No scheme, which is what a person types. */
        KT_ASSERT(r, http_parse_url("example.com", host, sizeof(host), &port,
                                    path, sizeof(path), &tls));
        KT_ASSERT(r, strcmp(host, "example.com") == 0);
        /* An absent path is "/", not the empty string: a request line with
         * an empty target is malformed and a server answers it with a 400. */
        KT_ASSERT(r, strcmp(path, "/") == 0);

        /* An explicit port. */
        KT_ASSERT(r, http_parse_url("http://10.0.2.2:8080/a/b?c=d", host,
                                    sizeof(host), &port, path, sizeof(path),
                                    &tls));
        KT_ASSERT(r, strcmp(host, "10.0.2.2") == 0);
        KT_EQ(r, (u32)port, 8080u);
        KT_ASSERT(r, strcmp(path, "/a/b?c=d") == 0);

        /* A query with no path. */
        KT_ASSERT(r, http_parse_url("host?q=1", host, sizeof(host), &port, path,
                                    sizeof(path), &tls));
        KT_ASSERT(r, strcmp(path, "?q=1") == 0);

        /* https is understood and refused, and the two are different: the
         * caller has to be able to say "there is no TLS here" rather than
         * "that is not a URL". */
        KT_ASSERT(r, !http_parse_url("https://example.com/", host, sizeof(host),
                                     &port, path, sizeof(path), &tls));
        KT_ASSERT(r, tls);

        /* Rejections. */
        KT_ASSERT(r, !http_parse_url("", host, sizeof(host), &port, path,
                                     sizeof(path), &tls));
        KT_ASSERT(r, !http_parse_url("http://", host, sizeof(host), &port, path,
                                     sizeof(path), &tls));
        /* Port 0 is not a port, and a port above 65535 does not wrap. */
        KT_ASSERT(r, !http_parse_url("http://h:0/", host, sizeof(host), &port,
                                     path, sizeof(path), &tls));
        KT_ASSERT(r, !http_parse_url("http://h:99999/", host, sizeof(host),
                                     &port, path, sizeof(path), &tls));
        KT_ASSERT(r, !http_parse_url("http://h:80x/", host, sizeof(host), &port,
                                     path, sizeof(path), &tls));
        KT_ASSERT(r, !http_parse_url("http://h:/", host, sizeof(host), &port,
                                     path, sizeof(path), &tls));
        /* A host longer than the buffer is refused rather than truncated: a
         * truncated host name is a request to the wrong server. */
        {
            char longurl[HTTP_HOST_MAX + 32];

            memset(longurl, 'a', sizeof(longurl) - 1);
            longurl[sizeof(longurl) - 1] = '\0';
            memcpy(longurl, "http://", 7);
            KT_ASSERT(r, !http_parse_url(longurl, host, sizeof(host), &port,
                                         path, sizeof(path), &tls));
        }
    }

    /* --- DNS name compression -------------------------------------------
     *
     * The parser whose input is a stranger's datagram. Every one of these is
     * a message a malicious server could send, and the assertion is that
     * none of them makes the parser read out of bounds or loop.
     */
    {
        /* "www.example.com" then the root label: a plain name. */
        static const u8 plain[] = {3,   'w', 'w', 'w', 7,   'e', 'x', 'a', 'm',
                                   'p', 'l', 'e', 3,   'c', 'o', 'm', 0};

        KT_EQ(r, dns_skip_name(plain, sizeof(plain), 0), 17);

        /* A name that ends in a pointer back to offset 0. Legal, and the
         * offset returned is just past the *pointer*, not past the name it
         * leads to - which is the subtlety: the two are different and using
         * the wrong one desynchronises the whole message. */
        static const u8 compressed[] = {
            3, 'w', 'w', 'w',  0,    /* "www." at offset 0, 5 bytes  */
            2, 'u', 's', 0xC0, 0x00, /* "us" then a pointer to 0      */
        };

        /* Offset 5 is the 2-byte label "us" (5..7), then the pointer at
         * 8..9, so the name ends at 10 - two bytes before the end of the
         * message, not at it. */
        KT_EQ(r, dns_skip_name(compressed, sizeof(compressed), 5), 10);

        /* A pointer to itself. The classic infinite loop, and the reason the
         * parser requires a pointer to go strictly backwards. */
        static const u8 self[] = {0xC0, 0x00};

        KT_ASSERT(r, dns_skip_name(self, sizeof(self), 0) < 0);

        /* A pointer forwards. Also a loop, built from two pointers that
         * reference each other, and rejected by the same rule. */
        static const u8 forward[] = {0xC0, 0x02, 0x00, 0xC0, 0x00};

        KT_ASSERT(r, dns_skip_name(forward, sizeof(forward), 0) < 0);

        /* A label whose length runs off the end of the message. */
        static const u8 overrun[] = {40, 'a', 'b', 'c'};

        KT_ASSERT(r, dns_skip_name(overrun, sizeof(overrun), 0) < 0);

        /* A pointer whose second byte is not in the message at all. */
        static const u8 short_ptr[] = {0xC0};

        KT_ASSERT(r, dns_skip_name(short_ptr, sizeof(short_ptr), 0) < 0);

        /* An offset past the end. */
        KT_ASSERT(r, dns_skip_name(plain, sizeof(plain), 99) < 0);

        /* The reserved label types, 0x40 and 0x80. Neither is defined, and a
         * parser that treats them as lengths reads whatever follows as a
         * label. */
        static const u8 reserved[] = {0x40, 0, 0, 0};

        KT_ASSERT(r, dns_skip_name(reserved, sizeof(reserved), 0) < 0);

        static const u8 reserved2[] = {0x80, 0, 0, 0};

        KT_ASSERT(r, dns_skip_name(reserved2, sizeof(reserved2), 0) < 0);

        /* A chain of backward pointers longer than the jump cap. Each step
         * is individually legal - every pointer goes backwards and stays in
         * bounds - so this is the case the first two defences do not catch,
         * and the reason the cap exists at all. */
        {
            static u8 chain[64];

            chain[0] = 0; /* the root label, so the chain terminates */
            chain[1] = 0;
            for (u32 i = 1; i < 32; i++) {
                chain[i * 2] = 0xC0;
                chain[i * 2 + 1] = (u8)((i - 1) * 2);
            }

            KT_ASSERT(r, dns_skip_name(chain, sizeof(chain), 62) < 0);
        }
    }

    /* --- the resolver's own behaviour ----------------------------------- */
    {
        ipv4_addr a = 0;

        /* A dotted quad is its own answer, with no query and no resolver.
         * This is what makes `fetch 10.0.2.2` work on a machine that has no
         * DNS server configured at all. */
        KT_EQ(r, dns_resolve("10.0.2.2", &a, 0), 0);
        KT_ASSERT(r, a == IPV4(10, 0, 2, 2));

        KT_ASSERT(r, dns_resolve("", &a, 0) == SYS_EINVAL);
        KT_ASSERT(r, dns_resolve(NULL, &a, 0) == SYS_EINVAL);

        /* The cache. A dotted quad does not enter it, because it never
         * needed a lookup. */
        dns_cache_clear();
        KT_EQ(r, dns_cache_count(), 0u);
        KT_EQ(r, dns_resolve("10.0.2.2", &a, 0), 0);
        KT_EQ(r, dns_cache_count(), 0u);

        KT_ASSERT(r, strcmp(dns_status_name(DNS_NOT_FOUND), "not found") == 0);
        KT_ASSERT(r, strcmp(dns_status_name(DNS_TIMEOUT), "timed out") == 0);
    }

    /* --- DHCP ------------------------------------------------------------ */
    {
        /* The point being asserted is the one from the top of dhcp.c: the
         * kernel's configuration is untouched until an ACK arrives, so a
         * lease that is not valid must not have been written anywhere. */
        const struct dhcp_lease *l = dhcp_get_lease();
        const struct net_config *cfg = net_get_config();

        if (dhcp_state() == DHCP_IDLE) {
            KT_ASSERT(r, !l->valid);
            KT_EQ(r, l->acks_received, 0u);
            /* And the static address is still the one net_init() set - on a
             * machine that has a controller at all. With no controller
             * net_init() never runs, so the address is zero and that is
             * correct rather than a missing configuration. */
            KT_ASSERT(r, !net_device() || cfg->addr != 0);
        } else {
            /* Something already ran it - the `internet` scenario does - in
             * which case the lease and the configuration must agree, which
             * is the same assertion from the other side. */
            KT_ASSERT(r, !l->valid || cfg->addr == l->addr);
            KT_ASSERT(r, l->valid || !net_device() || cfg->addr != 0);
        }

        KT_ASSERT(r, strcmp(dhcp_state_name(DHCP_BOUND), "bound") == 0);
        KT_ASSERT(r, strcmp(dhcp_state_name(DHCP_FAILED), "failed") == 0);
    }

    /* --- UDP's bound ports ----------------------------------------------
     *
     * The queue DNS and DHCP both sit on. Tested here rather than in the
     * `net` suite because it arrived with them.
     */
    {
        KT_ASSERT(r, !udp_bind(0));
        /* The echo service answers from inside udp_input() and keeps
         * nothing, so a binding on top of it would see datagrams that had
         * already been replied to. One owner per port. */
        KT_ASSERT(r, !udp_bind(udp_echo_port()));

        KT_ASSERT(r, udp_bind(5353));
        KT_ASSERT(r, udp_port_bound(5353));
        KT_ASSERT(r, !udp_bind(5353));
        KT_EQ(r, udp_queued(5353), 0u);

        struct udp_dgram d;

        /* An empty queue is 0, which is not an error - and a port nobody
         * bound is an error, which is a different answer. */
        KT_EQ(r, udp_recv(5353, &d), 0);
        KT_EQ(r, udp_recv(5354, &d), SYS_EINVAL);

        udp_unbind(5353);
        KT_ASSERT(r, !udp_port_bound(5353));
        KT_EQ(r, udp_recv(5353, &d), SYS_EINVAL);

        u16 e1 = udp_bind_ephemeral();
        u16 e2 = udp_bind_ephemeral();

        KT_ASSERT(r, e1 >= 49152 && e2 >= 49152);
        KT_ASSERT(r, e1 != e2);
        udp_unbind(e1);
        udp_unbind(e2);
    }

    /* --- the HTTP client's counters ------------------------------------- */
    {
        const struct http_stats *hs = http_get_stats();

        /* Not asserted as zero: the `internet` scenario fetches a URL before
         * this suite runs. What has to hold either way is that every request
         * was either answered or counted as a failure. */
        KT_ASSERT(r, hs->responses + hs->failures <= hs->requests);
    }
}

/* ---------------------------------------------------------------------------
 * Sockets, from inside the kernel.
 *
 * The interesting half of the socket layer is tested from ring 3 by
 * user/netcat.c, which is where it belongs: the point of a socket being a
 * descriptor is that a *process* can use it, and a kernel thread calling
 * socket() proves less. What is tested here is the half ring 3 cannot reach
 * conveniently - the refusals, the type checks and the resource bounds -
 * plus loopback, which nothing else can assert on without a peer.
 * ------------------------------------------------------------------------- */

static void test_sockets(struct ktest_result *r)
{
    u32 open_before = vfs_open_count();

    /* --- what socket() refuses --------------------------------------- */
    KT_EQ(r, socket_create(99, SOCK_STREAM, 0), SYS_ENOTSUP);
    KT_EQ(r, socket_create(AF_INET, 99, 0), SYS_ENOTSUP);
    /* A protocol of 0 means "the usual one", and naming the wrong one
     * explicitly is an error rather than a hint. */
    KT_EQ(r, socket_create(AF_INET, SOCK_STREAM, IPPROTO_UDP), SYS_ENOTSUP);
    KT_EQ(r, socket_create(AF_INET, SOCK_DGRAM, IPPROTO_TCP), SYS_ENOTSUP);

    /* --- a socket is a descriptor ------------------------------------ */
    {
        i32 fd = socket_create(AF_INET, SOCK_STREAM, 0);

        KT_ASSERT(r, fd >= 0);

        struct vfs_file *f = fd_get(fd);

        KT_ASSERT(r, f != NULL);
        KT_EQ(r, (u32)f->type, (u32)S_TYPE_SOCKET);

        struct sys_stat st;

        KT_EQ(r, vfs_stat_file(f, &st), 0);
        KT_EQ(r, st.type, (u32)S_TYPE_SOCKET);
        /* A stream has no position, so the VFS refuses the seek once rather
         * than the socket layer saying so. The NULL op is the answer. */
        KT_EQ(r, vfs_seek(f, 0, SEEK_SET), SYS_EINVAL);
        /* And no directory entries, for the same reason. */
        struct sys_dirent de;

        KT_EQ(r, vfs_getdent(f, 0, &de), SYS_ENOTDIR);

        /* Nothing is connected, so a read and a write have nowhere to go -
         * and ENOTCONN is a different answer from EBADF, which is the
         * distinction a caller acts on. */
        char buf[4];

        KT_EQ(r, vfs_read(f, buf, sizeof(buf)), SYS_ENOTCONN);
        KT_EQ(r, vfs_write(f, buf, sizeof(buf)), SYS_ENOTCONN);

        /* The non-blocking flag lives on the open file, which is where every
         * other blocking decision in this kernel reads it from. */
        KT_EQ(r, vfs_ioctl(f, FIONBIO, 1), 0);
        KT_ASSERT(r, (f->flags & O_NONBLOCK) != 0);
        KT_EQ(r, vfs_ioctl(f, FIONBIO, 0), 0);
        KT_ASSERT(r, (f->flags & O_NONBLOCK) == 0);
        KT_EQ(r, vfs_ioctl(f, 0x9999, 0), SYS_ENOTSUP);

        KT_EQ(r, fd_close(fd), 0);
    }

    /* --- the wrong calls on the wrong things ------------------------- */
    {
        struct sys_sockaddr sa;

        memset(&sa, 0, sizeof(sa));
        sa.family = AF_INET;
        sa.port = htons(9201);

        /* Not a descriptor at all. */
        KT_EQ(r, socket_bind(99, &sa), SYS_EBADF);
        KT_EQ(r, socket_listen(99, 1), SYS_EBADF);
        KT_EQ(r, socket_connect(99, &sa), SYS_EBADF);

        /* A real descriptor that is not a socket. ENOTSUP rather than EBADF:
         * the descriptor is fine, the operation is not - and conflating the
         * two would send a caller looking for a closed file. */
        i32 dev = fd_open("/dev/null", O_RDWR);

        KT_ASSERT(r, dev >= 0);
        KT_EQ(r, socket_bind(dev, &sa), SYS_ENOTSUP);
        KT_EQ(r, socket_listen(dev, 1), SYS_ENOTSUP);
        KT_EQ(r, socket_accept(dev, NULL), SYS_ENOTSUP);
        KT_EQ(r, fd_close(dev), 0);

        /* A datagram socket cannot listen, and a stream socket has no
         * sendto. */
        i32 d = socket_create(AF_INET, SOCK_DGRAM, 0);
        i32 t = socket_create(AF_INET, SOCK_STREAM, 0);

        KT_ASSERT(r, d >= 0 && t >= 0);
        KT_EQ(r, socket_listen(d, 1), SYS_ENOTSUP);
        KT_EQ(r, socket_shutdown(d, SHUT_WR), SYS_ENOTSUP);
        KT_EQ(r, socket_sendto(t, "x", 1, &sa), SYS_ENOTSUP);
        KT_EQ(r, socket_recvfrom(t, NULL, 0, NULL), SYS_ENOTSUP);

        /* listen() before bind() has no port to listen on. */
        KT_EQ(r, socket_listen(t, 1), SYS_EINVAL);
        /* accept() on something that is not listening. */
        KT_EQ(r, socket_accept(t, NULL), SYS_EINVAL);
        /* shutdown() on an unconnected stream. */
        KT_EQ(r, socket_shutdown(t, SHUT_WR), SYS_ENOTCONN);
        KT_EQ(r, socket_shutdown(t, 99), SYS_EINVAL);

        /* Port 0 is not a port, and a family we do not have is not a
         * family. */
        struct sys_sockaddr bad = sa;

        bad.port = 0;
        KT_EQ(r, socket_bind(t, &bad), SYS_EINVAL);
        bad = sa;
        bad.family = 99;
        KT_EQ(r, socket_bind(t, &bad), SYS_ENOTSUP);

        KT_EQ(r, fd_close(d), 0);
        KT_EQ(r, fd_close(t), 0);
    }

    /* --- bind, listen, and the second bind that must fail ------------ */
    {
        struct sys_sockaddr sa;

        memset(&sa, 0, sizeof(sa));
        sa.family = AF_INET;
        sa.port = htons(9202);

        i32 a = socket_create(AF_INET, SOCK_STREAM, 0);
        i32 b = socket_create(AF_INET, SOCK_STREAM, 0);

        KT_ASSERT(r, a >= 0 && b >= 0);
        KT_EQ(r, socket_bind(a, &sa), 0);
        KT_EQ(r, socket_listen(a, 4), 0);
        /* A second listener on the same port would make the destination
         * port ambiguous. The bind itself succeeds - a stream socket's bind
         * only records the number - and the listen is what claims it. */
        KT_EQ(r, socket_bind(b, &sa), 0);
        KT_EQ(r, socket_listen(b, 4), SYS_EADDRINUSE);
        /* Binding twice is a caller error either way. */
        KT_EQ(r, socket_bind(a, &sa), SYS_EINVAL);
        /* Nothing has connected, and a non-blocking accept says so rather
         * than waiting. */
        {
            struct vfs_file *f = fd_get(a);

            KT_EQ(r, vfs_ioctl(f, FIONBIO, 1), 0);
            KT_EQ(r, socket_accept(a, NULL), SYS_EAGAIN);
        }
        /* And a listener cannot then connect. */
        KT_EQ(r, socket_connect(a, &sa), SYS_EINVAL);

        KT_EQ(r, fd_close(b), 0);
        KT_EQ(r, fd_close(a), 0);

        /* Closing the listener released the port, or a program that opened
         * and closed a listening socket could never open it again. */
        i32 c = socket_create(AF_INET, SOCK_STREAM, 0);

        KT_ASSERT(r, c >= 0);
        KT_EQ(r, socket_bind(c, &sa), 0);
        KT_EQ(r, socket_listen(c, 4), 0);
        KT_EQ(r, fd_close(c), 0);
    }

    /* --- a datagram socket's bind and default peer ------------------- */
    {
        struct sys_sockaddr sa;

        memset(&sa, 0, sizeof(sa));
        sa.family = AF_INET;
        sa.port = htons(9203);

        i32 a = socket_create(AF_INET, SOCK_DGRAM, 0);
        i32 b = socket_create(AF_INET, SOCK_DGRAM, 0);

        KT_ASSERT(r, a >= 0 && b >= 0);
        KT_EQ(r, socket_bind(a, &sa), 0);
        /* A datagram bind really claims the port, because a datagram socket
         * owns a receive queue. The second one has nowhere to put what
         * arrives. */
        KT_EQ(r, socket_bind(b, &sa), SYS_EADDRINUSE);

        /* An unconnected datagram socket has nothing to write to. */
        struct vfs_file *f = fd_get(b);

        KT_EQ(r, vfs_write(f, "x", 1), SYS_ENOTCONN);

        /* connect() on a datagram socket sends nothing and cannot fail for
         * any reason to do with the network - it records a default peer,
         * which is what makes send() and recv() work on one. */
        sa.addr = IPV4(10, 0, 2, 2);
        KT_EQ(r, socket_connect(b, &sa), 0);

        KT_EQ(r, fd_close(a), 0);
        KT_EQ(r, fd_close(b), 0);
    }

    /* --- socket options ---------------------------------------------- */
    {
        i32 fd = socket_create(AF_INET, SOCK_STREAM, 0);
        u32 v = 0;

        KT_ASSERT(r, fd >= 0);

        KT_EQ(r, socket_getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &v), 0);
        KT_ASSERT(r, v > 0); /* there is a default, and it is not infinite */

        KT_EQ(r, socket_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, 1500), 0);
        KT_EQ(r, socket_getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &v), 0);
        KT_EQ(r, v, 1500u);

        /* Bounded, for the same reason sleep() is: a ring-3 program must not
         * be able to ask for a timeout long enough to look like a hang. */
        KT_EQ(r, socket_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, 0xFFFFFFFFu),
              0);
        KT_EQ(r, socket_getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &v), 0);
        KT_ASSERT(r, v <= 60000u);

        /* Zero means the default rather than "never". */
        KT_EQ(r, socket_setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, 0), 0);
        KT_EQ(r, socket_getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &v), 0);
        KT_ASSERT(r, v > 0);

        KT_EQ(r, socket_setsockopt(fd, 99, SO_RCVTIMEO, 1), SYS_ENOTSUP);
        KT_EQ(r, socket_setsockopt(fd, SOL_SOCKET, 99, 1), SYS_ENOTSUP);
        KT_EQ(r, socket_getsockopt(fd, SOL_SOCKET, 99, &v), SYS_ENOTSUP);

        /* SO_ERROR reads and clears, as Berkeley sockets define it: the
         * error belongs to the operation that failed, and leaving it set
         * would make the next read report a failure already seen. */
        KT_EQ(r, socket_getsockopt(fd, SOL_SOCKET, SO_ERROR, &v), 0);
        KT_EQ(r, v, 0u);

        KT_EQ(r, fd_close(fd), 0);
    }

    /* --- the table's ceiling ----------------------------------------
     *
     * A ring-3 loop calling socket() has to hit a bound rather than an
     * allocator, which is the same argument the open-file table makes.
     * Whichever limit is reached first, the answer is a refusal and not a
     * crash - and the sockets opened along the way all close again.
     */
    {
        i32 fds[SOCKET_MAX + 4];
        u32 made = 0;

        for (u32 i = 0; i < ARRAY_SIZE(fds); i++) {
            fds[i] = socket_create(AF_INET, SOCK_DGRAM, 0);
            if (fds[i] < 0)
                break;
            made++;
        }

        /* It stopped, and it stopped for a reason a caller can act on. */
        KT_ASSERT(r, made < ARRAY_SIZE(fds));
        KT_ASSERT(r, made > 0);

        i32 refused = socket_create(AF_INET, SOCK_DGRAM, 0);

        KT_ASSERT(r, refused == SYS_ENFILE || refused == SYS_EMFILE);

        for (u32 i = 0; i < made; i++)
            KT_EQ(r, fd_close(fds[i]), 0);

        /* And the ceiling was not permanent. */
        i32 again = socket_create(AF_INET, SOCK_DGRAM, 0);

        KT_ASSERT(r, again >= 0);
        KT_EQ(r, fd_close(again), 0);
    }

    /* --- loopback ---------------------------------------------------
     *
     * Nothing else in this suite can assert on this without a peer, and it
     * is the reason the ring-3 socket program works at all: a controller
     * does not receive its own transmissions, so a datagram to this
     * machine's own address has to be turned round in software.
     */
    {
        const struct net_config *cfg = net_get_config();

        KT_ASSERT(r, net_is_loopback(IPV4(127, 0, 0, 1)));
        KT_ASSERT(r, net_is_loopback(IPV4(127, 255, 255, 254)));
        KT_ASSERT(r, !net_is_loopback(IPV4(10, 0, 2, 2)));
        KT_ASSERT(r, !net_is_loopback(IPV4(128, 0, 0, 1)));

        if (cfg->addr) {
            KT_ASSERT(r, net_is_loopback(cfg->addr));

            /* A real round trip, with no network involved. The UDP echo
             * service answers it, which means the datagram went down
             * through IPv4, into the loopback queue, back up through
             * ipv4_input and udp_input, and out again. */
            u16 port = udp_bind_ephemeral();

            KT_ASSERT(r, port != 0);

            static const char payload[] = "loopback";
            const struct loopback_stats *ls = loopback_get_stats();
            u32 sent_before = ls->sent;

            KT_ASSERT(r, udp_send(cfg->addr, port, udp_echo_port(), payload,
                                  sizeof(payload) - 1));
            /* Queued, not delivered: the whole point of the queue is that a
             * send from inside a receive cannot nest. */
            KT_ASSERT(r, ls->sent > sent_before);
            KT_ASSERT(r, loopback_queued() > 0);

            /* Two drains: the first delivers the request to the echo
             * service, which queues the reply; the second delivers the
             * reply. One call cannot do both, by design. */
            for (u32 i = 0; i < 8 && !udp_queued(port); i++)
                loopback_drain();

            KT_ASSERT(r, udp_queued(port) > 0);

            struct udp_dgram d;
            i32 n = udp_recv(port, &d);

            KT_EQ(r, n, (i32)(sizeof(payload) - 1));
            KT_ASSERT(r, memcmp(d.data, payload, sizeof(payload) - 1) == 0);
            KT_ASSERT(r, d.src == cfg->addr);
            KT_EQ(r, (u32)d.src_port, (u32)udp_echo_port());

            udp_unbind(port);
        }
    }

    /* Nothing leaked across the whole suite. */
    KT_EQ(r, vfs_open_count(), open_before);
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
    {"vfs", "path resolution, open files, descriptors", test_vfs},
    {"smp", "ACPI, the local APIC, per-CPU state, locks, IPIs", test_smp},
    {"bootpd", "the AP bootstrap page directory", test_bootstrap_pd},
    {"longmode", "the 32 -> 64 -> 32 mode transition", test_longmode},
    {"net", "checksums, addresses, ARP, and the wire", test_net},
    {"inet", "URL parsing, DNS name compression, DHCP and HTTP state",
     test_inet},
    {"sock", "sockets as descriptors, their refusals, and loopback",
     test_sockets},
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
