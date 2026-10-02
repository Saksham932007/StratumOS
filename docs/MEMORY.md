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

The kernel occupies the top gigabyte of every address space; the bottom three
belong to user processes.

```
-- user space ----------------------------------------------------------
0x00000000 - 0x00000FFF   UNMAPPED - the null page, deliberately
0x00001000 - 0x003FFFFF   unmapped by design (catches low wild pointers)
0x00400000 - ...          a user program's image, from its own ELF
...
0xAFFFC000 - 0xB0000000   user stack (4 pages, USER|WRITE)
...
-- kernel space --------------------------------------------------------
0xC0000000 - 0xC0FFFFFF   linear map of the first 16 MiB of physical memory
  0xC0000000              physical 0
  0xC00B8000              the VGA text framebuffer (physical 0xB8000)
  0xC0100000              the kernel image (loaded at physical 0x100000)
  0xC012E000              the frame-allocator bitmap
0xD0000000 - 0xD4000000   kernel heap window, backed on demand, 64 MiB max
...
0xFFC00000 - 0xFFFFEFFF   recursive window: every page table
0xFFFFF000 - 0xFFFFFFFF   recursive window: the page directory itself
```

The kernel is **loaded** at 1 MiB physical but **linked** at `0xC0100000`.
`linker/kernel.ld` expresses that with `AT()`: a section's VMA is where its
code expects to be, its LMA is where the loader must put it.

That same `KERNEL_VIRT_BASE` offset doubles as a **linear map**: physical
address P is reachable at `P + 0xC0000000` for the first `VMM_LINEAR_SIZE`
(16 MiB). It is how the kernel reaches the VGA framebuffer, the loader's
tables and its own page frames, and it is why `phys_to_virt()` is one addition
rather than a page-table walk.

### Getting there: the chicken-and-egg problem

`_start` cannot run at `0xC0100000`, because nothing is mapped there yet. So it
lives in its own `.boot` section whose **VMA equals its LMA**, at 1 MiB - an
address valid both with paging off and, through the identity map it installs,
immediately after paging is on.

```asm
; .boot has VMA == LMA, so these symbols are already physical addresses.
mov     edi, boot_page_table
...
mov     [edi], eax                         ; PDE 0    identity, temporary
mov     [edi + KERNEL_PDE_INDEX * 4], eax  ; PDE 768  the kernel's window
mov     [edi + RECURSIVE_SLOT * 4], eax    ; PDE 1023 the directory itself
mov     cr3, eax
mov     eax, cr0
or      eax, CR0_PG | CR0_WP
mov     cr0, eax
lea     eax, [higher_half]
jmp     eax                                ; absolute: a relative jump would
                                           ; stay down here
```

One page table serves both windows, deliberately: the identity map covers
`0x00000000-0x003FFFFF` and the kernel window maps `0xC0000000-0xC03FFFFF` to
the same physical range, so the same 1024 entries do both jobs.

`vmm_init()` then widens the linear map to 16 MiB and **drops PDE 0**. That
removal is what frees the bottom of the address space for user processes, and
it is why `e_entry` in the kernel ELF is a low address - both loaders jump to
it with paging off.

`check-kernel.py` verifies the whole arrangement at build time:

```
boot segment runs where it loads (0x100000)
3 higher-half segment(s), all at +0xc0000000
```

### Why the null page is unmapped

Once the boot identity mapping is dropped, the whole bottom of the address
space is empty until a process maps something into it - so a NULL dereference
faults for free. User images are additionally refused below 4 MiB, so a small
wild pointer faults too rather than landing in a program's own text.

```
stratum> fault null
  faulting address: 0x00000000
  access          : write from ring 0
  reason          : nothing is mapped at that address
  region          : the null page - almost certainly a NULL dereference
```

Nothing of the loader's survives the transition: `boot_parse()` **copies** the
command line and loader name into kernel buffers rather than pointing at them.
That is not fastidiousness - pointing at them works for exactly as long as the
identity map lasts, and then faults the first time anything prints them. It
did, during this work:

```
 page fault at 0x00008400 in low user space (unmapped by design)
  at  strlen+0x6
Call trace:
  [0] emit_number+0xd67
  [1] kprintf+0x29
  [2] cmd_version+0x5c
```

A test now asserts that those strings live at kernel addresses and are still
readable, so the bug cannot return quietly.

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

## Historical note: this was once identity-mapped

Until v0.4.0 the kernel was linked and loaded at 1 MiB, with virtual addresses
equal to physical ones. That kept both boot paths trivially simple, but it
meant every process had to share the bottom of its address space with the
kernel - so there was no clean way to give userspace a private 3 GiB, and no
real isolation beyond the `USER` bit.

The relocation cost a link script using `AT()`, about forty instructions of
early assembly, and an audit of every place that assumed virtual equals
physical: the frame-allocator bitmap, the VGA framebuffer, the boot
information block. What it bought is the precondition for per-process address
spaces - and a kernel/user boundary that is now a single comparison:

```c
/* core/syscall.c - this used to have to enumerate individual kernel windows */
if (is_kernel_address(base) || is_kernel_address(base + len - 1))
    return false;
```

It also moved the ring-3 payload from a section of the kernel to a genuinely
separate ELF, because a program linked at a kernel address and mapped
elsewhere has every string literal pointing where ring 3 cannot read. See
[USERSPACE.md](USERSPACE.md).

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
- **One address space.** The kernel is in the higher half, which is the
  precondition for per-process address spaces, but there is still a single
  page directory: `context_switch` does not touch CR3, and two user programs
  would share a view of memory. That is the next thing to build.
- **The linear map is capped at 16 MiB**, so the frame bitmap and anything
  else the kernel addresses physically must fit below it. `pmm_init()` panics
  with an explicit message rather than corrupting memory if it does not.
