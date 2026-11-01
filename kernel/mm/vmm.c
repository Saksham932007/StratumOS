/* StratumOS - virtual memory manager (PAE, three-level x86 paging).
 *
 * Paging is already enabled before this file runs: kernel/arch/x86/boot.asm
 * sets CR4.PAE and builds a PDPT, four page directories and two page tables -
 * an identity map of the first 4 MiB, the same 4 MiB again at 0xC0000000, and
 * a self-map in each directory - then jumps into the higher half. The VMM
 * adopts those tables, enables the no-execute bit, widens the linear map, and
 * drops the identity mapping it no longer needs.
 *
 * The self-mapping window
 * -----------------------
 * Once CR0.PG is set, a page table can only be written through a virtual
 * address - but page tables come from the physical allocator, which can hand
 * back a frame anywhere in RAM, including far outside the linear map. Mapping
 * all of physical memory to work around that does not scale and is impossible
 * on a 4 GiB machine in a 32-bit address space.
 *
 * So entry 511 of every page directory points at that directory. The hardware
 * walk then uses the directory as both of the last two levels, and within
 * gigabyte k
 *
 *     (k << 30) + 0x3FFFF000            -> the page directory
 *     (k << 30) + 0x3FE00000 + i*4096   -> the page table for its entry i
 *
 * Every page table in the system becomes addressable at a computable address,
 * at the cost of the top 2 MiB of each gigabyte. mm/vmm.h explains why this
 * shape rather than the two-level trick of a self-referencing CR3, which PAE
 * does not allow: CR3 names a PDPT, and a PDPT entry cannot be walked as a
 * page table.
 *
 * See docs/MEMORY.md for the full address-space layout.
 */
#define LOG_TAG "vmm"

#include <arch/cpu.h>
#include <arch/harden.h>
#include <arch/idt.h>
#include <arch/io.h>

#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/sched.h>
#include <kernel/string.h>

#include <mm/pmm.h>
#include <mm/vmm.h>

/* Built by _start; their symbol values are physical addresses, because .boot
 * is linked at its load address. boot_page_directory is the kernel's own
 * gigabyte - PDPT slot 3 - and is shared by every address space. */
extern u8 boot_pdpt[];
extern u8 boot_page_directory[];

static paddr_t kernel_pdpt_phys;
static paddr_t kernel_pd_phys;
static bool initialised;
static bool nx_ok;
static u32 stat_page_dirs;
static u32 stat_page_tables;
static u32 stat_mapped_pages;
static u32 stat_page_faults;
static u32 stat_cow_faults;
static u32 stat_cow_copies;
static u32 stat_address_spaces;

/* Page tables are always reached through the self-mapping window. Before the
 * higher-half jump there was a physical path as well; there no longer is,
 * because C never runs with paging off.
 *
 * `volatile`, and it has to be. A page table entry is not ordinary memory:
 * writing one changes where every subsequent access to a virtual address
 * *goes*, and the compiler cannot see that. Given a store to 0xFFF3C000 and a
 * load from 0xCF000000 it sees two unrelated absolute addresses and reorders
 * them freely - which it did, hoisting a temp_unmap() above the code using
 * the mapping and producing a page fault on a mapping that had just been
 * established. Volatile keeps these accesses in order relative to each other;
 * barrier() in temp_map/temp_unmap keeps them in order relative to accesses
 * through the mapping. Both are needed, and neither is sufficient alone. */
static inline volatile pte_t *pd_entries(u32 pdpti)
{
    return (volatile pte_t *)PD_WINDOW(pdpti);
}

static inline volatile pte_t *pt_entries(u32 pdpti, u32 pdi)
{
    return (volatile pte_t *)(PT_WINDOW(pdpti) + ((u32)pdi << PAGE_SHIFT));
}

paddr_t vmm_kernel_root_phys(void)
{
    return kernel_pdpt_phys;
}

paddr_t vmm_kernel_pd_phys(void)
{
    return kernel_pd_phys;
}

bool vmm_nx_enabled(void)
{
    return nx_ok;
}

/* ------------------------------------------------------------------------- */
/* No-execute                                                                */
/* ------------------------------------------------------------------------- */
/*
 * Two things have to be true before bit 63 of an entry may be written: the
 * processor must report the feature, and EFER.NXE must be set on *this*
 * processor. The bit is per-core state in a model-specific register, so every
 * application processor has to do this for itself, and has to do it before it
 * loads a page table that already contains NX bits - otherwise bit 63 is a
 * reserved bit, every walk through that entry faults, and an application
 * processor that dies this way looks exactly like one that never started.
 */
#define MSR_EFER     0xC0000080u
#define EFER_NXE     (1u << 11)
#define CPUID_EXT_NX (1u << 20) /* leaf 0x80000001, EDX */

bool vmm_enable_nx(void)
{
    const struct cpu_info *cpu = cpu_get_info();
    u32 a, b, c, d;

    if (!cpu->has_cpuid || !cpu->has_msr || cpu->max_ext_leaf < 0x80000001u)
        return false;

    cpuid_raw(0x80000001u, 0, &a, &b, &c, &d);
    if (!(d & CPUID_EXT_NX))
        return false;

    u64 efer = rdmsr(MSR_EFER);

    if (!(efer & EFER_NXE))
        wrmsr(MSR_EFER, efer | EFER_NXE);

    /* Read it back rather than trusting the write. A hypervisor may decline
     * to set a bit it does not implement, and the consequence of believing it
     * did is a reserved-bit fault on the next mapping this kernel makes. */
    return (rdmsr(MSR_EFER) & EFER_NXE) != 0;
}

/* The low twelve bits of an entry, plus PRESENT, plus NX unless the caller
 * asked for an executable page.
 *
 * Expressed this way round on purpose: a caller that says nothing gets a
 * non-executable page, so the kernel is W^X by default and the exceptions are
 * the short list that mentions PTE_EXEC. The alternative - an opt-in NX flag -
 * makes every mapping anyone forgets to annotate into an executable one. */
static pte_t leaf_flags(u32 flags)
{
    pte_t e = (pte_t)(flags & 0xFFF) | PTE_PRESENT;

    if (nx_ok && !(flags & PTE_EXEC))
        e |= PTE_NX;

    return e;
}

/* ------------------------------------------------------------------------- */
/* Temporary mappings                                                        */
/* ------------------------------------------------------------------------- */
/*
 * The self-mapping window reaches only the *current* address space's tables,
 * but fork has to build the child's while the parent's is loaded. These two
 * slots are how a frame that is mapped nowhere becomes writable for a moment.
 *
 * Two slots, because cloning needs a directory and one of its page tables
 * visible at once. They are a fixed resource, so every use is a short,
 * non-nesting critical section - asserted rather than assumed.
 */
static bool temp_slot_busy[VMM_TEMP_SLOTS];

static void *temp_map(unsigned slot, paddr_t frame)
{
    ASSERT(slot < VMM_TEMP_SLOTS);
    ASSERT(!temp_slot_busy[slot]);

    vaddr_t va = VMM_TEMP_BASE + slot * PAGE_SIZE;
    volatile pte_t *pt = pt_entries(PDPT_INDEX(va), PDE_INDEX(va));

    temp_slot_busy[slot] = true;
    pt[PTE_INDEX(va)] = (frame & PTE_ADDR_MASK) | leaf_flags(PTE_WRITE);
    invlpg(va);

    /* Nothing the caller does through this mapping may be moved above the
     * store that created it. invlpg's clobber already says so; saying it
     * again means the guarantee does not depend on invlpg staying here. */
    barrier();

    return (void *)va;
}

static void temp_unmap(unsigned slot)
{
    ASSERT(slot < VMM_TEMP_SLOTS);

    /* The caller's accesses through this mapping must all have happened
     * before it is torn down. Without this the compiler will hoist the store
     * below above them - it has, and the result was a page fault on a page
     * the code had just mapped and was still using. */
    barrier();

    vaddr_t va = VMM_TEMP_BASE + slot * PAGE_SIZE;
    volatile pte_t *pt = pt_entries(PDPT_INDEX(va), PDE_INDEX(va));

    pt[PTE_INDEX(va)] = 0;
    invlpg(va);
    temp_slot_busy[slot] = false;
}

/* Replace a present page's entry outright, keeping nothing. Used by the
 * copy-on-write handler, which needs to change both the frame and the flags
 * in one step - vmm_protect() keeps the frame and vmm_map() would warn about
 * replacing a live mapping. */
static void set_pte(vaddr_t va, pte_t entry)
{
    u32 pdpti = PDPT_INDEX(va);
    u32 pdi = PDE_INDEX(va);

    ASSERT(pd_entries(pdpti)[pdi] & PTE_PRESENT);
    pt_entries(pdpti, pdi)[PTE_INDEX(va)] = entry;
    invlpg(va);
}

/* Create the page table for a directory slot if it is missing.
 *
 * There used to be a panic here for any kernel table created after
 * vmm_init(), and a function that pre-allocated all 255 of them to make sure
 * none ever was. The reason was that a two-level address space *copied* the
 * kernel's directory entries when it was created, so a kernel page table
 * created later existed only in whichever address space happened to be
 * current: the mapping worked for one process and faulted for every other,
 * and the bug waited for the wrong process to be scheduled before it
 * appeared. Growing the heap past a 4 MiB boundary was the live example.
 *
 * PAE deleted the problem rather than guarding against it. The kernel's
 * gigabyte is one PDPT entry, so every address space points at the *same*
 * kernel page directory, and a table created here is in all of them the
 * instant the entry is written. Two megabytes of pre-allocated page tables
 * and a panic went away with it.
 *
 * A directory entry never carries NX. The bit is OR-ed down the walk, so
 * setting it on a directory would make every page beneath it non-executable;
 * executability is decided one level down, per page.
 */
static bool ensure_table(u32 pdpti, u32 pdi, u32 flags)
{
    volatile pte_t *pd = pd_entries(pdpti);

    if (pd[pdi] & PTE_PRESENT) {
        /* A directory entry's USER bit gates its whole 2 MiB range, so it has
         * to be widened if any page inside it becomes user-accessible. */
        if (flags & PTE_USER)
            pd[pdi] |= PTE_USER;
        return true;
    }

    paddr_t frame = pmm_alloc_frame();

    if (frame == PMM_NO_FRAME) {
        pr_err("cannot allocate a page table for slot %u of gigabyte %u", pdi,
               pdpti);
        return false;
    }

    pd[pdi] = (pte_t)frame | PTE_PRESENT | PTE_WRITE | (flags & PTE_USER);
    stat_page_tables++;

    /* The new table becomes reachable through the window as soon as the
     * directory entry is live, but the TLB may still hold the old
     * (not-present) translation for that window address. */
    invlpg(PT_WINDOW(pdpti) + ((u32)pdi << PAGE_SHIFT));

    /* Zeroed an entry at a time through the volatile pointer rather than with
     * memset, which would need the qualifier cast away - and casting it away
     * is exactly the mistake this file just finished paying for. */
    volatile pte_t *fresh = pt_entries(pdpti, pdi);

    for (u32 i = 0; i < PT_ENTRIES; i++)
        fresh[i] = 0;
    return true;
}

bool vmm_map(vaddr_t va, paddr_t pa, u32 flags)
{
    u32 pdpti = PDPT_INDEX(va);
    u32 pdi = PDE_INDEX(va);
    u32 pti = PTE_INDEX(va);

    if (pdi == PD_SELF_SLOT)
        panic("vmm_map(%p): refusing to map over gigabyte %u's page-table "
              "window - every page table in it would become unreachable",
              (void *)va, pdpti);

    if (!ensure_table(pdpti, pdi, flags))
        return false;

    volatile pte_t *pt = pt_entries(pdpti, pdi);

    if (pt[pti] & PTE_PRESENT) {
        /* Silently replacing a live mapping hides bugs, so say so. The
         * mapping is still replaced, because remapping is legitimate. */
        pr_warn("remapping %p (was frame %p, now %p)", (void *)va,
                (void *)(u32)(pt[pti] & PTE_ADDR_MASK), (void *)pa);
        if (pt[pti] & PTE_OWNED)
            pmm_free_frame((paddr_t)(pt[pti] & PTE_ADDR_MASK));
        stat_mapped_pages--;
    }

    pt[pti] = (pa & PTE_ADDR_MASK) | leaf_flags(flags);
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
    u32 pdpti = PDPT_INDEX(va);
    u32 pdi = PDE_INDEX(va);
    u32 pti = PTE_INDEX(va);
    volatile pte_t *pd = pd_entries(pdpti);

    if (!(pd[pdi] & PTE_PRESENT))
        return false;

    volatile pte_t *pt = pt_entries(pdpti, pdi);

    if (!(pt[pti] & PTE_PRESENT))
        return false;

    if (flags & PTE_USER)
        pd[pdi] |= PTE_USER;

    /* Keep the frame and the ownership bit; replace only the permissions -
     * which includes NX, so a page made writable by this call stops being
     * executable unless the caller asked again. */
    pte_t keep = pt[pti] & ((pte_t)PTE_ADDR_MASK | PTE_OWNED);

    pt[pti] = keep | leaf_flags(flags);

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
    u32 pdpti = PDPT_INDEX(va);
    u32 pdi = PDE_INDEX(va);
    u32 pti = PTE_INDEX(va);
    volatile pte_t *pd = pd_entries(pdpti);

    if (!(pd[pdi] & PTE_PRESENT))
        return;

    volatile pte_t *pt = pt_entries(pdpti, pdi);
    pte_t entry = pt[pti];

    if (!(entry & PTE_PRESENT))
        return;

    if (entry & PTE_OWNED)
        pmm_free_frame((paddr_t)(entry & PTE_ADDR_MASK));

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

pte_t vmm_pde_raw(u32 pdpti, u32 pdi)
{
    if (pdpti >= PDPT_ENTRIES || pdi >= PD_ENTRIES)
        return 0;
    return pd_entries(pdpti)[pdi];
}

bool vmm_inspect_entry(paddr_t table_phys, u32 index, pte_t *out)
{
    if (!table_phys || index >= PAGE_SIZE / sizeof(pte_t) || !out)
        return false;

    bool irqs = irq_save();
    const volatile pte_t *table = temp_map(0, table_phys);

    *out = table[index];

    temp_unmap(0);
    irq_restore(irqs);

    return true;
}

pte_t vmm_pte(vaddr_t va)
{
    u32 pdpti = PDPT_INDEX(va);
    u32 pdi = PDE_INDEX(va);
    volatile pte_t *pd = pd_entries(pdpti);

    if (!(pd[pdi] & PTE_PRESENT))
        return 0;

    return pt_entries(pdpti, pdi)[PTE_INDEX(va)];
}

bool vmm_translate(vaddr_t va, paddr_t *out)
{
    pte_t entry = vmm_pte(va);

    if (!(entry & PTE_PRESENT))
        return false;

    if (out)
        *out = (paddr_t)(entry & PTE_ADDR_MASK) | (va & PAGE_MASK);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Address spaces                                                            */
/* ------------------------------------------------------------------------- */
/*
 * An address space is a PDPT plus three page directories, one per gigabyte of
 * user space. The fourth entry is the kernel's directory, shared by pointer.
 *
 * The three user directories are allocated eagerly rather than on demand, and
 * that is the one place this costs more than two-level paging did: four
 * frames per address space instead of one. It buys an invariant worth more
 * than the twelve kilobytes - every PDPT entry in every address space is
 * present, always - and the invariant is what lets the page-table window be
 * used without checking, and lets the PDPT be written exactly once, when the
 * address space is created. A PDPT entry changed while its CR3 is loaded is
 * not seen until CR3 is reloaded, because the processor caches the four
 * entries internally; never changing one removes that trap entirely.
 */

/* Fill in a freshly allocated page directory: empty, with its self-map.
 *
 * The self-map entry carries NX. It covers only the 2 MiB window it creates,
 * so this makes the window - which is writable, and through which every page
 * table in the gigabyte is reachable - non-executable, without affecting
 * anything mapped under the other 511 slots. A writable, executable 2 MiB
 * hole in the kernel's own address space would be a strange thing to leave
 * behind in a kernel that went to PAE for W^X.
 */
static void init_directory(void *dir, paddr_t dir_phys)
{
    pte_t *entries = dir;

    memset(dir, 0, PAGE_SIZE);
    entries[PD_SELF_SLOT] =
        (pte_t)dir_phys | PTE_PRESENT | PTE_WRITE | (nx_ok ? PTE_NX : 0);
}

/* A PDPT entry holds a present bit, the two cache-control bits and the
 * directory address. Bits 1, 2 and 5-8 are *reserved* and a non-zero one
 * faults, so - unlike every other level - there is no WRITE or USER bit to
 * set here, and permissions are entirely a matter for the two levels below. */
#define PDPTE(phys) ((pte_t)(phys) | PTE_PRESENT)

paddr_t vmm_create_address_space(void)
{
    paddr_t pdpt = pmm_alloc_frame();
    paddr_t dirs[USER_PDPT_COUNT];

    if (pdpt == PMM_NO_FRAME) {
        pr_err("cannot allocate a PDPT");
        return 0;
    }

    for (u32 k = 0; k < USER_PDPT_COUNT; k++) {
        dirs[k] = pmm_alloc_frame();
        if (dirs[k] == PMM_NO_FRAME) {
            pr_err("cannot allocate the page directory for gigabyte %u", k);
            while (k-- > 0)
                pmm_free_frame(dirs[k]);
            pmm_free_frame(pdpt);
            return 0;
        }
    }

    bool irqs = irq_save();

    for (u32 k = 0; k < USER_PDPT_COUNT; k++) {
        init_directory(temp_map(0, dirs[k]), dirs[k]);
        temp_unmap(0);
    }

    pte_t *top = temp_map(0, pdpt);

    memset(top, 0, PAGE_SIZE);
    for (u32 k = 0; k < USER_PDPT_COUNT; k++)
        top[k] = PDPTE(dirs[k]);

    /* The kernel's gigabyte, by pointer. Not a copy: a kernel mapping made
     * after this address space exists has to be visible in it, and the only
     * way to guarantee that without tracking every address space is for there
     * to be one kernel page directory. */
    top[KERNEL_PDPT_INDEX] = PDPTE(kernel_pd_phys);

    temp_unmap(0);
    irq_restore(irqs);

    stat_page_dirs += USER_PDPT_COUNT;
    stat_address_spaces++;
    return pdpt;
}

paddr_t vmm_create_bootstrap_pd(void)
{
    paddr_t pdpt = pmm_alloc_frame();
    paddr_t dirs[USER_PDPT_COUNT];
    paddr_t tables[VMM_BOOT_MAPPED / (2 * MIB)];
    u32 allocated = 0;
    bool irqs;

    if (pdpt == PMM_NO_FRAME) {
        pr_err("cannot allocate a bootstrap PDPT");
        return 0;
    }

    for (u32 k = 0; k < USER_PDPT_COUNT; k++) {
        dirs[k] = pmm_alloc_frame();
        if (dirs[k] == PMM_NO_FRAME)
            goto out_of_memory;
        allocated++;
    }

    for (u32 i = 0; i < ARRAY_SIZE(tables); i++) {
        tables[i] = pmm_alloc_frame();
        if (tables[i] == PMM_NO_FRAME)
            goto out_of_memory;
    }

    irqs = irq_save();

    /* The identity tables: the first 4 MiB, one entry per page, 512 pages to
     * a table. Executable, which is the entire point - the instruction after
     * `mov cr0` on an application processor is fetched through this mapping. */
    for (u32 i = 0; i < ARRAY_SIZE(tables); i++) {
        pte_t *entries = temp_map(0, tables[i]);

        for (u32 j = 0; j < PT_ENTRIES; j++)
            entries[j] = (pte_t)((i * PT_ENTRIES + j) << PAGE_SHIFT) |
                         PTE_PRESENT | PTE_WRITE;

        temp_unmap(0);
    }

    for (u32 k = 0; k < USER_PDPT_COUNT; k++) {
        pte_t *dir = temp_map(0, dirs[k]);

        init_directory(dir, dirs[k]);

        /* Only gigabyte 0 gets the identity map; the other two directories
         * exist to keep "every PDPT entry is present" true, which is what the
         * page-table window relies on. */
        if (k == 0)
            for (u32 i = 0; i < ARRAY_SIZE(tables); i++)
                dir[i] = (pte_t)tables[i] | PTE_PRESENT | PTE_WRITE;

        temp_unmap(0);
    }

    pte_t *top = temp_map(0, pdpt);

    memset(top, 0, PAGE_SIZE);
    for (u32 k = 0; k < USER_PDPT_COUNT; k++)
        top[k] = PDPTE(dirs[k]);
    top[KERNEL_PDPT_INDEX] = PDPTE(kernel_pd_phys);

    temp_unmap(0);
    irq_restore(irqs);

    pr_debug("bootstrap PDPT at phys %p (identity 0-%u MiB plus the kernel's "
             "page directory)",
             (void *)pdpt, (unsigned)(VMM_BOOT_MAPPED / MIB));

    return pdpt;

out_of_memory:
    pr_err("cannot allocate the bootstrap address space");
    for (u32 i = 0; i < allocated; i++)
        pmm_free_frame(dirs[i]);
    pmm_free_frame(pdpt);
    return 0;
}

void vmm_destroy_bootstrap_pd(paddr_t pdpt_phys)
{
    if (!pdpt_phys)
        return;

    if (pdpt_phys == read_cr3())
        panic("vmm_destroy_bootstrap_pd(%p): that is the address space we are "
              "running on",
              (void *)pdpt_phys);

    bool irqs = irq_save();
    pte_t *top = temp_map(0, pdpt_phys);
    paddr_t dirs[USER_PDPT_COUNT];

    for (u32 k = 0; k < USER_PDPT_COUNT; k++)
        dirs[k] = (paddr_t)(top[k] & PTE_ADDR_MASK);

    temp_unmap(0);

    /* Gigabyte 0's identity tables. Nothing else here allocated a page
     * table, and the kernel's directory is shared and must not be touched. */
    pte_t *dir = temp_map(0, dirs[0]);
    paddr_t tables[VMM_BOOT_MAPPED / (2 * MIB)];

    for (u32 i = 0; i < ARRAY_SIZE(tables); i++) {
        tables[i] = (paddr_t)(dir[i] & PTE_ADDR_MASK);
        dir[i] = 0;
    }

    temp_unmap(0);
    irq_restore(irqs);

    for (u32 i = 0; i < ARRAY_SIZE(tables); i++)
        if (tables[i])
            pmm_free_frame(tables[i]);
    for (u32 k = 0; k < USER_PDPT_COUNT; k++)
        if (dirs[k])
            pmm_free_frame(dirs[k]);
    pmm_free_frame(pdpt_phys);
}

paddr_t vmm_clone_current(void)
{
    paddr_t child_pdpt = vmm_create_address_space();

    if (!child_pdpt)
        return 0;

    bool irqs = irq_save();

    /* The child's directories, read once. The PDPT is not needed after this,
     * which keeps both temporary slots free for a directory and a table. */
    pte_t *top = temp_map(0, child_pdpt);
    paddr_t child_dirs[USER_PDPT_COUNT];

    for (u32 k = 0; k < USER_PDPT_COUNT; k++)
        child_dirs[k] = (paddr_t)(top[k] & PTE_ADDR_MASK);

    temp_unmap(0);

    for (u32 pdpti = 0; pdpti < USER_PDPT_COUNT; pdpti++) {
        volatile pte_t *parent_pd = pd_entries(pdpti);

        for (u32 pdi = 0; pdi < PD_SELF_SLOT; pdi++) {
            if (!(parent_pd[pdi] & PTE_PRESENT))
                continue;

            paddr_t child_pt_frame = pmm_alloc_frame();

            if (child_pt_frame == PMM_NO_FRAME) {
                pr_err("out of memory cloning an address space");
                irq_restore(irqs);
                vmm_destroy_address_space(child_pdpt);
                return 0;
            }

            volatile pte_t *parent_pt = pt_entries(pdpti, pdi);
            pte_t *child_pt = temp_map(0, child_pt_frame);

            for (u32 pti = 0; pti < PT_ENTRIES; pti++) {
                pte_t entry = parent_pt[pti];

                if (!(entry & PTE_PRESENT)) {
                    child_pt[pti] = 0;
                    continue;
                }

                /* A writable page becomes read-only and COW-marked on *both*
                 * sides. Marking only the child would let the parent write
                 * through to a page the child believes is private. */
                if (entry & PTE_WRITE) {
                    entry = (entry & ~(pte_t)PTE_WRITE) | PTE_COW;
                    parent_pt[pti] = entry;
                    invlpg(((u32)pdpti << 30) | ((u32)pdi << 21) |
                           (pti << PAGE_SHIFT));
                }

                /* Both address spaces now hold the frame. */
                pmm_frame_ref((paddr_t)(entry & PTE_ADDR_MASK));
                child_pt[pti] = entry;
                stat_mapped_pages++;
            }

            temp_unmap(0);

            pte_t *child_pd = temp_map(0, child_dirs[pdpti]);

            child_pd[pdi] =
                (pte_t)child_pt_frame | PTE_PRESENT | PTE_WRITE | PTE_USER;
            temp_unmap(0);
            stat_page_tables++;
        }
    }

    irq_restore(irqs);

    return child_pdpt;
}

void vmm_destroy_address_space(paddr_t pdpt_phys)
{
    if (!pdpt_phys)
        return;

    if (pdpt_phys == read_cr3())
        panic("vmm_destroy_address_space(%p): that is the address space we "
              "are running in",
              (void *)pdpt_phys);

    bool irqs = irq_save();
    pte_t *top = temp_map(0, pdpt_phys);
    paddr_t dirs[USER_PDPT_COUNT];

    for (u32 k = 0; k < USER_PDPT_COUNT; k++)
        dirs[k] = (paddr_t)(top[k] & PTE_ADDR_MASK);

    temp_unmap(0);

    for (u32 k = 0; k < USER_PDPT_COUNT; k++) {
        if (!dirs[k])
            continue;

        for (u32 pdi = 0; pdi < PD_SELF_SLOT; pdi++) {
            pte_t *dir = temp_map(0, dirs[k]);
            paddr_t pt_frame = (dir[pdi] & PTE_PRESENT)
                                   ? (paddr_t)(dir[pdi] & PTE_ADDR_MASK)
                                   : 0;

            dir[pdi] = 0;
            temp_unmap(0);

            if (!pt_frame)
                continue;

            pte_t *pt = temp_map(0, pt_frame);

            for (u32 pti = 0; pti < PT_ENTRIES; pti++) {
                if (!(pt[pti] & PTE_PRESENT))
                    continue;
                if (pt[pti] & PTE_OWNED) {
                    /* Drops a reference; the frame only goes back to the
                     * allocator if no other address space holds it. */
                    pmm_free_frame((paddr_t)(pt[pti] & PTE_ADDR_MASK));
                    stat_mapped_pages--;
                }
            }

            temp_unmap(0);
            pmm_free_frame(pt_frame);
            stat_page_tables--;
        }

        pmm_free_frame(dirs[k]);
        stat_page_dirs--;
    }

    irq_restore(irqs);

    pmm_free_frame(pdpt_phys);
}

void vmm_clear_user_space(void)
{
    bool irqs = irq_save();

    for (u32 pdpti = 0; pdpti < USER_PDPT_COUNT; pdpti++) {
        volatile pte_t *pd = pd_entries(pdpti);

        for (u32 pdi = 0; pdi < PD_SELF_SLOT; pdi++) {
            if (!(pd[pdi] & PTE_PRESENT))
                continue;

            volatile pte_t *pt = pt_entries(pdpti, pdi);

            for (u32 pti = 0; pti < PT_ENTRIES; pti++) {
                if (!(pt[pti] & PTE_PRESENT))
                    continue;
                if (pt[pti] & PTE_OWNED) {
                    pmm_free_frame((paddr_t)(pt[pti] & PTE_ADDR_MASK));
                    stat_mapped_pages--;
                }
                pt[pti] = 0;
            }

            paddr_t pt_frame = (paddr_t)(pd[pdi] & PTE_ADDR_MASK);

            pd[pdi] = 0;
            pmm_free_frame(pt_frame);
            stat_page_tables--;
        }
    }

    /* The whole user half changed, so a per-page invlpg would be 786432
     * invalidations. Reloading CR3 flushes everything at once. */
    write_cr3(read_cr3());
    irq_restore(irqs);
}

/* Make W^X true of the kernel's own linear map, rather than intended.
 *
 * Three passes over the window, in this order, because the third is a blanket
 * rule and the first two are the exceptions to it:
 *
 *   .text     read-only and executable
 *   .rodata   read-only and non-executable
 *   the rest  writable and non-executable
 *
 * _start had to leave the first 4 MiB executable - it is fetching
 * instructions through that mapping - so until this runs, the kernel's stack,
 * heap, page tables and every byte of low physical memory are executable as
 * well as writable. This is where that stops being the case.
 *
 * Read-only matters for the same reason and is older than the NX bit here:
 * CR0.WP is set, so a ring-0 write to a read-only page faults rather than
 * being quietly allowed. Without it a wild kernel pointer can rewrite an
 * instruction, and the consequence shows up later as a machine executing
 * something nobody wrote. With NX, the other half is covered too: a wild
 * *jump* into data now faults at the fetch instead of interpreting whatever
 * happened to be there.
 *
 * Both ends of each range are rounded outward to a page, which is safe
 * because the linker aligns the following section to 4 KiB: the rounding can
 * only cover padding.
 */
void vmm_protect_kernel_text(void)
{
    vaddr_t text_start = PAGE_TRUNC((vaddr_t)__text_start);
    vaddr_t text_end = PAGE_ALIGN((vaddr_t)__text_end);
    vaddr_t rodata_start = PAGE_TRUNC((vaddr_t)__rodata_start);
    vaddr_t rodata_end = PAGE_ALIGN((vaddr_t)__rodata_end);
    u32 ro = 0, nx = 0;

    for (vaddr_t va = text_start; va < text_end; va += PAGE_SIZE) {
        if (!vmm_protect(va, PTE_PRESENT | PTE_EXEC)) {
            pr_err("kernel .text is not mapped at %p", (void *)va);
            return;
        }
        ro++;
    }

    for (vaddr_t va = rodata_start; va < rodata_end; va += PAGE_SIZE) {
        if (!vmm_protect(va, PTE_PRESENT)) {
            pr_err("kernel .rodata is not mapped at %p", (void *)va);
            return;
        }
        ro++;
        nx++;
    }

    /* Everything else the kernel can reach by physical address: .data, .bss,
     * the stacks, the page tables _start built, the VGA framebuffer, and the
     * low memory the loader left behind. All of it is data. */
    for (vaddr_t va = KERNEL_VIRT_BASE; va < KERNEL_VIRT_BASE + VMM_LINEAR_SIZE;
         va += PAGE_SIZE) {
        if (va >= text_start && va < rodata_end)
            continue;
        if (!vmm_protect(va, PTE_PRESENT | PTE_WRITE))
            continue; /* a hole in the linear map is not this pass's problem */
        nx++;
    }

    harden_note_text_ro();

    pr_info("kernel .text and .rodata mapped read-only (%u pages); CR0.WP "
            "makes that binding on ring 0 too",
            ro);
    if (nx_ok)
        pr_info("W^X: %u pages of the kernel's linear map marked no-execute, "
                "so only .text is executable",
                nx);
    else
        pr_warn("no NX bit on this processor: the kernel's data pages are "
                "writable *and* executable, and W^X is only a claim");
}

/* ---- the MMIO window ---------------------------------------------------
 *
 * A bump pointer, because every mapping here is permanent: see mm/vmm.h.
 * Page granularity, so two devices whose registers share a page get one
 * mapping between them - which is correct, and is why the offset within the
 * page has to be carried through to the returned pointer.
 */
static u32 mmio_next;

u32 vmm_mmio_used(void)
{
    return mmio_next;
}

void *vmm_map_mmio(paddr_t phys, size_t bytes, bool uncached)
{
    if (bytes == 0)
        return NULL;

    paddr_t first = PAGE_TRUNC(phys);
    u32 offset = phys - first;
    size_t span = PAGE_ALIGN(offset + bytes);

    if (mmio_next + span > VMM_MMIO_SIZE) {
        pr_err("the MMIO window is full: %u KiB in use, %u KiB more wanted",
               mmio_next / KIB, (unsigned)(span / KIB));
        return NULL;
    }

    vaddr_t va = VMM_MMIO_BASE + mmio_next;
    u32 flags = PTE_PRESENT | PTE_WRITE | (uncached ? PTE_PCD : 0);

    if (!vmm_map_range(va, first, span, flags)) {
        pr_err("cannot map %u KiB of MMIO at phys %p", (unsigned)(span / KIB),
               (void *)first);
        return NULL;
    }

    mmio_next += span;

    pr_debug("mapped phys %p+%u -> %p (%s)", (void *)phys, (unsigned)bytes,
             (void *)(va + offset), uncached ? "uncached" : "cached");

    return (void *)(va + offset);
}

void vmm_switch_address_space(paddr_t pdpt_phys)
{
    if (pdpt_phys && pdpt_phys != read_cr3())
        write_cr3(pdpt_phys);
}

u32 vmm_count_user_pages(void)
{
    u32 count = 0;
    bool irqs = irq_save();

    for (u32 pdpti = 0; pdpti < USER_PDPT_COUNT; pdpti++) {
        volatile pte_t *pd = pd_entries(pdpti);

        for (u32 pdi = 0; pdi < PD_SELF_SLOT; pdi++) {
            if (!(pd[pdi] & PTE_PRESENT))
                continue;

            volatile pte_t *pt = pt_entries(pdpti, pdi);

            for (u32 pti = 0; pti < PT_ENTRIES; pti++)
                if (pt[pti] & PTE_PRESENT)
                    count++;
        }
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
    /* The top 2 MiB of each user gigabyte is that gigabyte's page-table
     * window: mapped, but supervisor-only, so a user access there is a
     * protection fault on a kernel structure rather than a missing page. */
    if (addr < KERNEL_VIRT_BASE) {
        if ((addr & 0x3FFFFFFFu) >= 0x3FE00000u)
            return "a user gigabyte's page-table window, which is "
                   "supervisor-only";
        return "user space";
    }
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
    if (addr >= VMM_MMIO_BASE && addr < VMM_MMIO_BASE + VMM_MMIO_SIZE)
        return "the MMIO window - a device register that is not mapped";
    if (addr >= VMM_TEMP_BASE &&
        addr < VMM_TEMP_BASE + VMM_TEMP_SLOTS * PAGE_SIZE)
        return "a temporary frame-mapping slot that is not currently mapped";
    if (addr >= KSTACK_BASE && addr < KSTACK_BASE + KSTACK_REGION)
        return "a kernel-stack guard page - a task overran its stack";
    if (addr >= PT_WINDOW(KERNEL_PDPT_INDEX))
        return "the kernel gigabyte's page-table window";
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

    pte_t pte = vmm_pte(addr);

    if (!(pte & PTE_PRESENT) || !(pte & PTE_COW))
        return false;

    paddr_t old_frame = (paddr_t)(pte & PTE_ADDR_MASK);
    vaddr_t page = PAGE_TRUNC(addr);

    stat_cow_faults++;

    /* Sole owner: the other side already broke its copy, so there is nothing
     * to duplicate. Just restore write access. This is the common case in a
     * fork-then-exit pattern and it costs one PTE write. */
    if (pmm_frame_refs(old_frame) <= 1) {
        set_pte(page, (pte & ~(pte_t)PTE_COW) | PTE_WRITE);
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

    /* The new frame is private: writable, owned, no longer COW. NX comes
     * across from the old entry, so breaking a copy-on-write page cannot
     * quietly turn a data page into an executable one - which is exactly the
     * kind of hole a fork-heavy workload would open if this copied only the
     * low twelve bits. */
    set_pte(page, (pte_t)fresh | (pte & (0xFFF | PTE_NX) & ~(pte_t)PTE_COW) |
                      PTE_WRITE | PTE_PRESENT | PTE_OWNED);

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

    const char *where = fault_region(addr);

    /* A fault taken in ring 3 kills the *process*, not the kernel.
     *
     * This is the difference between a kernel bug and a program bug, and
     * until a fuzz target pointed it out this code did not make it: every
     * unresolved fault panicked, so a user program dereferencing a null
     * pointer took the machine down with it. The route the fuzzer found was
     * an ELF whose entry point lay outside any segment it mapped - the loader
     * accepted it, usermode_enter() IRETed to an unmapped address, and the
     * kernel panicked on behalf of a program that had done nothing but be
     * malformed.
     *
     * The low two bits of the saved CS are the privilege level the fault was
     * taken at, and they are the whole test. There is no ambiguity here and
     * no judgement call: ring 3 cannot have corrupted kernel state, because
     * every way it could have was already checked at the syscall boundary. */
    if ((r->cs & 3) != 0) {
        struct task *t = task_current();

        /* Rate limited, and for the same reason the syscall warnings are: a
         * program that forks children which fault on purpose would otherwise
         * print a full register dump per child, as fast as it can fork. */
        static struct log_ratelimit rl = {
            .window_ms = LOG_RATELIMIT_WINDOW_MS,
            .burst = LOG_RATELIMIT_BURST,
        };

        if (log_ratelimit_allow(&rl, LOG_ERROR, LOG_TAG)) {
            log_emit(LOG_ERROR, LOG_TAG,
                     "pid %u \"%s\" faulted at %p (%s); killing it",
                     t ? t->pid : 0, t ? t->name : "?", (void *)addr, where);
            page_fault_describe(r);
        }

        /* A non-zero exit code, so a parent waiting on it can tell a fault
         * from a clean exit. */
        task_exit(-11);
    }

    /* A fault in ring 0 is a kernel bug, so the job is to report it as
     * precisely as possible. Demand paging is the other natural resolvable
     * case - see docs/ROADMAP.md. */
    kprintf("\n");
    page_fault_describe(r);
    kprintf("  region          : %s\n", where);

    panic_with_regs(r, "page fault at %p in %s", (void *)addr, where);
}

void vmm_init(void)
{
    ASSERT(!initialised);

    /* Adopt what _start built. The symbol values are already physical
     * addresses: .boot is linked at its load address, which is the whole
     * reason that section exists. */
    kernel_pdpt_phys = (paddr_t)boot_pdpt;
    kernel_pd_phys = (paddr_t)boot_page_directory;

    if (read_cr3() != kernel_pdpt_phys)
        panic("CR3 is %p but the boot PDPT is at %p", (void *)read_cr3(),
              (void *)kernel_pdpt_phys);

    if (!(read_cr4() & CR4_PAE))
        panic("CR4.PAE is clear, so the processor is reading 32-bit page "
              "table entries out of 64-bit tables");

    if (!(pd_entries(KERNEL_PDPT_INDEX)[PD_SELF_SLOT] & PTE_PRESENT))
        panic("the kernel directory's self-map is missing; page tables would "
              "be unreachable");

    /* Before anything sets bit 63 of an entry. On a processor without NX this
     * fails and leaves nx_ok false, and every mapping is made executable -
     * which is what the kernel did before PAE, so it still runs, it just
     * cannot enforce W^X. vmm_protect_kernel_text() says so out loud. */
    nx_ok = vmm_enable_nx();

    /* _start could not set the bit on the self-maps it built, because EFER
     * had not been touched yet and a reserved bit set in a live entry faults
     * every walk through it. Now it can be set: the entry covers only the
     * 2 MiB window it creates, so this makes the window non-executable
     * without touching anything mapped under the directory's other slots. */
    if (nx_ok)
        for (u32 k = 0; k < PDPT_ENTRIES; k++)
            pd_entries(k)[PD_SELF_SLOT] |= PTE_NX;

    stat_page_tables = VMM_BOOT_MAPPED / (2 * MIB); /* the ones _start made */
    stat_mapped_pages = VMM_BOOT_MAPPED / PAGE_SIZE;

    isr_install_handler(14, page_fault_handler);

    /* The one page table that cannot be created on demand.
     *
     * Everything else in the kernel half arrives through vmm_map(), which
     * calls ensure_table() and gets a table built for it. temp_map() cannot:
     * it writes a page table entry directly, precisely so that it does not
     * depend on anything that might itself need a temporary mapping, and a
     * missing table there is a page fault inside the mechanism the fault
     * handler would need to resolve it. So its slot is claimed here, before
     * the first temporary mapping is made.
     *
     * This is also what is left of the function that used to pre-allocate all
     * 255 kernel page tables. One region genuinely needs it; the rest were
     * paying for a problem PAE removed. */
    _Static_assert(VMM_TEMP_SLOTS * PAGE_SIZE <= 2 * MIB,
                   "the temporary slots must fit in one directory entry, "
                   "because only one is reserved here");

    if (!ensure_table(PDPT_INDEX(VMM_TEMP_BASE), PDE_INDEX(VMM_TEMP_BASE),
                      PTE_PRESENT | PTE_WRITE))
        panic("cannot reserve the page table for the temporary mapping "
              "slots at %p",
              (void *)VMM_TEMP_BASE);

    /* Widen the linear map beyond what _start could reach with its two page
     * tables. Everything the kernel addresses physically must be inside this
     * window. No PTE_EXEC: the kernel's code is in the first 4 MiB, which
     * _start already mapped, and nothing above it is ever executed. */
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
     * The directory for gigabyte 0 stays, with its self-map; it is now an
     * empty user directory like the other two. Nothing may hold a low pointer
     * at this point. Low physical memory is still reachable, through the
     * linear map at KERNEL_VIRT_BASE. */
    for (u32 pdi = 0; pdi < VMM_BOOT_MAPPED / (2 * MIB); pdi++)
        pd_entries(0)[pdi] = 0;

    /* A directory changed wholesale, so the whole TLB has to go - invlpg
     * would only cover one page, and this invalidated 1024 of them. */
    write_cr3(kernel_pdpt_phys);

    initialised = true;

    pr_info("paging: PAE, kernel at %p, linear map %u MiB, identity map "
            "dropped",
            (void *)KERNEL_VIRT_BASE, (unsigned)(VMM_LINEAR_SIZE / MIB));
    pr_info("the kernel's gigabyte is one PDPT entry, so every address space "
            "shares its page directory rather than copying 256 entries");
    pr_debug("PDPT at phys %p, kernel page directory at phys %p, %u page "
             "tables",
             (void *)kernel_pdpt_phys, (void *)kernel_pd_phys,
             stat_page_tables);
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

    out->page_dirs = stat_page_dirs;
    out->page_tables = stat_page_tables;
    out->mapped_pages = stat_mapped_pages;
    out->page_faults = stat_page_faults;
    out->cow_faults = stat_cow_faults;
    out->cow_copies = stat_cow_copies;
    out->address_spaces = stat_address_spaces;
}
