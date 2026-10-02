# Memory management

Three allocators, each solving a different problem, stacked.

```
kmalloc / kfree          ← bytes, arbitrary sizes, in a virtual window
   │  mm/heap.c
   ▼
vmm_map / vmm_alloc_at   ← pages, virtual → physical, with permissions
   │  mm/vmm.c
   ▼
pmm_alloc_frame          ← 4 KiB physical frames, from the firmware map
      mm/pmm.c
```

---

## Address space

```
0x00000000 ─ 0x00000FFF   UNMAPPED — the null page, deliberately
0x00001000 ─ 0x0009FFFF   identity: low memory, BIOS data, EBDA
0x000B8000 ─ 0x000BFFFF   identity: VGA text framebuffer
0x00100000 ─ 0x0011xxxx   identity: the kernel image
0x0011xxxx ─ 0x0012xxxx   identity: the PMM bitmap (just above the kernel)
0x00120000 ─ 0x00FFFFFF   identity: free physical memory, directly addressable
...
0xAFFFE000 ─ 0xB0000000   the ring-3 demo's user stack (2 pages, USER|WRITE)
...
0xD0000000 ─ 0xD4000000   kernel heap window, backed on demand, 64 MiB max
...
0xFFC00000 ─ 0xFFFFEFFF   recursive window: every page table
0xFFFFF000 ─ 0xFFFFFFFF   recursive window: the page directory itself
```

The first 16 MiB of physical memory is identity-mapped
(`VMM_IDENTITY_SIZE`). That covers the kernel, the PMM bitmap, the VGA
framebuffer and the bootloader's structures, so every physical address the
kernel is already holding keeps working the instant `CR0.PG` is set — which is
what makes enabling paging a non-event rather than a cliff.

### Why the null page is unmapped

The identity mapping starts at the *second* page. The first one holds the
real-mode interrupt vector table and the BIOS data area, neither of which a
protected-mode kernel has any use for. Leaving it unmapped converts every
NULL dereference from a silent write into an immediate, precisely located
page fault:

```
stratum> fault null
  faulting address: 0x00000000
  access          : write from ring 0
  reason          : nothing is mapped at that address
  region          : the null page - almost certainly a NULL dereference
```

There is one guard. If a loader ever placed its information block inside the
first page, `vmm_init()` maps it and says so, because losing the boot info is
worse than losing the check:

```c
if (bp && bp->reserved_hi > bp->reserved_lo && bp->reserved_lo < PAGE_SIZE) {
    pr_warn("the loader's info block overlaps the null page; mapping it "
            "and giving up NULL-dereference detection");
    ...
}
```

No real loader does this. The warning exists so that if one ever does, the
reason the check stopped working is in the log instead of being a mystery.

---

## Physical memory manager (`mm/pmm.c`)

A bitmap, one bit per 4 KiB frame — 32 KiB of bitmap per GiB of RAM. At this
scale that beats a free list, which would have to store its metadata inside
the very pages it hands out.

### The bitmap starts full

Every bit begins set, meaning *used*, and regions the firmware explicitly
reported as usable are punched out:

```c
memset(bitmap, 0xFF, bitmap_bytes);     /* everything is reserved */
used_frames = total_frames;

for (each region) if (type == MEM_USABLE) mark_range_free(...);
```

The inverse — start free, mark the reserved ranges — fails badly when the
firmware omits a region: the allocator hands out memory-mapped device
registers as if they were RAM, and the symptom is a stack that corrupts
itself whenever a device is touched. Starting from "everything is reserved"
means an omission merely wastes memory.

Partially covered frames are rounded the safe way in each direction: a frame
is only freed if it is *entirely* inside a usable region, and reserving rounds
outward.

### The bitmap is sized at runtime

It is placed immediately above the kernel image and sized from the real top of
memory:

```c
bitmap_phys = PAGE_ALIGN((u32)__kernel_end);
total_frames = highest_addr >> PAGE_SHIFT;
```

then reserved against itself, because allocating over it would be memorable.
A fixed-size static array would either waste 128 KiB of `.bss` on a small
machine or silently cap how much RAM a large one could use.

### What is taken back

| Range | Why |
| --- | --- |
| `0` – `1 MiB` | IVT, BIOS data area, EBDA, VGA framebuffer, stage 2's structures. Parts are nominally usable; the ~600 KiB is not worth the class of bug. |
| `__kernel_start` – `__kernel_end` | the kernel image |
| the bitmap | itself |
| the loader's info block | the kernel reads the command line and loader name out of it for its whole life |

### Allocation

A rotating hint plus whole-word skipping, so a full region costs one compare
rather than 32:

```c
if (bitmap[word] == 0xFFFFFFFFu)
    continue;           /* 32 frames skipped at once */
```

Freeing is O(1) and biases the next search towards the frame just released,
which is almost certainly still in cache. A double free panics rather than
corrupting the bitmap:

```c
if (!frame_is_set(pfn))
    panic("pmm_free_frame(%p): frame is already free (double free)", ...);
```

That panic has already paid for itself — it caught a frame being reused after
`vmm_unmap` had released it, in the project's own test code.

---

## Virtual memory manager (`mm/vmm.c`)

Standard two-level x86 paging: a 1024-entry page directory, each entry
covering 4 MiB through a 1024-entry page table.

### The recursive mapping

This is the most interesting mechanism in the kernel.

Once `CR0.PG` is set, a page table can only be written through a virtual
address. But page tables come from the physical allocator, which can return a
frame anywhere in RAM — possibly far outside the identity-mapped window.
Identity-mapping all of physical memory to work around that does not scale,
and on a 4 GiB machine is impossible in a 32-bit address space.

The solution is to point the last page directory entry at the directory
itself:

```c
pd_entries()[1023] = pd_phys | PTE_PRESENT | PTE_WRITE;
```

The hardware walk then does something useful by accident. For an address in
the top 4 MiB, the CPU reads entry 1023 of the directory to find the "page
table", which *is* the directory, and then indexes it as a table. So:

```
0xFFFFF000             → the page directory
0xFFC00000 + i * 4096  → the page table for directory entry i
```

Every page table in the system becomes addressable at a computable virtual
address. The cost is one 4 MiB slot at the very top of the address space, and
`vmm_map()` refuses to map over it:

```c
if (pdi == RECURSIVE_SLOT)
    panic("vmm_map(%p): refusing to map over the recursive page-table "
          "window", (void *)va);
```

Before paging is enabled, physical equals virtual and the tables are addressed
directly; afterwards, everything goes through the window. One pair of
accessors hides the difference:

```c
static u32 *pd_entries(void)
{
    return paging_on ? (u32 *)PD_VADDR : (u32 *)pd_phys;
}
```

### Frame ownership

A page table entry records who allocated its frame, in one of the bits the CPU
ignores:

```c
#define PTE_OWNED 0x200     /* bits 9-11 are available to software */
```

`vmm_alloc_at()` sets it; `vmm_map()` of a caller-supplied frame does not.
`vmm_unmap()` then knows whether returning the frame is its job:

```c
if (entry & PTE_OWNED)
    pmm_free_frame(entry & PTE_ADDR_MASK);
```

Without this distinction, unmapping either leaks every frame it ever mapped,
or "frees" frames it never owned — the VGA framebuffer being the memorable
example. The `vmm` test suite asserts both directions.

### `vmm_protect` vs `vmm_map`

Changing a page's permissions and replacing its mapping are different
operations, and conflating them loses information. `vmm_map()` warns when it
replaces a live mapping, because silently doing so hides bugs; `vmm_protect()`
keeps the frame and the ownership bit and changes only the permission bits, so
an intentional permission change is not reported as an accident.

The ring-3 setup needs exactly this: the `.user` section is already identity
mapped as part of the kernel image, and all that changes is that ring 3 may
read and execute it.

A directory entry's `USER` bit gates its whole 4 MiB range, so widening a
single page to user access widens the directory entry too. Other pages in the
same table keep their own `USER` bit clear and stay inaccessible.

### `CR0.WP`

```c
write_cr0(read_cr0() | CR0_PG | CR0_WP);
```

Without the write-protect bit, ring 0 can write to pages it marked read-only
and the protection is decorative. `fault readonly` in the shell demonstrates
the difference.

### Page faults

Nothing grows a mapping on demand yet, so every fault is a real bug and the
handler's job is to report it as precisely as possible: the faulting address
from `CR2`, the access type and privilege level and reason decoded from the
error code, the region of the address space it landed in, a full register
dump, and a backtrace.

```
  faulting address: 0xc9100000
  access          : write from ring 0
  reason          : the page is mapped read-only (CR0.WP applies to ring 0 too)
  region          : no region the kernel maps - a wild pointer
```

Naming the region is most of the diagnosis. The same "page fault at 0x..."
means very different things in the heap window (a bad heap pointer or a task
stack overflow) and at address zero.

---

## Kernel heap (`mm/heap.c`)

A first-fit allocator over an address-ordered list of blocks, living in a
virtual window at `0xD0000000`. Pages are mapped into that window on demand,
so the heap's physical footprint tracks its actual use.

The high virtual base is chosen so that a heap pointer accidentally used as a
physical address faults loudly instead of corrupting low memory.

### Guard magics

```
[ header: magic, size, next, prev, free, pad ][ payload ][ footer magic ]
         24 bytes                                         8 bytes
```

`kfree()` validates both magics before touching a single list pointer. This
matters more than it looks. The characteristic failure of a kernel heap is a
one-byte overrun that corrupts the *next* block's header, where the crash then
happens in an unrelated allocation minutes later and points at innocent code.
Checking the footer turns that into an immediate panic naming the guilty
pointer and its size:

```c
panic("%s(%p): footer magic is %08x, not %08x - the allocation of %u "
      "bytes was overrun", op, ptr, *footer_of(b), HEAP_FOOTER, b->size);
```

Distinct magics for allocated and free blocks also make a double free
unambiguous rather than a guess.

### Why the footer is 8 bytes for a 4-byte magic

```c
_Static_assert((HDR + FTR) % HEAP_ALIGN == 0,
               "header + footer must be a multiple of HEAP_ALIGN so that "
               "splitting a block keeps the next payload aligned");
```

With a 4-byte footer, `HDR + FTR` is 28. Splitting a block then puts the next
header 28 bytes along and its payload ends up only 4-byte aligned — so a `u64`
or a `double` in a `kmalloc`'d struct straddles a boundary. The in-kernel heap
test asserts 8-byte payload alignment and caught exactly this; the static
assertions exist so that it cannot come back silently.

### Coalescing

Adjacent free blocks are merged on release, in both directions. The adjacency
check is explicit:

```c
if ((u8 *)b + block_bytes(b) != (u8 *)n)
    return;     /* a gap means the heap grew in two separate mappings */
```

`heap_check()` walks the whole arena and validates magics, back-links, bounds
and the absence of unmerged neighbours, returning a problem count. The
`stress` shell command runs several tasks through thousands of alloc/free
cycles and then calls it.

### Interrupt safety

The heap is reachable from interrupt context — the scheduler allocates task
stacks, drivers allocate buffers — so every entry point masks interrupts. On a
uniprocessor that is the whole of the required mutual exclusion, and the
critical sections are short and bounded.

---

## Design trade-off: identity-mapped, not higher-half

The kernel lives at 1 MiB in both the physical and virtual address space,
rather than being relocated to `0xC0000000` as a production kernel would be.

**What that buys.** Both boot paths stay simple: stage 2 copies ELF segments
to their physical addresses and jumps, with no early page tables and no
assembly trampoline that has to run at a different address than it was linked
for. Physical addresses from the firmware remain directly usable. The
`CR0.PG` transition changes nothing observable, which makes it debuggable.

**What it costs.** Every process would have to share the bottom of its address
space with the kernel, so there is no clean way to give userspace a full
private 3 GiB. There is no guard between the kernel image and user mappings
beyond the `USER` bit. It is the main structural reason this kernel cannot
grow real processes without being relocated first.

Moving to the higher half means a linker script using `AT()` to separate
virtual and load addresses, early page tables built before C runs, and a jump
to the virtual entry point — a contained change, and the first item in
[ROADMAP.md](ROADMAP.md).

---

## Current limitations

- **Empty page tables are never reclaimed.** Unmapping the last page in a
  4 MiB region leaves its table allocated — one frame per touched region.
- **No demand paging.** Every fault is a bug.
- **No swap, no page replacement, no memory pressure handling.** `kmalloc`
  returns `NULL` when the heap cannot grow, and callers are expected to check.
- **The heap is a single arena**, so a long-lived small allocation can keep a
  large region from coalescing. Slab caches are the usual answer.
- **64 MiB heap ceiling** (`KHEAP_MAX_SIZE`), chosen to keep the window well
  clear of the recursive mapping.
