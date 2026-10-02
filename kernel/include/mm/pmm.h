/* StratumOS - physical memory manager.
 *
 * A flat bitmap over every 4 KiB frame the machine reports. One bit per frame
 * costs 32 KiB of bitmap per GiB of RAM, which is a good trade at this scale:
 * allocation is a bounded scan from a rotating hint, and freeing is O(1).
 *
 * The bitmap starts fully *used* and regions reported usable by the firmware
 * are punched out of it. Starting from "everything is reserved" means a BIOS
 * that forgets to mention a region causes us to waste it, rather than to hand
 * out MMIO space as if it were RAM.
 */
#ifndef _MM_PMM_H
#define _MM_PMM_H

#include <boot/bootinfo.h>
#include <kernel/types.h>

#define PAGE_SIZE  4096u
#define PAGE_SHIFT 12
#define PAGE_MASK  (PAGE_SIZE - 1)

#define PFN(addr)       ((u32)(addr) >> PAGE_SHIFT)
#define PFN_PHYS(pfn)   ((paddr_t)(pfn) << PAGE_SHIFT)
#define PAGE_ALIGN(x)   ALIGN_UP((u32)(x), PAGE_SIZE)
#define PAGE_TRUNC(x)   ALIGN_DOWN((u32)(x), PAGE_SIZE)

#define PMM_NO_FRAME ((paddr_t)0)

void pmm_init(const struct boot_params *bp);

/* Allocate one frame. Returns PMM_NO_FRAME when out of memory. */
paddr_t pmm_alloc_frame(void);
/* Allocate `count` physically contiguous frames (for DMA-style needs). */
paddr_t pmm_alloc_frames(size_t count);
void    pmm_free_frame(paddr_t frame);
void    pmm_free_frames(paddr_t frame, size_t count);

/* Mark a physical range as never-allocatable. Used for firmware regions,
 * the kernel image itself and the structures the bootloader left behind. */
void pmm_reserve_range(paddr_t start, paddr_t end);

struct pmm_stats {
    u32 total_frames;
    u32 used_frames;
    u32 free_frames;
    u32 reserved_frames;
    u64 highest_addr;
    u32 alloc_calls;
    u32 free_calls;
};

void pmm_get_stats(struct pmm_stats *out);

#endif /* _MM_PMM_H */
