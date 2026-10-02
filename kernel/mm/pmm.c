/* StratumOS - physical memory manager.
 *
 * A bitmap over every 4 KiB frame the firmware reported, one bit per frame:
 * 32 KiB of bitmap per GiB of RAM. At this scale that beats a free-list, which
 * would need its metadata stored in the very pages it is handing out.
 *
 * Two design choices worth stating plainly:
 *
 *  1. The bitmap starts entirely *used* and usable regions are punched out of
 *     it. The alternative - start free and mark reserved - means that a region
 *     the firmware forgot to describe gets handed out as RAM, and the symptom
 *     is memory-mapped device registers being used as a stack. Starting
 *     reserved means an omission merely wastes memory.
 *
 *  2. The bitmap itself lives immediately above the kernel image, sized from
 *     the real top of memory, rather than being a fixed-size static array. A
 *     statically sized bitmap either wastes 128 KiB of .bss on a small machine
 *     or silently caps how much RAM the kernel can use on a large one.
 */
#define LOG_TAG "pmm"

#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/string.h>

#include <mm/pmm.h>
#include <mm/vmm.h>

#define BITS_PER_WORD 32

static u32 *bitmap;
static u32 bitmap_words;
static u32 total_frames;
static u32 used_frames;
static u32 reserved_frames;
static u64 highest_addr;
static u32 alloc_calls, free_calls;

/* Rotating search hint: without it, every allocation rescans the low frames
 * that are permanently reserved. */
static u32 search_hint;

static inline bool frame_is_set(u32 pfn)
{
    return (bitmap[pfn / BITS_PER_WORD] >> (pfn % BITS_PER_WORD)) & 1u;
}

static inline void frame_set(u32 pfn)
{
    bitmap[pfn / BITS_PER_WORD] |= 1u << (pfn % BITS_PER_WORD);
}

static inline void frame_clear(u32 pfn)
{
    bitmap[pfn / BITS_PER_WORD] &= ~(1u << (pfn % BITS_PER_WORD));
}

/* Mark [start, end) free, counting only whole frames fully inside the range. */
static void mark_range_free(paddr_t start, paddr_t end)
{
    u32 first = PFN(PAGE_ALIGN(start));
    u32 last = PFN(PAGE_TRUNC(end));

    for (u32 pfn = first; pfn < last && pfn < total_frames; pfn++) {
        if (frame_is_set(pfn)) {
            frame_clear(pfn);
            used_frames--;
        }
    }
}

void pmm_reserve_range(paddr_t start, paddr_t end)
{
    /* Round outward: a partially covered frame must be treated as reserved,
     * because handing out the rest of it would hand out part of something
     * that matters. */
    u32 first = PFN(PAGE_TRUNC(start));
    u32 last = PFN(PAGE_ALIGN(end));

    for (u32 pfn = first; pfn < last && pfn < total_frames; pfn++) {
        if (!frame_is_set(pfn)) {
            frame_set(pfn);
            used_frames++;
        }
        reserved_frames++;
    }
}

void pmm_init(const struct boot_params *bp)
{
    u32 bitmap_bytes;
    paddr_t bitmap_phys;

    ASSERT(bp != NULL);

    highest_addr = bp->mem_highest;

    /* Cap at 4 GiB - 4 KiB: this is a 32-bit kernel without PAE, so a frame
     * number has to fit in a 32-bit address. */
    if (highest_addr > 0xFFFFF000ull)
        highest_addr = 0xFFFFF000ull;

    if (highest_addr <= 2 * MIB)
        panic("only %llu KiB of memory reported - cannot continue",
              highest_addr / KIB);

    total_frames = (u32)(highest_addr >> PAGE_SHIFT);
    bitmap_words = ALIGN_UP(total_frames, BITS_PER_WORD) / BITS_PER_WORD;
    bitmap_bytes = bitmap_words * sizeof(u32);

    /* Park the bitmap just above the kernel image, physically. Paging is
     * already on by the time this runs - _start enabled it - so the bitmap is
     * written through the kernel's linear map rather than at its physical
     * address. It must therefore fit inside the window _start mapped. */
    bitmap_phys = PAGE_ALIGN((u32)__kernel_phys_end);
    bitmap = (u32 *)phys_to_virt(bitmap_phys);

    /* Everything used, then open up what the firmware vouched for. */
    memset(bitmap, 0xFF, bitmap_bytes);
    used_frames = total_frames;
    reserved_frames = 0;

    for (u32 i = 0; i < bp->region_count; i++) {
        const struct mem_region *r = &bp->regions[i];

        if (r->type != MEM_USABLE)
            continue;

        u64 start = r->base;
        u64 end = r->base + r->length;

        if (start >= highest_addr)
            continue;
        if (end > highest_addr)
            end = highest_addr;

        mark_range_free((paddr_t)start, (paddr_t)end);
    }

    /* --- now take back everything that is not really ours to give ------- */

    /* The whole first mebibyte. Parts of it are nominally usable, but it
     * holds the interrupt vector table, the BIOS data area, the EBDA, the
     * VGA framebuffer and our own stage-2 structures. The ~600 KiB it costs
     * is not worth the class of bug it prevents. */
    pmm_reserve_range(0, 1 * MIB);

    /* The kernel image, by physical address. */
    pmm_reserve_range((paddr_t)__kernel_phys_start, (paddr_t)__kernel_phys_end);

    /* The bitmap itself - allocating over it would be memorable. */
    pmm_reserve_range(bitmap_phys, bitmap_phys + bitmap_bytes);

    /* Whatever the bootloader asked us to leave alone (its info block). */
    if (bp->reserved_hi > bp->reserved_lo)
        pmm_reserve_range(bp->reserved_lo, bp->reserved_hi);

    search_hint = PFN(1 * MIB);
    alloc_calls = free_calls = 0;

    if (bitmap_phys + bitmap_bytes > VMM_BOOT_MAPPED)
        panic("the frame bitmap needs %u KiB at %p, which is outside the "
              "%u MiB that _start mapped",
              bitmap_bytes / KIB, (void *)bitmap_phys,
              (unsigned)(VMM_BOOT_MAPPED / MIB));

    pr_info("%u frames total (%llu MiB), %u free (%u MiB), bitmap %u KiB at "
            "phys %p",
            total_frames, highest_addr / MIB, total_frames - used_frames,
            ((total_frames - used_frames) * (PAGE_SIZE / KIB)) / KIB,
            bitmap_bytes / KIB, (void *)bitmap_phys);
}

paddr_t pmm_alloc_frame(void)
{
    alloc_calls++;

    /* Two passes: from the hint to the end, then from the start to the hint.
     * Scanning whole words at a time lets a full region be skipped with one
     * compare instead of 32. */
    for (int pass = 0; pass < 2; pass++) {
        u32 from = (pass == 0) ? search_hint : 0;
        u32 to = (pass == 0) ? total_frames : search_hint;

        for (u32 word = from / BITS_PER_WORD; word < bitmap_words; word++) {
            if (bitmap[word] == 0xFFFFFFFFu)
                continue; /* fully allocated, skip all 32 */

            for (u32 bit = 0; bit < BITS_PER_WORD; bit++) {
                u32 pfn = word * BITS_PER_WORD + bit;

                if (pfn >= to || pfn >= total_frames)
                    break;
                if (pfn < from)
                    continue;
                if (frame_is_set(pfn))
                    continue;

                frame_set(pfn);
                used_frames++;
                search_hint = pfn + 1;
                if (search_hint >= total_frames)
                    search_hint = PFN(1 * MIB);
                return PFN_PHYS(pfn);
            }
        }
    }

    pr_err("out of physical memory (%u/%u frames in use)", used_frames,
           total_frames);
    return PMM_NO_FRAME;
}

paddr_t pmm_alloc_frames(size_t count)
{
    if (count == 0)
        return PMM_NO_FRAME;
    if (count == 1)
        return pmm_alloc_frame();

    /* Linear search for a run of `count` clear bits. Contiguous allocation is
     * rare (it exists for DMA-style buffers), so a simple scan is the right
     * complexity trade. */
    u32 run_start = 0, run = 0;

    for (u32 pfn = PFN(1 * MIB); pfn < total_frames; pfn++) {
        if (frame_is_set(pfn)) {
            run = 0;
            continue;
        }

        if (run == 0)
            run_start = pfn;

        if (++run == count) {
            for (u32 i = 0; i < count; i++) {
                frame_set(run_start + i);
                used_frames++;
            }
            alloc_calls++;
            return PFN_PHYS(run_start);
        }
    }

    pr_err("no run of %u contiguous frames available", (unsigned)count);
    return PMM_NO_FRAME;
}

void pmm_free_frame(paddr_t frame)
{
    u32 pfn = PFN(frame);

    if (!IS_ALIGNED(frame, PAGE_SIZE))
        panic("pmm_free_frame(%p): not page aligned", (void *)frame);

    if (pfn >= total_frames)
        panic("pmm_free_frame(%p): beyond end of memory", (void *)frame);

    if (!frame_is_set(pfn))
        panic("pmm_free_frame(%p): frame is already free (double free)",
              (void *)frame);

    frame_clear(pfn);
    used_frames--;
    free_calls++;

    /* Bias the next search towards the frame we just released: it is almost
     * certainly still in cache. */
    if (pfn < search_hint)
        search_hint = pfn;
}

void pmm_free_frames(paddr_t frame, size_t count)
{
    for (size_t i = 0; i < count; i++)
        pmm_free_frame(frame + i * PAGE_SIZE);
}

void pmm_get_stats(struct pmm_stats *out)
{
    if (!out)
        return;

    out->total_frames = total_frames;
    out->used_frames = used_frames;
    out->free_frames = total_frames - used_frames;
    out->reserved_frames = reserved_frames;
    out->highest_addr = highest_addr;
    out->alloc_calls = alloc_calls;
    out->free_calls = free_calls;
}
