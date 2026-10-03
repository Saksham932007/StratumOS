/* StratumOS - Sv39 paging.
 *
 * Three levels, 512 entries each, 4 KiB pages, and a 39-bit virtual address
 * split 9 + 9 + 9 + 12. The x86 kernel's two-level 32-bit paging is in
 * kernel/mm/vmm.c and none of it ports: the entry format is different, the
 * level count is different, and - the part that actually forced a redesign -
 * the recursive self-mapping trick that lets vmm.c reach its own page tables
 * through a fixed virtual address does not generalise past two levels.
 *
 * WHAT REPLACED THE RECURSIVE MAPPING
 * -----------------------------------
 * On x86 the kernel points the last page-directory entry at the directory
 * itself, which makes every page table appear at a computable virtual
 * address. With three levels there is no single window that reaches all of
 * them, so this port does the other thing: it identity-maps physical memory
 * and walks the tables through those addresses. That is simpler, costs an
 * identity map the kernel must keep, and is what Linux does on RISC-V too.
 *
 * It is the clearest single answer to "how much of the memory manager
 * ports": the policy ports, the mechanism does not.
 */
#define LOG_TAG "paging"

#include <arch/riscv64/riscv.h>

#include <kernel/log.h>
#include <kernel/string.h>

/* Statically allocated rather than taken from a frame allocator, because at
 * the point paging is switched on there is no allocator yet - the same
 * bootstrap problem the x86 kernel solves with a page directory in .bss.
 * Three tables is enough for the mappings below plus a few leaf pages. */
#define MAX_TABLES 8

static u64 tables[MAX_TABLES][SV39_ENTRIES] ALIGNED(4096);
static u32 tables_used;
static u64 *root;
static bool enabled;

static u64 *alloc_table(void)
{
    if (tables_used >= MAX_TABLES) {
        pr_err("out of static page tables (%u)", MAX_TABLES);
        return NULL;
    }

    u64 *t = tables[tables_used++];

    memset(t, 0, SV39_PAGE_SIZE);
    return t;
}

/* Walk to the leaf level, creating intermediate tables. Returns a pointer to
 * the level-0 entry for `va`, or NULL.
 *
 * The check that matters: a non-leaf entry has V set and R, W and X all
 * clear. An entry with any of R/W/X *is* a leaf - that is how a gigapage or
 * megapage is expressed - so walking into one would be reading a physical
 * page as if it were a table. x86 distinguishes the two with a dedicated PS
 * bit; RISC-V infers it from the permissions, which is more economical and
 * much easier to get wrong. */
static u64 *leaf_entry(u64 va, bool create)
{
    u64 *table = root;

    for (int level = SV39_LEVELS - 1; level > 0; level--) {
        u64 *pte = &table[SV39_VPN(va, level)];

        if (!(*pte & PTE_V)) {
            if (!create)
                return NULL;

            u64 *next = alloc_table();

            if (!next)
                return NULL;

            /* A pointer to the next level: valid, and no R/W/X. */
            *pte = pte_from_phys((u64)(uptr)next, PTE_V);
            table = next;
            continue;
        }

        if (*pte & (PTE_R | PTE_W | PTE_X)) {
            /* A large page already covers this address. Splitting it is a
             * real operation a mature VMM needs and this one does not have,
             * so it refuses rather than corrupting the mapping. */
            if (create)
                pr_err("%p is already inside a large page", (void *)(uptr)va);
            return NULL;
        }

        table = (u64 *)(uptr)pte_to_phys(*pte);
    }

    return &table[SV39_VPN(va, 0)];
}

bool paging_map_page(u64 va, u64 pa, u64 flags)
{
    if (!sv39_canonical(va)) {
        /* Bits 63:39 must replicate bit 38, so the address space has a hole
         * in the middle. 32-bit x86 has no such rule and the kernel being
         * ported from had nowhere to express it - this check is new code,
         * not ported code. */
        pr_err("%p is not a canonical Sv39 address", (void *)(uptr)va);
        return false;
    }

    if (!IS_ALIGNED(va, SV39_PAGE_SIZE) || !IS_ALIGNED(pa, SV39_PAGE_SIZE))
        return false;

    u64 *pte = leaf_entry(va, true);

    if (!pte)
        return false;

    /* A and D set up front. RISC-V allows an implementation either to set
     * them in hardware on first access or to raise a page fault so software
     * can; setting them here works on both, where leaving them clear works
     * only on the first kind. QEMU is the first kind, which is exactly how
     * this would have shipped broken. */
    *pte = pte_from_phys(pa, flags | PTE_V | PTE_A | PTE_D);
    sfence_vma();
    return true;
}

bool paging_translate(u64 va, u64 *pa_out)
{
    u64 *table = root;

    if (!root)
        return false;

    for (int level = SV39_LEVELS - 1; level >= 0; level--) {
        u64 *pte = &table[SV39_VPN(va, level)];

        if (!(*pte & PTE_V))
            return false;

        if (*pte & (PTE_R | PTE_W | PTE_X)) {
            /* A leaf. At level 0 it is a 4 KiB page; above that it is a
             * large page, and the offset within it is wider. */
            u64 page_size = 1ul << (12 + 9 * level);
            u64 base = pte_to_phys(*pte);

            if (pa_out)
                *pa_out = base + (va & (page_size - 1));
            return true;
        }

        table = (u64 *)(uptr)pte_to_phys(*pte);
    }

    return false;
}

bool paging_init(void)
{
    root = alloc_table();
    if (!root)
        return false;

    /* Two gigapages, which is the whole of the initial address space:
     *
     *   VPN[2] = 0  ->  0x00000000-0x3FFFFFFF   the UART and the CLINT
     *   VPN[2] = 2  ->  0x80000000-0xBFFFFFFF   RAM, including this kernel
     *
     * Identity mappings, because the code that writes satp has to still be
     * fetchable on the instruction after it - the same constraint as the
     * x86 higher-half transition, solved the same way. The difference is
     * that one gigapage entry covers it, where x86 needed a page directory
     * with the kernel mapped twice.
     *
     * A large page at the root is a leaf with R/W/X set, so these two lines
     * are also the thing leaf_entry() has to refuse to walk into. */
    root[SV39_VPN(0x00000000ul, 2)] =
        pte_from_phys(0x00000000ul, PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);
    root[SV39_VPN(VIRT_RAM_BASE, 2)] = pte_from_phys(
        VIRT_RAM_BASE, PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);

    csr_write(satp, satp_from_root((u64)(uptr)root));
    sfence_vma();

    /* Reading satp back is the only confirmation available: there is no
     * equivalent of x86's CR0.PG bit to inspect, because translation being
     * active is implied by satp's mode field and the current privilege. A
     * zero here would mean the write was ignored, which is what happens if
     * this runs in machine mode. */
    enabled = (csr_read(satp) & SATP_MODE_SV39) == SATP_MODE_SV39;

    if (!enabled) {
        pr_err("satp did not take: Sv39 unsupported, or still in M-mode");
        return false;
    }

    pr_info("Sv39 enabled: root at %p, 2 gigapages identity-mapped, "
            "%u of %u static tables used",
            (void *)(uptr)root, tables_used, MAX_TABLES);
    return true;
}

bool paging_enabled(void)
{
    return enabled;
}

u64 paging_root(void)
{
    return (u64)(uptr)root;
}
