/* StratumOS - virtual memory manager (two-level x86 paging).
 *
 * Layout: a single page directory of 1024 entries, each covering 4 MiB via a
 * page table of 1024 4 KiB entries. The kernel identity-maps the low region
 * so that physical addresses the firmware told us about (the VGA framebuffer,
 * the bootloader's memory map, the kernel image) stay reachable, and maps the
 * heap into a dedicated high virtual window that is backed on demand.
 *
 * The kernel lives in the top gigabyte (KERNEL_VIRT_BASE) and reaches low
 * physical memory through a linear map at that same base. See docs/MEMORY.md
 * for the full address-space layout.
 */
#ifndef _MM_VMM_H
#define _MM_VMM_H

#include <kernel/types.h>

#include <mm/pmm.h>

/* Page table / directory entry flags */
#define PTE_PRESENT      0x001
#define PTE_WRITE        0x002
#define PTE_USER         0x004
#define PTE_PWT          0x008
#define PTE_PCD          0x010
#define PTE_ACCESSED     0x020
#define PTE_DIRTY        0x040
#define PTE_PSE          0x080 /* 4 MiB page, in a directory entry */
#define PTE_GLOBAL       0x100
#define PTE_ADDR_MASK    0xFFFFF000u

/* Software-only bits. The CPU ignores bits 9-11, so a kernel gets three bits
 * per page for free - and both of these pay for themselves.
 *
 * PTE_OWNED  this mapping allocated its own frame, so unmapping it should
 *            return the frame to the allocator. Without it, unmap either
 *            leaks every frame or frees frames it does not own - the VGA
 *            framebuffer being the memorable example.
 *
 * PTE_COW    the page is shared copy-on-write: mapped read-only in every
 *            address space holding it, so the first write faults and the
 *            handler gives the writer a private copy.
 */
#define PTE_OWNED        0x200
#define PTE_COW          0x400

#define PDE_INDEX(va)    (((u32)(va) >> 22) & 0x3FF)
#define PTE_INDEX(va)    (((u32)(va) >> 12) & 0x3FF)

/* The kernel heap's virtual window. Chosen above every plausible identity
 * mapping so a stray heap pointer used as a physical address faults loudly
 * instead of corrupting low memory. */
#define KHEAP_BASE       0xD0000000u
#define KHEAP_INIT_SIZE  (1 * MIB)
#define KHEAP_MAX_SIZE   (64 * MIB)

/* How much physical memory the kernel's linear map covers, at
 * KERNEL_VIRT_BASE. Everything the kernel needs to reach by physical address
 * must be below this: the image, the frame bitmap, the VGA framebuffer and
 * whatever the loader left in low memory. */
#define VMM_LINEAR_SIZE  (16 * MIB)

/* _start maps the first 4 MiB before C runs, using a single page table. The
 * VMM extends the window from there. */
#define VMM_BOOT_MAPPED  (4 * MIB)

/* Where a user process's image is mapped. Well clear of the null page so a
 * NULL dereference still faults, and far below KERNEL_VIRT_BASE. */
#define USER_IMAGE_BASE  0x00400000u

/* The user stack. Placed well below the kernel boundary and well above the
 * program image, so that an overflow in either direction lands on an unmapped
 * page and faults rather than quietly corrupting the other. */
#define USER_STACK_TOP   0xB0000000u
#define USER_STACK_PAGES 4

/* The first page directory slot that belongs to the kernel. Every address
 * space shares these entries, so a kernel mapping is identical in all of
 * them - which is what lets an interrupt be delivered no matter which process
 * was running. */
#define KERNEL_PDE_FIRST (KERNEL_VIRT_BASE >> 22)

/* Two kernel pages reserved for temporarily mapping an arbitrary frame.
 *
 * The recursive window can only reach the *current* address space's tables,
 * but fork has to build the child's while the parent's is active. These slots
 * are how a frame that is not mapped anywhere becomes writable for a moment.
 * Two, because cloning needs the child's page directory and one of its page
 * tables visible at the same time. */
#define VMM_TEMP_BASE    0xCF000000u
#define VMM_TEMP_SLOTS   2

struct page_directory;

void vmm_init(void);
bool vmm_is_enabled(void);
/* Physical address of the kernel's page directory. */
paddr_t vmm_kernel_pd_phys(void);

/* ---- address spaces ----------------------------------------------------
 *
 * A new address space starts with an empty user half and the kernel's half
 * copied in, so kernel mappings are identical everywhere and an interrupt can
 * be delivered whichever process was running.
 */

/* Returns the physical address of a fresh page directory, or 0. */
paddr_t vmm_create_address_space(void);

/* Clone the *current* address space's user half copy-on-write: every writable
 * user page becomes read-only and COW-marked in both parent and child, and
 * the shared frame's reference count goes up. Returns the child's page
 * directory, or 0. */
paddr_t vmm_clone_current(void);

/* Free every user page, page table and the directory itself. Must not be the
 * currently loaded address space. */
void vmm_destroy_address_space(paddr_t pd_phys);

/* Unmap and free the entire user half of the current address space, leaving
 * the kernel half intact. What exec() needs before loading a new image. */
void vmm_clear_user_space(void);

/* Switch the active address space. */
void vmm_switch_address_space(paddr_t pd_phys);

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
u32 vmm_pte(vaddr_t va);

bool vmm_map_range(vaddr_t va, paddr_t pa, size_t bytes, u32 flags);
void vmm_unmap_range(vaddr_t va, size_t bytes);

struct vmm_stats {
    u32 page_tables;
    u32 mapped_pages;
    u32 page_faults;
    u32 cow_faults;     /* faults resolved by copy-on-write      */
    u32 cow_copies;     /* of those, ones that needed a real copy */
    u32 address_spaces; /* created                                */
};
void vmm_get_stats(struct vmm_stats *out);

#endif /* _MM_VMM_H */
