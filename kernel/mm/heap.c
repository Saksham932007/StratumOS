/* StratumOS - kernel heap.
 *
 * First-fit allocator over an address-ordered list of blocks living in a
 * dedicated virtual window (KHEAP_BASE). Pages are mapped into that window on
 * demand, so the heap's footprint tracks its actual use rather than being
 * committed up front.
 *
 * Every block carries a header magic and a footer magic written just past the
 * payload. kfree() validates both before it touches a single list pointer.
 * This matters more than it looks: the usual failure mode of a kernel heap is
 * a one-byte overrun that corrupts the *next* block's header, and the crash
 * then happens in an unrelated allocation minutes later. Checking the footer
 * turns that into an immediate panic naming the guilty pointer.
 *
 * Adjacent free blocks are coalesced on release, in both directions, so a
 * malloc/free churn loop does not grind the arena into unusable fragments.
 */
#define LOG_TAG "heap"

#include <arch/io.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/string.h>
#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#define HEAP_MAGIC_ALLOC 0xA110C8EDu
#define HEAP_MAGIC_FREE  0xF3EEB10Cu
#define HEAP_FOOTER      0xDEADBEEFu

/* Every payload is 8-byte aligned, because a u64 or a double stored in a
 * kmalloc'd struct must not straddle a boundary. */
#define HEAP_ALIGN 8

struct block {
    u32 magic;
    u32 size; /* payload bytes, not counting header or footer */
    struct block *next;
    struct block *prev;
    u32 free;
    u32 pad; /* keeps sizeof(struct block) a multiple of HEAP_ALIGN */
};

#define HDR  ((u32)sizeof(struct block))

/* The footer holds a 4-byte magic but occupies 8, so that HDR + FTR is a
 * multiple of HEAP_ALIGN. With a 4-byte footer, splitting a block puts the
 * next header 28 bytes along and its payload ends up only 4-byte aligned -
 * which is exactly the bug the alignment assertion in core/ktest.c caught. */
#define FTR  (8u)

/* Smallest payload worth splitting a block for. Below this, the leftover
 * fragment costs more in metadata than it could ever serve. */
#define MIN_PAYLOAD 16
#define MIN_SPLIT   (HDR + MIN_PAYLOAD + FTR)

static struct block *head;
static u32 region_bytes;   /* virtual bytes currently mapped */
static u32 alloc_calls, free_calls;
static bool heap_ready;

static inline u8 *payload_of(struct block *b)
{
    return (u8 *)b + HDR;
}

static inline u32 *footer_of(struct block *b)
{
    return (u32 *)(payload_of(b) + b->size);
}

static inline u32 block_bytes(struct block *b)
{
    return HDR + b->size + FTR;
}

static inline struct block *block_of_payload(void *p)
{
    return (struct block *)((u8 *)p - HDR);
}

static void write_guards(struct block *b)
{
    b->magic = b->free ? HEAP_MAGIC_FREE : HEAP_MAGIC_ALLOC;
    *footer_of(b) = HEAP_FOOTER;
}

/* Sanity checks that cost nothing at runtime: if these ever stop holding, the
 * allocator silently starts handing out misaligned memory. */
_Static_assert(HDR % HEAP_ALIGN == 0, "block header must be HEAP_ALIGN sized");
_Static_assert((HDR + FTR) % HEAP_ALIGN == 0,
               "header + footer must be a multiple of HEAP_ALIGN so that "
               "splitting a block keeps the next payload aligned");
_Static_assert(FTR >= sizeof(u32), "footer must hold the guard magic");

/* Grow the heap's virtual window by at least `bytes`, mapping fresh frames. */
static bool heap_grow(u32 bytes)
{
    u32 want = PAGE_ALIGN(bytes);

    /* Grow in reasonable steps rather than a page at a time: each page costs
     * a PMM allocation and a TLB invalidation. */
    if (want < 64 * KIB)
        want = 64 * KIB;

    if (region_bytes + want > KHEAP_MAX_SIZE) {
        want = KHEAP_MAX_SIZE - region_bytes;
        if (want < PAGE_ALIGN(bytes)) {
            pr_err("heap cannot grow past its %u MiB limit",
                   KHEAP_MAX_SIZE / MIB);
            return false;
        }
    }

    vaddr_t from = KHEAP_BASE + region_bytes;
    u32 pages = want / PAGE_SIZE;

    for (u32 i = 0; i < pages; i++) {
        if (!vmm_alloc_at(from + i * PAGE_SIZE, PTE_PRESENT | PTE_WRITE)) {
            /* Unwind the pages we did map so the window stays describable. */
            for (u32 j = 0; j < i; j++)
                vmm_unmap(from + j * PAGE_SIZE);
            pr_err("out of physical memory while growing the heap");
            return false;
        }
    }

    /* Attach the new space: extend the final block if it happens to be free,
     * otherwise append a new free block. */
    struct block *last = head;
    while (last && last->next)
        last = last->next;

    if (last && last->free) {
        last->size += want;
        write_guards(last);
    } else {
        struct block *nb = (struct block *)from;
        nb->size = want - HDR - FTR;
        nb->free = 1;
        nb->next = NULL;
        nb->prev = last;
        write_guards(nb);
        if (last)
            last->next = nb;
        else
            head = nb;
    }

    region_bytes += want;
    return true;
}

void heap_init(void)
{
    ASSERT(vmm_is_enabled());
    ASSERT(!heap_ready);

    head = NULL;
    region_bytes = 0;
    alloc_calls = free_calls = 0;

    if (!heap_grow(KHEAP_INIT_SIZE))
        panic("cannot create the kernel heap");

    heap_ready = true;
    pr_info("kernel heap at %p, %u KiB committed, %u MiB maximum",
            (void *)KHEAP_BASE, region_bytes / KIB, KHEAP_MAX_SIZE / MIB);
}

/* Split `b` so that it serves exactly `size` bytes, returning the remainder
 * to the free list when it is large enough to be useful. */
static void split_block(struct block *b, u32 size)
{
    if (b->size < size + MIN_SPLIT)
        return;

    struct block *rest = (struct block *)(payload_of(b) + size + FTR);

    rest->size = b->size - size - FTR - HDR;
    rest->free = 1;
    rest->next = b->next;
    rest->prev = b;
    write_guards(rest);

    if (b->next)
        b->next->prev = rest;
    b->next = rest;

    b->size = size;
    write_guards(b);
}

static void validate(struct block *b, const char *op, void *ptr)
{
    u32 expect = b->free ? HEAP_MAGIC_FREE : HEAP_MAGIC_ALLOC;

    if (b->magic != HEAP_MAGIC_ALLOC && b->magic != HEAP_MAGIC_FREE)
        panic("%s(%p): block header magic is %08x - not a heap pointer, or "
              "the header was overwritten", op, ptr, b->magic);

    if (b->magic != expect)
        panic("%s(%p): header says %s but the free flag says %s", op, ptr,
              b->magic == HEAP_MAGIC_FREE ? "free" : "allocated",
              b->free ? "free" : "allocated");

    if (*footer_of(b) != HEAP_FOOTER)
        panic("%s(%p): footer magic is %08x, not %08x - the allocation of %u "
              "bytes was overrun", op, ptr, *footer_of(b), HEAP_FOOTER,
              b->size);
}

static void *alloc_from_list(u32 size)
{
    for (struct block *b = head; b; b = b->next) {
        if (!b->free || b->size < size)
            continue;

        validate(b, "kmalloc", b);
        split_block(b, size);
        b->free = 0;
        write_guards(b);
        return payload_of(b);
    }
    return NULL;
}

/* The heap is reachable from interrupt context (the scheduler allocates task
 * stacks, drivers allocate buffers), so every entry point runs with interrupts
 * masked. On a uniprocessor that is the whole of the required mutual
 * exclusion; the critical sections are short and bounded. */
void *kmalloc(size_t size)
{
    void *p;

    if (size == 0)
        return NULL;

    if (!heap_ready)
        panic("kmalloc(%u) before heap_init()", (unsigned)size);

    u32 want = ALIGN_UP((u32)size, HEAP_ALIGN);
    if (want < MIN_PAYLOAD)
        want = MIN_PAYLOAD;

    bool irqs = irq_save();
    alloc_calls++;

    p = alloc_from_list(want);
    if (p) {
        irq_restore(irqs);
        return p;
    }

    /* Nothing fits. Grow and try once more; a second failure is genuine
     * exhaustion. */
    if (!heap_grow(want + HDR + FTR)) {
        irq_restore(irqs);
        return NULL;
    }

    p = alloc_from_list(want);
    irq_restore(irqs);

    if (!p)
        pr_err("kmalloc(%u) failed even after growing the heap",
               (unsigned)size);
    return p;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);

    if (p)
        memset(p, 0, size);
    return p;
}

void *kcalloc(size_t n, size_t size)
{
    /* Reject an overflowing product rather than allocating a tiny buffer the
     * caller believes is huge - a classic path to a heap overflow. */
    if (n && size && n > (size_t)0xFFFFFFFFu / size) {
        pr_err("kcalloc(%u, %u) would overflow", (unsigned)n, (unsigned)size);
        return NULL;
    }
    return kzalloc(n * size);
}

void *kmalloc_aligned(size_t size, size_t align)
{
    if (align <= HEAP_ALIGN)
        return kmalloc(size);

    if ((align & (align - 1)) != 0)
        panic("kmalloc_aligned: alignment %u is not a power of two",
              (unsigned)align);

    u32 want = ALIGN_UP((u32)size, HEAP_ALIGN);
    bool irqs = irq_save();

    /* Find a free block in which an aligned payload address exists with room
     * to spare, then carve the leading gap off as its own free block. */
    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct block *b = head; b; b = b->next) {
            if (!b->free)
                continue;

            u32 base = (u32)payload_of(b);
            u32 aligned = ALIGN_UP(base, (u32)align);

            /* The gap left behind has to be big enough to be a block in its
             * own right; if it is not, step to the next boundary. */
            while (aligned != base && aligned - base < MIN_SPLIT)
                aligned += (u32)align;

            u32 gap = aligned - base;
            if (b->size < gap + want)
                continue;

            validate(b, "kmalloc_aligned", b);

            if (gap == 0) {
                split_block(b, want);
                b->free = 0;
                write_guards(b);
                alloc_calls++;
                irq_restore(irqs);
                return payload_of(b);
            }

            /* Shrink b to the gap, then the new block starts exactly at
             * `aligned - HDR`, which is what makes `aligned` the payload. */
            u32 total = b->size;
            struct block *nb = (struct block *)(aligned - HDR);

            b->size = gap - HDR - FTR;
            b->free = 1;
            write_guards(b);

            nb->size = total - gap;
            nb->free = 1;
            nb->next = b->next;
            nb->prev = b;
            write_guards(nb);

            if (b->next)
                b->next->prev = nb;
            b->next = nb;

            split_block(nb, want);
            nb->free = 0;
            write_guards(nb);
            alloc_calls++;
            irq_restore(irqs);
            return payload_of(nb);
        }

        if (attempt == 0 && !heap_grow(want + (u32)align + HDR + FTR))
            break;
    }

    irq_restore(irqs);
    pr_err("kmalloc_aligned(%u, %u) failed", (unsigned)size, (unsigned)align);
    return NULL;
}

/* Merge `b` with its successor when both are free and physically adjacent. */
static void coalesce_forward(struct block *b)
{
    struct block *n = b->next;

    if (!n || !n->free || !b->free)
        return;

    /* Adjacency check: a gap means the heap grew in two separate mappings and
     * the blocks are not really neighbours. */
    if ((u8 *)b + block_bytes(b) != (u8 *)n)
        return;

    b->size += block_bytes(n);
    b->next = n->next;
    if (n->next)
        n->next->prev = b;
    write_guards(b);
}

void kfree(void *ptr)
{
    if (!ptr)
        return; /* free(NULL) is a no-op, as it should be */

    if ((u32)ptr < KHEAP_BASE || (u32)ptr >= KHEAP_BASE + region_bytes)
        panic("kfree(%p): pointer is outside the heap window [%p, %p)", ptr,
              (void *)KHEAP_BASE, (void *)(KHEAP_BASE + region_bytes));

    bool irqs = irq_save();
    struct block *b = block_of_payload(ptr);

    if (b->magic == HEAP_MAGIC_FREE) {
        irq_restore(irqs);
        panic("kfree(%p): double free", ptr);
    }

    validate(b, "kfree", ptr);

    b->free = 1;
    write_guards(b);
    free_calls++;

    coalesce_forward(b);
    if (b->prev && b->prev->free)
        coalesce_forward(b->prev);

    irq_restore(irqs);
}

void *krealloc(void *ptr, size_t size)
{
    if (!ptr)
        return kmalloc(size);

    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    struct block *b = block_of_payload(ptr);
    validate(b, "krealloc", ptr);

    u32 want = ALIGN_UP((u32)size, HEAP_ALIGN);

    /* Shrinking, or growing within the slack we already rounded up to, needs
     * no copy at all. */
    if (want <= b->size) {
        split_block(b, want);
        return ptr;
    }

    void *fresh = kmalloc(size);
    if (!fresh)
        return NULL; /* the original is left intact, as realloc requires */

    memcpy(fresh, ptr, b->size);
    kfree(ptr);
    return fresh;
}

void heap_get_stats(struct heap_stats *out)
{
    if (!out)
        return;

    bool irqs = irq_save();
    memset(out, 0, sizeof(*out));
    out->region_bytes = region_bytes;
    out->alloc_calls = alloc_calls;
    out->free_calls = free_calls;

    for (struct block *b = head; b; b = b->next) {
        out->block_count++;
        if (b->free) {
            out->free_blocks++;
            out->free_bytes += b->size;
            if (b->size > out->largest_free)
                out->largest_free = b->size;
        } else {
            out->used_bytes += b->size + HDR + FTR;
        }
    }

    irq_restore(irqs);
}

unsigned heap_check(void)
{
    unsigned problems = 0;
    struct block *prev = NULL;
    u32 seen = 0;

    for (struct block *b = head; b; prev = b, b = b->next) {
        if (++seen > 1000000) {
            pr_err("heap_check: block list does not terminate (cycle?)");
            return problems + 1;
        }

        if (b->magic != HEAP_MAGIC_ALLOC && b->magic != HEAP_MAGIC_FREE) {
            pr_err("heap_check: block %p has bad magic %08x", (void *)b,
                   b->magic);
            problems++;
            break; /* the list is untrustworthy from here on */
        }

        if (*footer_of(b) != HEAP_FOOTER) {
            pr_err("heap_check: block %p (%u bytes) has a damaged footer",
                   (void *)b, b->size);
            problems++;
        }

        if (b->prev != prev) {
            pr_err("heap_check: block %p has a broken back-link", (void *)b);
            problems++;
        }

        if ((u32)b < KHEAP_BASE || (u32)b >= KHEAP_BASE + region_bytes) {
            pr_err("heap_check: block %p is outside the heap window",
                   (void *)b);
            problems++;
            break;
        }

        /* Two adjacent free blocks mean coalescing was missed. */
        if (b->free && b->next && b->next->free &&
            (u8 *)b + block_bytes(b) == (u8 *)b->next) {
            pr_err("heap_check: adjacent free blocks at %p were not merged",
                   (void *)b);
            problems++;
        }
    }

    return problems;
}
