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
    if (addr >= KERNEL_VIRT_BASE && addr < KERNEL_VIRT_BASE + VMM_LINEAR_SIZE)
        return "the kernel's linear map of low physical memory";
    if (addr >= (u32)__kernel_start && addr < (u32)__kernel_end)
        return "the kernel image";
    if (addr >= KHEAP_BASE && addr < KHEAP_BASE + KHEAP_MAX_SIZE)
        return "the kernel heap window - a bad heap pointer, or a task stack "
               "overflow";
    if (addr >= 0xFFC00000u)
        return "the recursive page-table window";
    return "no region the kernel maps - a wild pointer";
}

static void page_fault_handler(struct regs *r)
{
    u32 addr = read_cr2();
    const char *where = fault_region(addr);

    stat_page_faults++;

    /* Nothing grows a mapping on demand yet, so every fault is a real bug and
     * the job is to report it as precisely as possible. Demand paging and
     * copy-on-write are the natural extensions - see docs/ROADMAP.md. */
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
}
