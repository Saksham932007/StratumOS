/* StratumOS - virtual memory manager (two-level x86 paging).
 *
 * Paging is already enabled before this file runs: kernel/arch/x86/boot.asm
 * builds a page directory with an identity map of the first 4 MiB, a mapping
 * of 0xC0000000 onto physical 0, and a recursive entry, then jumps into the
 * higher half. The VMM adopts that directory, widens the linear map, and
 * drops the identity mapping it no longer needs.
 *
 * The recursive mapping
 * ---------------------
 * Once CR0.PG is set, a page table can only be written through a virtual
 * address - but page tables come from the physical allocator, which can hand
 * back a frame anywhere in RAM, including far outside the linear map. Mapping
 * all of physical memory to work around that does not scale and is impossible
 * on a 4 GiB machine in a 32-bit address space.
 *
 * So the last page directory entry points at the directory itself. The
 * hardware walk then resolves
 *
 *     0xFFFFF000            -> the page directory
 *     0xFFC00000 + i*4096   -> the page table for directory entry i
 *
 * because it uses the directory as both levels. Every page table in the
 * system becomes addressable at a computable address, at the cost of one
 * 4 MiB slot at the very top.
 *
 * See docs/MEMORY.md for the full address-space layout.
 */
#define LOG_TAG "vmm"

#include <arch/harden.h>
#include <arch/idt.h>
#include <arch/io.h>

#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/string.h>

#include <mm/pmm.h>
#include <mm/vmm.h>

#define RECURSIVE_SLOT 1023
#define PD_VADDR       0xFFFFF000u
#define PT_VADDR(pdi)  (0xFFC00000u + ((u32)(pdi) << PAGE_SHIFT))

/* Built by _start; its symbol value is a physical address, because .boot is
 * linked at its load address. */
extern u8 boot_page_directory[];

static paddr_t kernel_pd_phys;
static bool initialised;
static u32 stat_page_tables;
static u32 stat_mapped_pages;
static u32 stat_page_faults;
static u32 stat_cow_faults;
static u32 stat_cow_copies;
static u32 stat_address_spaces;

/* Page tables are always reached through the recursive window. Before the
 * higher-half jump there was a physical path as well; there no longer is,
 * because C never runs with paging off. */
static inline u32 *pd_entries(void)
{
    return (u32 *)PD_VADDR;
}

static inline u32 *pt_entries(u32 pdi)
{
    return (u32 *)PT_VADDR(pdi);
}

paddr_t vmm_kernel_pd_phys(void)
{
    return kernel_pd_phys;
}

/* ------------------------------------------------------------------------- */
/* Temporary mappings                                                        */
/* ------------------------------------------------------------------------- */
/*
 * The recursive window reaches only the *current* address space's tables, but
 * fork has to build the child's while the parent's is loaded. These two slots
 * are how a frame that is mapped nowhere becomes writable for a moment.
 *
 * Two slots, because cloning needs the child's page directory and one of its
 * page tables visible at once. They are a fixed resource, so every use is a
 * short, non-nesting critical section - asserted rather than assumed.
 */
static bool temp_slot_busy[VMM_TEMP_SLOTS];

static void *temp_map(unsigned slot, paddr_t frame)
{
    ASSERT(slot < VMM_TEMP_SLOTS);
    ASSERT(!temp_slot_busy[slot]);

    vaddr_t va = VMM_TEMP_BASE + slot * PAGE_SIZE;
    u32 *pt = pt_entries(PDE_INDEX(va));

    temp_slot_busy[slot] = true;
    pt[PTE_INDEX(va)] = (frame & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITE;
    invlpg(va);

    return (void *)va;
}

static void temp_unmap(unsigned slot)
{
    ASSERT(slot < VMM_TEMP_SLOTS);

    vaddr_t va = VMM_TEMP_BASE + slot * PAGE_SIZE;
    u32 *pt = pt_entries(PDE_INDEX(va));

    pt[PTE_INDEX(va)] = 0;
    invlpg(va);
    temp_slot_busy[slot] = false;
}

/* Replace a present page's entry outright, keeping nothing. Used by the
 * copy-on-write handler, which needs to change both the frame and the flags
 * in one step - vmm_protect() keeps the frame and vmm_map() would warn about
 * replacing a live mapping. */
static void set_pte(vaddr_t va, u32 entry)
{
    u32 pdi = PDE_INDEX(va);

    ASSERT(pd_entries()[pdi] & PTE_PRESENT);
    pt_entries(pdi)[PTE_INDEX(va)] = entry;
    invlpg(va);
}

/* Create the page table for a directory slot if it is missing. */
static bool ensure_table(u32 pdi, u32 flags)
{
    u32 *pd = pd_entries();

    if (pd[pdi] & PTE_PRESENT) {
        /* A directory entry's USER bit gates its whole 4 MiB range, so it has
         * to be widened if any page inside it becomes user-accessible. */
        if (flags & PTE_USER)
            pd[pdi] |= PTE_USER;
        return true;
    }

    /* A kernel page table created now would be missing from every address
     * space already cloned from the kernel's, because a clone copies the
     * kernel's directory entries once, at creation. The mapping would then
     * work in whichever address space was current and fault in the others -
     * a bug that appears only when the wrong process is scheduled.
     *
     * So every kernel region claims its tables in vmm_init(), and arriving
     * here afterwards means one was missed. That is a bug in the kernel's
     * own layout, not a runtime condition to recover from. */
    if (initialised && pdi >= KERNEL_PDE_FIRST)
        panic("a kernel page table for directory slot %u (covering %p) is "
              "being created after vmm_init(); it would be missing from "
              "every existing address space - add the region to "
              "vmm_reserve_kernel_tables()",
              pdi, (void *)(pdi << 22));

    paddr_t frame = pmm_alloc_frame();
    if (frame == PMM_NO_FRAME) {
        pr_err("cannot allocate a page table for directory slot %u", pdi);
        return false;
    }

    pd[pdi] = frame | PTE_PRESENT | PTE_WRITE | (flags & PTE_USER);
    stat_page_tables++;

    /* The new table becomes reachable through the recursive window as soon as
     * the directory entry is live, but the TLB may still hold the old
     * (not-present) translation for that window address. */
    invlpg(PT_VADDR(pdi));

    memset(pt_entries(pdi), 0, PAGE_SIZE);
    return true;
}

bool vmm_map(vaddr_t va, paddr_t pa, u32 flags)
{
    u32 pdi = PDE_INDEX(va);
    u32 pti = PTE_INDEX(va);

    if (pdi == RECURSIVE_SLOT)
        panic("vmm_map(%p): refusing to map over the recursive page-table "
              "window - every page table would become unreachable",
              (void *)va);

    if (!ensure_table(pdi, flags))
        return false;

    u32 *pt = pt_entries(pdi);

    if (pt[pti] & PTE_PRESENT) {
        /* Silently replacing a live mapping hides bugs, so say so. The
         * mapping is still replaced, because remapping is legitimate. */
        pr_warn("remapping %p (was frame %p, now %p)", (void *)va,
                (void *)(pt[pti] & PTE_ADDR_MASK), (void *)pa);
        if (pt[pti] & PTE_OWNED)
            pmm_free_frame(pt[pti] & PTE_ADDR_MASK);
        stat_mapped_pages--;
    }

    pt[pti] = (pa & PTE_ADDR_MASK) | (flags & 0xFFF) | PTE_PRESENT;
    stat_mapped_pages++;

    invlpg(va);
    return true;
}

bool vmm_alloc_at(vaddr_t va, u32 flags)
{
    paddr_t frame = pmm_alloc_frame();

    if (frame == PMM_NO_FRAME)
        return false;

    /* PTE_OWNED records that this mapping allocated its own frame, so
     * vmm_unmap() knows it is responsible for returning it. Without the
     * distinction, unmapping either leaks every frame or frees frames it does
     * not own - the VGA framebuffer being the memorable example. */
    if (!vmm_map(va, frame, flags | PTE_OWNED)) {
        pmm_free_frame(frame);
        return false;
    }

    return true;
}

bool vmm_protect(vaddr_t va, u32 flags)
{
    u32 pdi = PDE_INDEX(va);
    u32 pti = PTE_INDEX(va);
    u32 *pd = pd_entries();

    if (!(pd[pdi] & PTE_PRESENT))
        return false;

    u32 *pt = pt_entries(pdi);
    if (!(pt[pti] & PTE_PRESENT))
        return false;

    if (flags & PTE_USER)
        pd[pdi] |= PTE_USER;

    /* Keep the frame and the ownership bit; replace only the permissions. */
    u32 keep = pt[pti] & (PTE_ADDR_MASK | PTE_OWNED);
    pt[pti] = keep | (flags & 0xFFF) | PTE_PRESENT;

    invlpg(va);
    return true;
}

bool vmm_protect_range(vaddr_t va, size_t bytes, u32 flags)
{
    size_t pages = PAGE_ALIGN(bytes) / PAGE_SIZE;

    for (size_t i = 0; i < pages; i++)
        if (!vmm_protect(va + i * PAGE_SIZE, flags))
            return false;
    return true;
}

void vmm_unmap(vaddr_t va)
{
    u32 pdi = PDE_INDEX(va);
    u32 pti = PTE_INDEX(va);
    u32 *pd = pd_entries();

    if (!(pd[pdi] & PTE_PRESENT))
        return;

    u32 *pt = pt_entries(pdi);
    u32 entry = pt[pti];

    if (!(entry & PTE_PRESENT))
        return;

    if (entry & PTE_OWNED)
        pmm_free_frame(entry & PTE_ADDR_MASK);

    pt[pti] = 0;
    stat_mapped_pages--;

    invlpg(va);
}

bool vmm_map_range(vaddr_t va, paddr_t pa, size_t bytes, u32 flags)
{
    size_t pages = PAGE_ALIGN(bytes) / PAGE_SIZE;

    for (size_t i = 0; i < pages; i++) {
        if (!vmm_map(va + i * PAGE_SIZE, pa + i * PAGE_SIZE, flags)) {
            /* Roll back, so a partial failure does not leave a half-mapped
             * range behind for someone else to trip over. */
            for (size_t j = 0; j < i; j++)
                vmm_unmap(va + j * PAGE_SIZE);
            return false;
        }
    }
    return true;
}

void vmm_unmap_range(vaddr_t va, size_t bytes)
{
    size_t pages = PAGE_ALIGN(bytes) / PAGE_SIZE;

    for (size_t i = 0; i < pages; i++)
        vmm_unmap(va + i * PAGE_SIZE);
}

u32 vmm_pde_raw(u32 pdi)
{
    return pdi < 1024 ? pd_entries()[pdi] : 0;
}

u32 vmm_pte(vaddr_t va)
{
    u32 pdi = PDE_INDEX(va);
    u32 *pd = pd_entries();

    if (!(pd[pdi] & PTE_PRESENT))
        return 0;

    return pt_entries(pdi)[PTE_INDEX(va)];
}

bool vmm_translate(vaddr_t va, paddr_t *out)
{
    u32 entry = vmm_pte(va);

    if (!(entry & PTE_PRESENT))
        return false;

    if (out)
        *out = (entry & PTE_ADDR_MASK) | (va & PAGE_MASK);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Address spaces                                                            */
/* ------------------------------------------------------------------------- */

paddr_t vmm_create_address_space(void)
{
    paddr_t pd = pmm_alloc_frame();

    if (pd == PMM_NO_FRAME) {
        pr_err("cannot allocate a page directory");
        return 0;
    }

    bool irqs = irq_save();
    u32 *dst = temp_map(0, pd);
    const u32 *kernel = pd_entries();

    memset(dst, 0, PAGE_SIZE);

    /* Share the kernel's half. Every address space must see identical kernel
     * mappings, or an interrupt delivered while this process is running would
     * fault on the kernel's own code. */
    for (u32 i = KERNEL_PDE_FIRST; i < RECURSIVE_SLOT; i++)
        dst[i] = kernel[i];

    /* The recursive entry must point at *this* directory, not the kernel's -
     * copying it would make the new address space's window show someone
     * else's page tables. */
    dst[RECURSIVE_SLOT] = pd | PTE_PRESENT | PTE_WRITE;

    temp_unmap(0);
    irq_restore(irqs);

    stat_address_spaces++;
    return pd;
}

paddr_t vmm_clone_current(void)
{
    paddr_t child_pd = vmm_create_address_space();

    if (!child_pd)
        return 0;

    bool irqs = irq_save();
    u32 *parent_pd = pd_entries();
    u32 *child_pd_map = temp_map(0, child_pd);

    for (u32 pdi = 0; pdi < KERNEL_PDE_FIRST; pdi++) {
        if (!(parent_pd[pdi] & PTE_PRESENT))
            continue;

        paddr_t child_pt_frame = pmm_alloc_frame();
        if (child_pt_frame == PMM_NO_FRAME) {
            pr_err("out of memory cloning an address space");
            temp_unmap(0);
            irq_restore(irqs);
            vmm_destroy_address_space(child_pd);
            return 0;
        }

        u32 *parent_pt = pt_entries(pdi);
        u32 *child_pt = temp_map(1, child_pt_frame);

        for (u32 pti = 0; pti < 1024; pti++) {
            u32 entry = parent_pt[pti];

            if (!(entry & PTE_PRESENT)) {
                child_pt[pti] = 0;
                continue;
            }

            /* A writable page becomes read-only and COW-marked on *both*
             * sides. Marking only the child would let the parent write
             * through to a page the child believes is private. */
            if (entry & PTE_WRITE) {
                entry = (entry & ~(u32)PTE_WRITE) | PTE_COW;
                parent_pt[pti] = entry;
                invlpg((pdi << 22) | (pti << PAGE_SHIFT));
            }

            /* Both address spaces now hold the frame. */
            pmm_frame_ref(entry & PTE_ADDR_MASK);
            child_pt[pti] = entry;
            stat_mapped_pages++;
        }

        temp_unmap(1);

        child_pd_map[pdi] = child_pt_frame | PTE_PRESENT | PTE_WRITE | PTE_USER;
        stat_page_tables++;
    }

    temp_unmap(0);
    irq_restore(irqs);

    return child_pd;
}

void vmm_destroy_address_space(paddr_t pd_phys)
{
    if (!pd_phys)
        return;

    if (pd_phys == read_cr3())
        panic("vmm_destroy_address_space(%p): that is the address space we "
              "are running in",
              (void *)pd_phys);

    bool irqs = irq_save();
    u32 *pd = temp_map(0, pd_phys);

    for (u32 pdi = 0; pdi < KERNEL_PDE_FIRST; pdi++) {
        if (!(pd[pdi] & PTE_PRESENT))
            continue;

        paddr_t pt_frame = pd[pdi] & PTE_ADDR_MASK;
        u32 *pt = temp_map(1, pt_frame);

        for (u32 pti = 0; pti < 1024; pti++) {
            if (!(pt[pti] & PTE_PRESENT))
                continue;
            if (pt[pti] & PTE_OWNED) {
                /* Drops a reference; the frame only goes back to the
                 * allocator if no other address space holds it. */
                pmm_free_frame(pt[pti] & PTE_ADDR_MASK);
                stat_mapped_pages--;
            }
        }

        temp_unmap(1);
        pmm_free_frame(pt_frame);
        stat_page_tables--;
        pd[pdi] = 0;
    }

    temp_unmap(0);
    irq_restore(irqs);

    pmm_free_frame(pd_phys);
}

void vmm_clear_user_space(void)
{
    bool irqs = irq_save();
    u32 *pd = pd_entries();

    for (u32 pdi = 0; pdi < KERNEL_PDE_FIRST; pdi++) {
        if (!(pd[pdi] & PTE_PRESENT))
            continue;

        u32 *pt = pt_entries(pdi);

        for (u32 pti = 0; pti < 1024; pti++) {
            if (!(pt[pti] & PTE_PRESENT))
                continue;
            if (pt[pti] & PTE_OWNED) {
                pmm_free_frame(pt[pti] & PTE_ADDR_MASK);
                stat_mapped_pages--;
            }
            pt[pti] = 0;
        }

        paddr_t pt_frame = pd[pdi] & PTE_ADDR_MASK;
        pd[pdi] = 0;
        pmm_free_frame(pt_frame);
        stat_page_tables--;
    }

    /* The whole user half changed, so a per-page invlpg would be 786432
     * invalidations. Reloading CR3 flushes everything at once. */
    write_cr3(read_cr3());
    irq_restore(irqs);
}

/* Drop PTE_WRITE from the kernel's own code and constants.
 *
 * CR0.WP is already set, so a ring-0 write to a read-only page faults rather
 * than being quietly allowed - that is what makes this worth doing at all.
 * Without it, a wild kernel pointer can rewrite an instruction, and the
 * consequence shows up later as a machine executing something nobody wrote.
 *
 * .text and .rodata only. .data and .bss are writable by definition, and
 * .boot is left alone because it holds the page directory _start built, which
 * the VMM edits - through the recursive window rather than through here, but
 * there is no reason to narrow a mapping the kernel has a use for.
 *
 * Both ends are rounded outward to a page, which is safe because the linker
 * aligns the following section to 4 KiB: the rounding can only cover padding.
 */
void vmm_protect_kernel_text(void)
{
    struct {
        const char *what;
        vaddr_t start, end;
    } ranges[] = {
        {"text", (vaddr_t)__text_start, (vaddr_t)__text_end},
        {"rodata", (vaddr_t)__rodata_start, (vaddr_t)__rodata_end},
    };

    u32 pages = 0;

    for (size_t i = 0; i < ARRAY_SIZE(ranges); i++) {
        vaddr_t start = PAGE_TRUNC(ranges[i].start);
        vaddr_t end = PAGE_ALIGN(ranges[i].end);

        for (vaddr_t va = start; va < end; va += PAGE_SIZE) {
            if (!vmm_protect(va, PTE_PRESENT)) {
                pr_err("kernel .%s is not mapped at %p", ranges[i].what,
                       (void *)va);
                return;
            }
            pages++;
        }
    }

    harden_note_text_ro();
    pr_info("kernel .text and .rodata mapped read-only (%u pages); CR0.WP "
            "makes that binding on ring 0 too",
            pages);
}

void vmm_reserve_kernel_tables(vaddr_t base, size_t bytes)
{
    ASSERT(base >= KERNEL_VIRT_BASE);

    u32 first = PDE_INDEX(base);
    u32 last = PDE_INDEX(base + bytes - 1);

    for (u32 pdi = first; pdi <= last; pdi++)
        if (!ensure_table(pdi, PTE_PRESENT | PTE_WRITE))
            panic("cannot reserve the kernel page table for %p",
                  (void *)(pdi << 22));

    pr_debug("reserved directory slots %u-%u for %p+%u KiB", first, last,
             (void *)base, (unsigned)(bytes / KIB));
}

void vmm_switch_address_space(paddr_t pd_phys)
{
    if (pd_phys && pd_phys != read_cr3())
        write_cr3(pd_phys);
}

u32 vmm_count_user_pages(void)
{
    u32 count = 0;
    bool irqs = irq_save();
    u32 *pd = pd_entries();

    for (u32 pdi = 0; pdi < KERNEL_PDE_FIRST; pdi++) {
        if (!(pd[pdi] & PTE_PRESENT))
            continue;
        u32 *pt = pt_entries(pdi);
        for (u32 pti = 0; pti < 1024; pti++)
            if (pt[pti] & PTE_PRESENT)
                count++;
    }

    irq_restore(irqs);
    return count;
}

/* ------------------------------------------------------------------------- */

/* Naming the region a fault landed in is most of the diagnosis: the same
 * "page fault at 0x..." means very different things in the heap window and at
 * address zero. */
static const char *fault_region(u32 addr)
{
    if (addr < PAGE_SIZE)
        return "the null page - almost certainly a NULL dereference";
    if (addr < USER_IMAGE_BASE)
        return "low user space (unmapped by design)";
    if (addr < KERNEL_VIRT_BASE)
        return "user space";
    /* Most specific first: the kernel image lives *inside* the linear map,
     * so testing the window first would report every fault in the kernel's
     * own text as a stray physical access. */
    if (addr >= PAGE_TRUNC((u32)__text_start) &&
        addr < PAGE_ALIGN((u32)__rodata_end))
        return "the kernel's own code or constants, which are read-only";
    if (addr >= (u32)__kernel_start && addr < (u32)__kernel_end)
        return "the kernel image";
    if (addr >= KERNEL_VIRT_BASE && addr < KERNEL_VIRT_BASE + VMM_LINEAR_SIZE)
        return "the kernel's linear map of low physical memory";
    if (addr >= KHEAP_BASE && addr < KHEAP_BASE + KHEAP_MAX_SIZE)
        return "the kernel heap window - a bad heap pointer";
    if (addr >= VMM_TEMP_BASE &&
        addr < VMM_TEMP_BASE + VMM_TEMP_SLOTS * PAGE_SIZE)
        return "a temporary frame-mapping slot that is not currently mapped";
    if (addr >= KSTACK_BASE && addr < KSTACK_BASE + KSTACK_REGION)
        return "a kernel-stack guard page - a task overran its stack";
    if (addr >= 0xFFC00000u)
        return "the recursive page-table window";
    return "no region the kernel maps - a wild pointer";
}

/* Try to resolve a fault as a copy-on-write break. Returns true if the fault
 * was handled and execution can resume at the faulting instruction.
 *
 * This is the one kind of page fault that is not a bug: it is how fork() gets
 * away with sharing every page of the parent's memory until somebody writes
 * to one. */
static bool cow_fault(struct regs *r, u32 addr)
{
    /* Must be a write to a page that is present. A read of a COW page is
     * legitimate and never faults; a write to an absent page is a real
     * fault. */
    if (!(r->err_code & 0x02) || !(r->err_code & 0x01))
        return false;

    if (is_kernel_address(addr))
        return false; /* the kernel's own pages are never COW */

    u32 pte = vmm_pte(addr);

    if (!(pte & PTE_PRESENT) || !(pte & PTE_COW))
        return false;

    paddr_t old_frame = pte & PTE_ADDR_MASK;
    vaddr_t page = PAGE_TRUNC(addr);

    stat_cow_faults++;

    /* Sole owner: the other side already broke its copy, so there is nothing
     * to duplicate. Just restore write access. This is the common case in a
     * fork-then-exit pattern and it costs one PTE write. */
    if (pmm_frame_refs(old_frame) <= 1) {
        set_pte(page, (pte & ~(u32)PTE_COW) | PTE_WRITE);
        return true;
    }

    paddr_t fresh = pmm_alloc_frame();
    if (fresh == PMM_NO_FRAME) {
        pr_err("out of memory breaking a copy-on-write page at %p",
               (void *)page);
        return false; /* fall through to the panic: we cannot continue */
    }

    /* Copy through a temporary mapping. The source is readable through its
     * own (read-only) mapping, which is why no second temp slot is needed. */
    bool irqs = irq_save();
    void *dst = temp_map(0, fresh);

    /* The source is the user page being broken, read from ring 0 - so SMAP
     * applies, even though the destination is a kernel temporary slot. */
    user_access_begin();
    memcpy(dst, (const void *)page, PAGE_SIZE);
    user_access_end();

    temp_unmap(0);
    irq_restore(irqs);

    /* The new frame is private: writable, owned, no longer COW. */
    set_pte(page, fresh | (pte & 0xFFF & ~(u32)PTE_COW) | PTE_WRITE |
                      PTE_PRESENT | PTE_OWNED);

    /* Release our share of the original. */
    pmm_free_frame(old_frame);

    stat_cow_copies++;
    return true;
}

static void page_fault_handler(struct regs *r)
{
    u32 addr = read_cr2();

    stat_page_faults++;

    /* A copy-on-write break is the one fault that is expected. */
    if (cow_fault(r, addr))
        return;

    /* Everything else is a real bug, so the job is to report it as precisely
     * as possible. Demand paging is the other natural resolvable case - see
     * docs/ROADMAP.md. */
    const char *where = fault_region(addr);

    kprintf("\n");
    page_fault_describe(r);
    kprintf("  region          : %s\n", where);

    panic_with_regs(r, "page fault at %p in %s", (void *)addr, where);
}

void vmm_init(void)
{
    ASSERT(!initialised);

    /* Adopt the directory _start built. Its symbol value is already a
     * physical address: .boot is linked at its load address, which is the
     * whole reason that section exists. */
    kernel_pd_phys = (paddr_t)boot_page_directory;

    /* _start mapped the first 4 MiB and set up the recursive entry; sanity
     * check that we really are running on that directory before trusting it. */
    if (read_cr3() != kernel_pd_phys)
        panic("CR3 is %p but the boot page directory is at %p",
              (void *)read_cr3(), (void *)kernel_pd_phys);

    if (!(pd_entries()[RECURSIVE_SLOT] & PTE_PRESENT))
        panic("the recursive page-directory entry is missing; page tables "
              "would be unreachable");

    stat_page_tables = 1; /* the one _start created */
    stat_mapped_pages = VMM_BOOT_MAPPED / PAGE_SIZE;

    isr_install_handler(14, page_fault_handler);

    /* Widen the linear map beyond what _start could reach with its single
     * page table. Everything the kernel addresses physically must be inside
     * this window. */
    if (!vmm_map_range(KERNEL_VIRT_BASE + VMM_BOOT_MAPPED, VMM_BOOT_MAPPED,
                       VMM_LINEAR_SIZE - VMM_BOOT_MAPPED,
                       PTE_PRESENT | PTE_WRITE))
        panic("cannot extend the kernel's linear map to %u MiB",
              (unsigned)(VMM_LINEAR_SIZE / MIB));

    /* Claim a page table for every slot in the kernel half, now, while there
     * is exactly one address space to put them in.
     *
     * An address space copies the kernel's directory entries once, when it is
     * created. A kernel page table created *after* that exists only in
     * whichever address space happened to be current, so the mapping works
     * for one process and faults for every other - a bug that waits for the
     * wrong process to be scheduled before it appears. The heap was the live
     * example: growing past 4 MiB creates a new directory entry, and a
     * process forked before the growth would never have seen it.
     *
     * Reserving only the regions the kernel currently uses would work and
     * would be cheaper, but it leaves a list to keep in step with the layout,
     * and the failure mode for forgetting an entry is the bug above. Claiming
     * all 255 slots costs 1020 KiB of page tables once and removes the
     * category. ensure_table() then panics if anything tries to create a
     * kernel table later, which can now only mean a bug in this function. */
    vmm_reserve_kernel_tables(
        KERNEL_VIRT_BASE, (size_t)(RECURSIVE_SLOT - KERNEL_PDE_FIRST) << 22);

    /* Drop the identity mapping of the first 4 MiB. It existed only to keep
     * the handful of instructions between `mov cr0` and the higher-half jump
     * fetchable. Removing it is what makes the bottom of the address space
     * available to user processes - and what makes a NULL dereference fault
     * rather than landing on the interrupt vector table.
     *
     * Nothing may hold a low pointer at this point. Low physical memory is
     * still reachable, through the linear map at KERNEL_VIRT_BASE. */
    pd_entries()[0] = 0;

    /* The directory changed, so the whole TLB has to go - invlpg would only
     * cover one page, and this invalidated 1024 of them. */
    write_cr3(kernel_pd_phys);

    initialised = true;

    pr_info("paging: kernel at %p, linear map %u MiB, identity map dropped",
            (void *)KERNEL_VIRT_BASE, (unsigned)(VMM_LINEAR_SIZE / MIB));
    pr_info("kernel half fully backed: %u page tables (%u KiB), so every "
            "address space sees identical kernel mappings",
            stat_page_tables, stat_page_tables * 4);
    pr_debug("page directory at phys %p, %u page tables",
             (void *)kernel_pd_phys, stat_page_tables);
}

bool vmm_is_enabled(void)
{
    /* Paging is on from _start onwards; this reports whether the VMM has
     * taken ownership of the directory. */
    return initialised;
}

void vmm_get_stats(struct vmm_stats *out)
{
    if (!out)
        return;

    out->page_tables = stat_page_tables;
    out->mapped_pages = stat_mapped_pages;
    out->page_faults = stat_page_faults;
    out->cow_faults = stat_cow_faults;
    out->cow_copies = stat_cow_copies;
    out->address_spaces = stat_address_spaces;
}
