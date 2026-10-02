/* StratumOS - virtual memory manager (two-level x86 paging).
 *
 * The interesting part of this file is how it edits page tables *after* paging
 * is enabled. Once CR0.PG is set, a page table can only be written through a
 * virtual address, and the frames holding page tables come from the physical
 * allocator - which may hand back a frame nowhere near the identity-mapped
 * window. Identity-mapping all of RAM to work around that does not scale.
 *
 * The solution is the recursive mapping: the last page directory entry points
 * at the page directory itself. The hardware then resolves
 *
 *     0xFFFFF000            -> the page directory
 *     0xFFC00000 + i*4096   -> the page table for directory entry i
 *
 * because the walk uses the directory as both levels. Every page table in the
 * system becomes addressable at a computable virtual address, at the cost of
 * one 4 MiB slot at the very top of the address space.
 *
 * See docs/MEMORY.md for the full address-space map and for why the kernel is
 * identity-mapped rather than relocated to the higher half.
 */
#define LOG_TAG "vmm"

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

#define CR0_PG         0x80000000u
#define CR0_WP         0x00010000u

static paddr_t pd_phys;
static bool paging_on;
static u32 stat_page_tables;
static u32 stat_mapped_pages;
static u32 stat_page_faults;

/* Before paging is enabled, physical == virtual and we address the tables
 * directly. Afterwards, everything goes through the recursive window. */
static u32 *pd_entries(void)
{
    return paging_on ? (u32 *)PD_VADDR : (u32 *)pd_phys;
}

static u32 *pt_entries(u32 pdi)
{
    if (paging_on)
        return (u32 *)PT_VADDR(pdi);
    return (u32 *)(pd_entries()[pdi] & PTE_ADDR_MASK);
}

/* Create the page table for a directory slot if it is missing. */
static bool ensure_table(u32 pdi, u32 flags)
{
    u32 *pd = pd_entries();

    if (pd[pdi] & PTE_PRESENT) {
        /* A directory entry's USER bit gates the whole 4 MiB range, so it has
         * to be widened if any page inside it becomes user-accessible. */
        if (flags & PTE_USER)
            pd[pdi] |= PTE_USER;
        return true;
    }

    paddr_t frame = pmm_alloc_frame();
    if (frame == PMM_NO_FRAME) {
        pr_err("cannot allocate a page table for directory slot %u", pdi);
        return false;
    }

    pd[pdi] = frame | PTE_PRESENT | PTE_WRITE | (flags & PTE_USER);
    stat_page_tables++;

    /* The new table is reachable through the recursive window as soon as the
     * directory entry is live, but the TLB may still hold the old (absent)
     * translation for that window address. */
    if (paging_on)
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
              "window",
              (void *)va);

    if (!ensure_table(pdi, flags))
        return false;

    u32 *pt = pt_entries(pdi);

    if (pt[pti] & PTE_PRESENT) {
        /* Silently replacing a live mapping hides bugs; say so. The mapping
         * is still replaced, because remapping is legitimate. */
        pr_warn("remapping %p (was frame %p, now %p)", (void *)va,
                (void *)(pt[pti] & PTE_ADDR_MASK), (void *)pa);
        if (pt[pti] & PTE_OWNED)
            pmm_free_frame(pt[pti] & PTE_ADDR_MASK);
        stat_mapped_pages--;
    }

    pt[pti] = (pa & PTE_ADDR_MASK) | (flags & 0xFFF) | PTE_PRESENT;
    stat_mapped_pages++;

    if (paging_on)
        invlpg(va);

    return true;
}

bool vmm_alloc_at(vaddr_t va, u32 flags)
{
    paddr_t frame = pmm_alloc_frame();

    if (frame == PMM_NO_FRAME)
        return false;

    /* PTE_OWNED records that this mapping allocated its own frame, so
     * vmm_unmap() knows it is responsible for returning it. Without this,
     * unmapping either leaks every frame or frees frames it does not own -
     * for instance the VGA framebuffer. */
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

    /* A directory entry's USER bit gates its whole 4 MiB range, so widening a
     * single page to user access means widening the directory entry too. */
    if (flags & PTE_USER)
        pd[pdi] |= PTE_USER;

    /* Keep the frame and the ownership bit; replace only the permissions. */
    u32 keep = pt[pti] & (PTE_ADDR_MASK | PTE_OWNED);
    pt[pti] = keep | (flags & 0xFFF) | PTE_PRESENT;

    if (paging_on)
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

    if (paging_on)
        invlpg(va);
}

bool vmm_map_range(vaddr_t va, paddr_t pa, size_t bytes, u32 flags)
{
    size_t pages = PAGE_ALIGN(bytes) / PAGE_SIZE;

    for (size_t i = 0; i < pages; i++) {
        if (!vmm_map(va + i * PAGE_SIZE, pa + i * PAGE_SIZE, flags)) {
            /* Roll back so a partial failure does not leave a half-mapped
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

static void page_fault_handler(struct regs *r)
{
    u32 addr = read_cr2();

    stat_page_faults++;

    /* Nothing here grows a mapping on demand yet - every fault is a real bug,
     * so the job is to report it as precisely as possible. Demand paging and
     * copy-on-write are the natural extensions and are in docs/ROADMAP.md. */
    kprintf("\n");
    page_fault_describe(r);

    /* Naming the region a fault landed in is most of the diagnosis: the same
     * "page fault at 0x..." means very different things in the heap window
     * and at address zero. */
    const char *where;

    if (addr < PAGE_SIZE)
        where = "the null page - almost certainly a NULL dereference";
    else if (addr < 1 * MIB)
        where = "low memory (BIOS, IVT, VGA)";
    else if (addr >= (u32)__kernel_start && addr < (u32)__kernel_end)
        where = "the kernel image";
    else if (addr < VMM_IDENTITY_SIZE)
        where = "the identity-mapped window";
    else if (addr >= KHEAP_BASE && addr < KHEAP_BASE + KHEAP_MAX_SIZE)
        where = "the kernel heap window - a bad heap pointer, or a task "
                "stack overflow";
    else if (addr >= 0xFFC00000u)
        where = "the recursive page-table window";
    else
        where = "no region the kernel maps - a wild pointer";

    kprintf("  region          : %s\n", where);

    panic_with_regs(r, "page fault at %p in %s", (void *)addr, where);
}

void vmm_init(void)
{
    ASSERT(!paging_on);

    pd_phys = pmm_alloc_frame();
    if (pd_phys == PMM_NO_FRAME)
        panic("cannot allocate the page directory");

    memset((void *)pd_phys, 0, PAGE_SIZE);

    /* Identity-map the low region. This covers the kernel image, the PMM
     * bitmap, the VGA framebuffer at 0xB8000 and the bootloader's structures,
     * so every physical address the kernel already holds keeps working after
     * CR0.PG is set - which is what makes enabling paging a non-event rather
     * than a cliff.
     *
     * The mapping deliberately starts at the *second* page. Leaving the first
     * one unmapped turns every NULL dereference into an immediate, precisely
     * located page fault instead of a silent write to the interrupt vector
     * table - which is the single cheapest bug-catching measure available to
     * a kernel. The first page holds only the real-mode IVT and the BIOS data
     * area, neither of which a protected-mode kernel has any use for. */
    if (!vmm_map_range(PAGE_SIZE, PAGE_SIZE, VMM_IDENTITY_SIZE - PAGE_SIZE,
                       PTE_PRESENT | PTE_WRITE))
        panic("cannot build the identity mapping");

    /* ...unless a loader put something we still need down there. No loader in
     * practice does, but failing loudly beats faulting mysteriously. */
    const struct boot_params *bp = kernel_boot_params();
    if (bp && bp->reserved_hi > bp->reserved_lo &&
        bp->reserved_lo < PAGE_SIZE) {
        pr_warn("the loader's info block overlaps the null page; mapping it "
                "and giving up NULL-dereference detection");
        if (!vmm_map(0, 0, PTE_PRESENT | PTE_WRITE))
            panic("cannot map the loader's info block");
    }

    /* The recursive entry. Must be installed before paging is enabled,
     * because afterwards there is no other way to reach the tables. */
    pd_entries()[RECURSIVE_SLOT] = pd_phys | PTE_PRESENT | PTE_WRITE;

    isr_install_handler(14, page_fault_handler);

    write_cr3(pd_phys);

    /* CR0.WP makes ring-0 writes respect the read-only bit too. Without it,
     * the kernel can scribble over pages it marked read-only and the
     * protection is decorative. */
    write_cr0(read_cr0() | CR0_PG | CR0_WP);

    paging_on = true;

    pr_info("paging enabled: %u MiB identity-mapped, directory at %p, "
            "%u page tables",
            VMM_IDENTITY_SIZE / MIB, (void *)pd_phys, stat_page_tables);
}

bool vmm_is_enabled(void)
{
    return paging_on;
}

void vmm_get_stats(struct vmm_stats *out)
{
    if (!out)
        return;

    out->page_tables = stat_page_tables;
    out->mapped_pages = stat_mapped_pages;
    out->page_faults = stat_page_faults;
}
