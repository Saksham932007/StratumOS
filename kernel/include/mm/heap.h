/* StratumOS - kernel heap.
 *
 * A first-fit free-list allocator over a growable virtual region. Each block
 * carries a header and a footer magic; kfree() validates both before touching
 * the list, which turns the usual silent heap corruption into an immediate,
 * located panic. Adjacent free blocks are coalesced on release so that a
 * malloc/free churn loop does not fragment the arena into dust.
 */
#ifndef _MM_HEAP_H
#define _MM_HEAP_H

#include <kernel/types.h>

void heap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *kcalloc(size_t n, size_t size);
void *krealloc(void *ptr, size_t size);
/* Allocation whose payload starts on a `align`-byte boundary (power of two).
 * Needed for page-aligned structures such as page directories. */
void *kmalloc_aligned(size_t size, size_t align);
void kfree(void *ptr);

struct heap_stats {
    u32 region_bytes; /* virtual bytes committed to the heap */
    u32 used_bytes;   /* payload + header overhead in use    */
    u32 free_bytes;
    u32 block_count;
    u32 free_blocks;
    u32 largest_free;
    u32 alloc_calls;
    u32 free_calls;
};

void heap_get_stats(struct heap_stats *out);

/* Walk every block and validate its magics and list links.
 * Returns the number of problems found; 0 means the heap is consistent. */
unsigned heap_check(void);

#endif /* _MM_HEAP_H */
