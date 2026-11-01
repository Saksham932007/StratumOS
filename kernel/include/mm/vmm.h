/* StratumOS - virtual memory manager (PAE, three-level x86 paging).
 *
 * Layout: CR3 points at a page-directory-pointer table of four entries, one
 * per gigabyte of the 32-bit address space. Each covers a page directory of
 * 512 entries, each of those a page table of 512 entries, each of those a
 * 4 KiB page. Entries are 64 bits wide.
 *
 * Why PAE, when two-level paging addressed everything a 32-bit kernel can
 * reach anyway
 * --------------------------------------------------------------------------
 * The no-execute bit. It lives at bit 63 of a page table entry, so it does
 * not exist in the 32-bit entry format - on classic two-level paging there is
 * no way to say "this page is data" at all, and a kernel that maps its stack
 * writable has also mapped it executable. W^X was then a claim about what the
 * kernel intended rather than something the hardware would enforce. PAE is
 * the only way to get the bit on 32-bit x86, and getting the bit is the whole
 * reason this is a three-level walk.
 *
 * Two things fell out of it that were not the point but are worth having:
 *
 *   - The kernel's gigabyte is exactly one PDPT entry, so the whole kernel
 *     half hangs off a single page directory and every address space can
 *     share it *by pointer* instead of copying 256 directory entries at
 *     creation. A kernel mapping made after a process was forked is now
 *     visible in that process immediately, which removes a whole category
 *     of bug - see the note on ensure_table() in vmm.c for the machinery
 *     that used to be needed instead.
 *   - Page frames may sit above 4 GiB. Nothing here uses that, because
 *     paddr_t is 32 bits, but the entry format no longer forbids it.
 *
 * The self-mapping window
 * -----------------------
 * Once CR0.PG is set, a page table can only be written through a virtual
 * address - but page tables come from the physical allocator, which can hand
 * back a frame anywhere in RAM, including far outside the kernel's linear
 * map.
 *
 * The two-level trick was to point the last directory entry at the directory
 * itself. That does not transpose directly: CR3 now names a PDPT, and a PDPT
 * entry cannot be walked as a page table. What does work is one self-map per
 * directory: entry 511 of each page directory points at that directory. The
 * hardware walk for an address whose directory index is 511 then uses the
 * directory as both of the last two levels, so within gigabyte k
 *
 *     (k << 30) + 0x3FE00000 + i*4096   -> the page table for PD entry i
 *     (k << 30) + 0x3FFFF000            -> the page directory itself
 *
 * Every page table in the system is addressable at a computable address, at
 * the cost of the top 2 MiB of each gigabyte. Three of those four holes are
 * in user space; they are mapped supervisor-only, so a user process cannot
 * read its own page tables, it just cannot use those addresses either.
 *
 * See docs/MEMORY.md for the full address-space layout.
 */
#ifndef _MM_VMM_H
#define _MM_VMM_H

#include <kernel/types.h>

#include <mm/pmm.h>

/* ---- entry format -------------------------------------------------------
 *
 * A PAE entry is 64 bits: bits 0-11 are flags, 12-51 the frame address,
 * 52-62 reserved or ignored, and 63 the no-execute bit. The low twelve are
 * bit-for-bit what they were in the 32-bit format, which is why every caller
 * that passes PTE_PRESENT | PTE_WRITE needed no change.
 */
typedef u64 pte_t;

/* Page table / directory entry flags */
#define PTE_PRESENT       0x001
#define PTE_WRITE         0x002
#define PTE_USER          0x004
#define PTE_PWT           0x008
#define PTE_PCD           0x010
#define PTE_ACCESSED      0x020
#define PTE_DIRTY         0x040
#define PTE_PSE           0x080 /* 2 MiB page, in a directory entry */
#define PTE_GLOBAL        0x100

/* The frame address, as a mask over the low 32 bits. The field is wider than
 * this in the hardware format, but paddr_t is 32 bits, so a frame this kernel
 * can allocate always fits - and masking a pte_t with it yields the frame
 * directly. */
#define PTE_ADDR_MASK     0xFFFFF000u

/* No-execute. Bit 63, and the reason this kernel uses PAE at all.
 *
 * It must only be set on a processor that has it: on one that does not, bit
 * 63 is *reserved*, and a reserved bit set in any entry on the walk makes
 * every access to that page fault with bit 3 of the error code set, forever.
 * So nothing sets this directly - vmm_map() adds it, and only when the
 * feature was found and EFER.NXE was successfully enabled. */
#define PTE_NX            (1ULL << 63)

/* Software-only bits. The CPU ignores bits 9-11, so a kernel gets three bits
 * per page for free - and all three pay for themselves.
 *
 * PTE_OWNED  this mapping allocated its own frame, so unmapping it should
 *            return the frame to the allocator. Without it, unmap either
 *            leaks every frame or frees frames it does not own - the VGA
 *            framebuffer being the memorable example.
 *
 * PTE_COW    the page is shared copy-on-write: mapped read-only in every
 *            address space holding it, so the first write faults and the
 *            handler gives the writer a private copy.
 *
 * PTE_EXEC   the caller wants this page executable. Expressed as a positive
 *            permission even though the hardware bit is a negative one, so
 *            that the default - a caller who says nothing - is the safe
 *            answer. Every mapping in the kernel is non-executable unless
 *            somebody asked for otherwise, and `grep PTE_EXEC` is the entire
 *            list of places that did.
 */
#define PTE_OWNED         0x200
#define PTE_COW           0x400
#define PTE_EXEC          0x800

/* How many entries at each level, and how to index them. A PDPT entry covers
 * 1 GiB, a directory entry 2 MiB, a table entry 4 KiB. */
#define PDPT_ENTRIES      4
#define PD_ENTRIES        512
#define PT_ENTRIES        512

#define PDPT_INDEX(va)    (((u32)(va) >> 30) & 0x3)
#define PDE_INDEX(va)     (((u32)(va) >> 21) & 0x1FF)
#define PTE_INDEX(va)     (((u32)(va) >> 12) & 0x1FF)

/* The directory slot in every page directory that points at the directory
 * itself, and the window that creates. See the header comment. */
#define PD_SELF_SLOT      511
#define PD_WINDOW(k)      (((u32)(k) << 30) + 0x3FFFF000u)
#define PT_WINDOW(k)      (((u32)(k) << 30) + 0x3FE00000u)

/* The kernel heap's virtual window. Chosen above every plausible identity
 * mapping so a stray heap pointer used as a physical address faults loudly
 * instead of corrupting low memory. */
#define KHEAP_BASE        0xD0000000u
#define KHEAP_INIT_SIZE   (1 * MIB)
#define KHEAP_MAX_SIZE    (64 * MIB)

/* How much physical memory the kernel's linear map covers, at
 * KERNEL_VIRT_BASE. Everything the kernel needs to reach by physical address
 * must be below this: the image, the frame bitmap, the VGA framebuffer and
 * whatever the loader left in low memory. */
#define VMM_LINEAR_SIZE   (16 * MIB)

/* _start maps the first 4 MiB before C runs, using a single page table. The
 * VMM extends the window from there. */
#define VMM_BOOT_MAPPED   (4 * MIB)

/* Where a user process's image is mapped. Well clear of the null page so a
 * NULL dereference still faults, and far below KERNEL_VIRT_BASE. */
#define USER_IMAGE_BASE   0x00400000u

/* The user stack. Placed well below the kernel boundary and well above the
 * program image, so that an overflow in either direction lands on an unmapped
 * page and faults rather than quietly corrupting the other. */
#define USER_STACK_TOP    0xB0000000u
#define USER_STACK_PAGES  4

/* The PDPT slot that belongs to the kernel, and the three below it that
 * belong to user space.
 *
 * Every address space holds the *same* page directory in this slot, so a
 * kernel mapping is not merely identical in all of them, it is the same
 * memory - which is what lets an interrupt be delivered no matter which
 * process was running, and what makes a kernel mapping created at runtime
 * appear in every existing address space at once. */
#define KERNEL_PDPT_INDEX (KERNEL_VIRT_BASE >> 30)
#define USER_PDPT_COUNT   KERNEL_PDPT_INDEX

/* Where kernel task stacks live, one slot each, every slot preceded by an
 * unmapped guard page.
 *
 * Task stacks used to come from kmalloc, which put them in the heap with the
 * next allocation's header immediately below: an overflowing stack would
 * silently eat a heap block's metadata and the damage would surface somewhere
 * else entirely. A page-granular region is what makes a guard page possible,
 * and a guard page turns a stack overflow into a page fault with the
 * offending EIP still in the frame.
 *
 * One slot is a guard page plus the stack, so slot i's guard sits immediately
 * above slot i-1's top and both directions are covered. 4 MiB is one page
 * directory entry and holds far more slots than TASK_MAX. */
#define KSTACK_BASE       0xE0000000u
#define KSTACK_REGION     (4 * MIB)

/* A window for device registers and firmware tables that live outside the
 * linear map.
 *
 * The linear map covers the first VMM_LINEAR_SIZE of physical memory, which
 * is where RAM and the VGA framebuffer are. The things that need this window
 * are not there: the local APIC sits at 0xFEE00000, just below 4 GiB, and
 * QEMU puts the ACPI tables near the top of installed memory - 0x07FE0000 on
 * a 128 MiB machine, eight times beyond the linear map's reach.
 *
 * Mappings here are permanent. A device's registers are needed for as long as
 * the kernel runs, so there is nothing to reclaim and no reason for an
 * allocator more sophisticated than a bump pointer. */
#define VMM_MMIO_BASE     0xCE000000u
#define VMM_MMIO_SIZE     (8 * MIB)

/* Per-CPU boot and idle stacks, laid out exactly like the task stacks at
 * KSTACK_BASE: one guard page, then the stack. A processor needs a stack
 * before it can execute a single line of C, so these cannot come from the
 * task allocator - the scheduler does not exist yet when an application
 * processor arrives. */
#define CPUSTACK_BASE     0xE8000000u
#define CPUSTACK_REGION   (1 * MIB)

/* Two kernel pages reserved for temporarily mapping an arbitrary frame.
 *
 * The recursive window can only reach the *current* address space's tables,
 * but fork has to build the child's while the parent's is active. These slots
 * are how a frame that is not mapped anywhere becomes writable for a moment.
 * Two, because cloning needs the child's page directory and one of its page
 * tables visible at the same time. */
#define VMM_TEMP_BASE     0xCF000000u
#define VMM_TEMP_SLOTS    2

struct page_directory;

void vmm_init(void);
bool vmm_is_enabled(void);
/* Physical address of the kernel's PDPT - what CR3 holds when no user process
 * is running, and what a task's page_dir is compared against to tell "no
 * address space of its own" from "one of its own". */
paddr_t vmm_kernel_root_phys(void);
/* Physical address of the kernel's page directory: the one every address
 * space holds in its fourth PDPT slot, by pointer. */
paddr_t vmm_kernel_pd_phys(void);

/* ---- address spaces ----------------------------------------------------
 *
 * A new address space starts with an empty user half and the kernel's half
 * copied in, so kernel mappings are identical everywhere and an interrupt can
 * be delivered whichever process was running.
 */

/* Returns the physical address of a fresh PDPT, suitable for CR3, or 0. The
 * three user directories are allocated eagerly and the kernel's is shared, so
 * an address space costs four frames and its PDPT is never written again. */
paddr_t vmm_create_address_space(void);

/* A PDPT that identity-maps the first 4 MiB *and* shares the kernel's page
 * directory.
 *
 * Exists for exactly one caller: an application processor enabling paging
 * while still executing at a low physical address. The kernel's own directory
 * has no low mapping - dropping it is what frees user space - so loading that
 * would unmap the instruction after `mov cr0`. Handing the processor a
 * directory with both halves means the kernel's address space is never in a
 * state its own test suite would reject. */
paddr_t vmm_create_bootstrap_pd(void);
void vmm_destroy_bootstrap_pd(paddr_t pdpt_phys);

/* Clone the *current* address space's user half copy-on-write: every writable
 * user page becomes read-only and COW-marked in both parent and child, and
 * the shared frame's reference count goes up. Returns the child's page
 * directory, or 0. */
paddr_t vmm_clone_current(void);

/* Free every user page, page table, user page directory and the PDPT itself.
 * The kernel's directory is shared and is left alone. Must not be the
 * currently loaded address space. */
void vmm_destroy_address_space(paddr_t pdpt_phys);

/* Unmap and free the entire user half of the current address space, leaving
 * the kernel half intact. What exec() needs before loading a new image. */
void vmm_clear_user_space(void);

/* Switch the active address space. */
void vmm_switch_address_space(paddr_t pdpt_phys);

/* Map a physical range into the MMIO window and return a kernel pointer to
 * it. `bytes` may be unaligned; the mapping is rounded out to whole pages and
 * the returned pointer keeps the offset within the first one.
 *
 * `uncached` sets PTE_PCD, which must be used for device registers: a cached
 * read of a status register can return a value the device has already
 * changed, and a cached write may never reach it at all. Firmware tables are
 * ordinary memory and want caching.
 *
 * Returns NULL if the window is full. Never unmapped. */
void *vmm_map_mmio(paddr_t phys, size_t bytes, bool uncached);

/* How much of the MMIO window has been handed out, in bytes. */
u32 vmm_mmio_used(void);

/* Remove write permission from the kernel's own .text and .rodata. Called
 * once, after the heap exists, because a failure here should be reportable
 * rather than a boot-time panic. */
void vmm_protect_kernel_text(void);

/* How many pages are mapped in the user half of the current address space. */
u32 vmm_count_user_pages(void);

/* Map one page. Allocates a page table from the PMM if needed.
 * Returns false only if a required page table could not be allocated. */
bool vmm_map(vaddr_t va, paddr_t pa, u32 flags);
/* Allocate a fresh frame and map it at `va`. */
bool vmm_alloc_at(vaddr_t va, u32 flags);
/* Unmap `va`; frees the backing frame if this mapping owns it. */
void vmm_unmap(vaddr_t va);
/* Change the permission bits of an existing mapping, keeping its frame.
 * Distinct from vmm_map() so that an intentional permission change is not
 * reported as an accidental remap. Returns false if `va` is not mapped. */
bool vmm_protect(vaddr_t va, u32 flags);
bool vmm_protect_range(vaddr_t va, size_t bytes, u32 flags);
/* Translate, or return false if `va` is not mapped. */
bool vmm_translate(vaddr_t va, paddr_t *out);
/* Raw PTE for inspection/debugging. */
pte_t vmm_pte(vaddr_t va);
/* Raw page *directory* entry for slot `pdi` of gigabyte `pdpti`, for
 * inspection. Separate from vmm_pte() because a directory entry's USER and
 * WRITE bits gate a whole 2 MiB range, so they are worth being able to assert
 * on directly. */
pte_t vmm_pde_raw(u32 pdpti, u32 pdi);

/* Read one entry out of any paging structure that is *not* in the current
 * address space, by the physical address of the table. One function for all
 * three levels, because a PDPT, a page directory and a page table are the
 * same thing to a reader: an array of 64-bit entries in a frame.
 *
 * For inspecting an address space from outside it - which the test suite
 * needs, and which is the access pattern that exposed the compiler-reordering
 * bug the map/unmap barriers now prevent. */
bool vmm_inspect_entry(paddr_t table_phys, u32 index, pte_t *out);

/* ---- no-execute --------------------------------------------------------
 *
 * True once the processor was found to support NX and EFER.NXE was set, which
 * together are what make PTE_NX safe to write into an entry. Everything that
 * reports on W^X asks this rather than assuming, because a kernel that
 * silently fell back to executable data pages should say so.
 */
bool vmm_nx_enabled(void);
/* Enable it on *this* processor. Called once by the boot processor before any
 * NX bit is set, and once by each application processor before it loads a
 * page table that already has them - a reserved-bit fault on an AP during
 * bring-up is indistinguishable from the AP never starting. Returns false if
 * the feature is absent. */
bool vmm_enable_nx(void);

bool vmm_map_range(vaddr_t va, paddr_t pa, size_t bytes, u32 flags);
void vmm_unmap_range(vaddr_t va, size_t bytes);

struct vmm_stats {
    u32 page_dirs; /* user page directories; the kernel's is shared */
    u32 page_tables;
    u32 mapped_pages;
    u32 page_faults;
    u32 cow_faults;     /* faults resolved by copy-on-write      */
    u32 cow_copies;     /* of those, ones that needed a real copy */
    u32 address_spaces; /* created                                */
};
void vmm_get_stats(struct vmm_stats *out);

#endif /* _MM_VMM_H */
