/* StratumOS - virtual memory manager (two-level x86 paging).
 *
 * Layout: a single page directory of 1024 entries, each covering 4 MiB via a
 * page table of 1024 4 KiB entries. The kernel identity-maps the low region
 * so that physical addresses the firmware told us about (the VGA framebuffer,
 * the bootloader's memory map, the kernel image) stay reachable, and maps the
 * heap into a dedicated high virtual window that is backed on demand.
 *
 * Identity-mapping the kernel rather than relocating it to the top of the
 * address space is a deliberate, documented trade-off - see docs/MEMORY.md.
 */
#ifndef _MM_VMM_H
#define _MM_VMM_H

#include <kernel/types.h>

#include <mm/pmm.h>

/* Page table / directory entry flags */
#define PTE_PRESENT       0x001
#define PTE_WRITE         0x002
#define PTE_USER          0x004
#define PTE_PWT           0x008
#define PTE_PCD           0x010
#define PTE_ACCESSED      0x020
#define PTE_DIRTY         0x040
#define PTE_PSE           0x080 /* 4 MiB page, in a directory entry */
#define PTE_GLOBAL        0x100
#define PTE_ADDR_MASK     0xFFFFF000u

/* Software-only bits (the CPU ignores 9-11), used to track who owns a frame
 * so vmm_unmap() knows whether to return it to the PMM. */
#define PTE_OWNED         0x200

#define PDE_INDEX(va)     (((u32)(va) >> 22) & 0x3FF)
#define PTE_INDEX(va)     (((u32)(va) >> 12) & 0x3FF)

/* The kernel heap's virtual window. Chosen above every plausible identity
 * mapping so a stray heap pointer used as a physical address faults loudly
 * instead of corrupting low memory. */
#define KHEAP_BASE        0xD0000000u
#define KHEAP_INIT_SIZE   (1 * MIB)
#define KHEAP_MAX_SIZE    (64 * MIB)

/* How much physical memory is identity-mapped at boot. */
#define VMM_IDENTITY_SIZE (16 * MIB)

struct page_directory;

void vmm_init(void);
bool vmm_is_enabled(void);

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
u32 vmm_pte(vaddr_t va);

bool vmm_map_range(vaddr_t va, paddr_t pa, size_t bytes, u32 flags);
void vmm_unmap_range(vaddr_t va, size_t bytes);

struct vmm_stats {
    u32 page_tables;
    u32 mapped_pages;
    u32 page_faults;
};
void vmm_get_stats(struct vmm_stats *out);

#endif /* _MM_VMM_H */
