/* StratumOS - fuzzing the kernel heap by driving it from a byte stream.
 *
 * The allocator is not a parser, so there is no hostile input to hand it.
 * What there is instead is an enormous space of *operation sequences*, and
 * allocator bugs live in the sequences rather than in any single call:
 * coalescing two blocks whose neighbour is in a particular state, splitting a
 * block whose remainder is exactly the header size, reusing a freed block
 * whose footer was clobbered by the allocation before it.
 *
 * So the fuzzer's bytes are a program. Each byte is an opcode, and libFuzzer's
 * coverage feedback explores the state machine - which is a far better
 * generator of awkward sequences than a hand-written stress test, because it
 * keeps the sequences that reached new code.
 *
 * The invariants checked after every operation:
 *
 *   heap_check()     walks the whole arena and validates every header, every
 *                    footer and every guard magic. The kernel's own answer to
 *                    "is the heap still consistent", run here thousands of
 *                    times a second instead of once per shell command.
 *   alignment        every payload is 8-byte aligned, which a split that
 *                    miscomputes the remainder breaks - and which is how the
 *                    footer came to be 8 bytes for a 4-byte magic.
 *   isolation        each block holds a pattern derived from its own index,
 *                    rechecked before it is freed. An allocator that hands
 *                    out overlapping blocks is caught by the pattern rather
 *                    than by a sanitizer, because both blocks are inside the
 *                    arena and ASan sees nothing wrong.
 */
#include <stdint.h>

#include <mm/heap.h>

#include "shim.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SLOTS 64

struct slot {
    unsigned char *p;
    size_t size;
    unsigned char tag;
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerInitialize(int *argc, char ***argv);

/* The real kernel heap, on a host arena: heap_init() maps its own window
 * through the VMM, which the shim backs with mmap at the kernel's own
 * addresses, so the arena is laid out exactly as it is in the kernel.
 *
 * Once, not per input. heap_init() asserts it has not already run - which is
 * correct kernel behaviour, since calling it twice would orphan the first
 * arena - so the target has to respect that rather than work around it.
 *
 * The consequence is that fragmentation carries across inputs. That is
 * tolerable and arguably right: allocator bugs live in histories, not in
 * single calls. And it stays close to reproducible because every input frees
 * everything it allocated before returning, which a coalescing first-fit
 * allocator collapses back to one free block. */
int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc;
    (void)argv;

    shim_reset();
    heap_init();

    return 0;
}

static void fill(struct slot *s)
{
    memset(s->p, s->tag, s->size);
}

static void verify(const struct slot *s, unsigned op)
{
    for (size_t i = 0; i < s->size; i++) {
        if (s->p[i] != s->tag) {
            fprintf(stderr,
                    "block of %zu bytes tagged %02x was overwritten at "
                    "offset %zu (found %02x) after operation %u\n",
                    s->size, s->tag, i, s->p[i], op);
            assert(0 && "a live allocation was modified by another operation");
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2 || size > 4096)
        return 0;

    struct slot slots[SLOTS];

    memset(slots, 0, sizeof(slots));

    for (size_t i = 0; i + 1 < size; i += 2) {
        unsigned op = data[i] & 0x07;
        unsigned which = data[i] >> 3; /* 0-31, into the low slots */
        size_t arg = data[i + 1];

        which %= SLOTS;

        struct slot *s = &slots[which];

        switch (op) {
        case 0:
        case 1: /* allocate; twice as likely as anything else */
            if (s->p)
                break;
            s->size = arg * 8 + 1; /* 1 to 2041 bytes, never zero */
            s->p = kmalloc(s->size);
            if (s->p) {
                /* The two properties every payload must have. */
                assert(((uintptr_t)s->p & 7) == 0);
                s->tag = (unsigned char)(which + 1);
                fill(s);
            } else {
                s->size = 0;
            }
            break;

        case 2: /* free */
            if (!s->p)
                break;
            verify(s, op);
            kfree(s->p);
            s->p = NULL;
            s->size = 0;
            break;

        case 3: /* grow or shrink */
            if (!s->p)
                break;
            verify(s, op);
            {
                size_t want = arg * 16 + 1;
                unsigned char *moved = krealloc(s->p, want);

                if (moved) {
                    /* krealloc must preserve the smaller of the two sizes.
                     * Checking the preserved prefix is the point: an
                     * allocator that copies the *new* size reads past the end
                     * of the old block. */
                    size_t keep = want < s->size ? want : s->size;

                    for (size_t k = 0; k < keep; k++)
                        assert(moved[k] == s->tag);

                    s->p = moved;
                    s->size = want;
                    assert(((uintptr_t)s->p & 7) == 0);
                    fill(s);
                }
            }
            break;

        case 4: /* aligned allocation */
            if (s->p)
                break;
            {
                size_t align = (size_t)1 << (3 + (arg & 7)); /* 8 .. 1024 */

                s->size = (arg >> 3) * 8 + 1;
                s->p = kmalloc_aligned(s->size, align);
                if (s->p) {
                    assert(((uintptr_t)s->p & (align - 1)) == 0);
                    s->tag = (unsigned char)(which + 1);
                    fill(s);
                } else {
                    s->size = 0;
                }
            }
            break;

        case 5: /* the degenerate calls, which must not be special-cased away */
            (void)kmalloc(0);
            kfree(NULL);
            if (s->p) {
                verify(s, op);
                /* krealloc to zero is a free, and must not return a pointer
                 * the caller could then use. */
                assert(krealloc(s->p, 0) == NULL);
                s->p = NULL;
                s->size = 0;
            }
            break;

        case 6: /* verify everything that is live */
            for (unsigned k = 0; k < SLOTS; k++)
                if (slots[k].p)
                    verify(&slots[k], op);
            break;

        default: /* walk the arena */
            assert(heap_check() == 0);
            break;
        }

        /* After every single operation. The kernel runs this on demand; here
         * it runs thousands of times a second, which is the difference
         * between an invariant that is documented and one that is enforced. */
        assert(heap_check() == 0);
    }

    for (unsigned k = 0; k < SLOTS; k++) {
        if (slots[k].p) {
            verify(&slots[k], 0xFF);
            kfree(slots[k].p);
        }
    }

    assert(heap_check() == 0);

    return 0;
}
