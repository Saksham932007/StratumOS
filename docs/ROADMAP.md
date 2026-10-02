# Roadmap

Ordered by what each item would teach, with an honest estimate of what it
takes. Items are sized assuming the existing structure, which was built with
most of these in mind.

---

## Next

### 1. Per-process address spaces, fork, exec, wait

The kernel moved to the higher half in v0.4.0, which was the
precondition. What remains is the part that makes "user space" mean more
than one program: a page directory per task, a CR3 switch in
`context_switch`, an address-space clone, and copy-on-write `fork`.

Copy-on-write is where it gets interesting - mark both copies read-only
and have the page-fault handler duplicate on write. The handler is already
structured to distinguish a resolvable fault from a fatal one; right now
it reports every fault as fatal, with a comment marking where demand
paging goes.

`exec` is half done already: `kernel/core/elf.c` maps PT_LOAD segments
into the current address space with user permissions, validated against a
hostile image. It needs to tear down the old address space first, and to
read its input from a file rather than an embedded blob (item 5).

Roughly two to three weekends, and the boot tests will earn their keep.

### 2. Interrupt-driven serial transmit

`console_write` currently holds interrupts off for the whole of a polled UART
write. On QEMU that is free; on real hardware at 115200 it is ~87 µs per
character and can cost timer ticks during heavy logging.

Needs: a transmit ring buffer, the THR-empty interrupt enabled in IER, and a
drain path in the existing `serial_irq`. The panic path has to keep polling,
because it cannot rely on interrupts.

### 3. Reclaim empty page tables

Unmapping the last page in a 4 MiB region leaves its table allocated. Needs a
per-table mapped-page count, decremented in `vmm_unmap`, freeing the frame and
clearing the directory entry at zero — and a `tlb` flush of the recursive
window entry for that slot.

### 4. Symbolising the profiler's call graph

Samples are attributed to the leaf function only, so a helper called from
several places aggregates all of its callers together. Walking the
frame-pointer chain at sample time would fix it, and the backtrace code
already exists - see [PERFORMANCE.md](PERFORMANCE.md).

### 5. A block device and a filesystem

ATA PIO first, because it needs no DMA and no interrupts. Then FAT16 read-only,
which is enough to load an init binary and is well documented. The existing
`pci.c` already finds the IDE controller.

### 6. A slab allocator over the heap

The heap is one arena, so a long-lived small allocation can keep a large region
from coalescing. Per-size caches for the common fixed-size objects (`struct
task`, page-table wrappers) would fix the fragmentation and speed up the common
path. `heap_check()` already exists to validate the result.

### 7. APIC and the HPET

The 8259 and 8254 are legacy. The local APIC timer and the I/O APIC are what
real hardware uses, and the local APIC is a prerequisite for SMP. `cpu.c`
already detects the APIC feature bit.

### 8. SMP

Needs: ACPI MADT parsing to find the other cores, a trampoline to bring them
out of reset in real mode, per-CPU data, and — at last — `spinlock_t` becoming
a real spinlock. `kernel/core/spinlock.c` is written so that this is a change
of implementation rather than a change of every call site.

### 9. A proper VFS and a `/proc`

Once there is a filesystem, the introspection the shell does through direct
calls (`ps`, `meminfo`, `irq`) belongs behind readable files instead.

---

## Smaller items

| Item | Notes |
| --- | --- |
| Tab completion in the shell | the command table is already the single source of truth |
| ANSI colour over serial | the console layer would need per-sink escape handling |
| `kmalloc` call-site tracking | a `__builtin_return_address(0)` in the block header would make leaks attributable |
| A watchdog on the NMI | `nmi_handler` currently only warns |
| Framebuffer support | the Multiboot2 framebuffer tag is already requested but ignored |
| PS/2 mouse | IRQ 12, and the 8042 is already driven for A20 |
| `cpuid` leaf 4 cache topology | `cpu.c` reports only the line size |
| Stack canaries for task stacks | a magic at the low end of each, checked on switch, would catch overflow before it corrupts the heap |

---

## Not planned

Things deliberately out of scope, so the roadmap is not read as a to-do list
of everything:

- **64-bit / long mode.** A different project. The 32-bit boot path, with its
  A20 gate and its real-to-protected transition, is the thing being
  demonstrated here.
- **A GUI.** The VGA text driver is a means, not an end.
- **Networking.** Interesting, but it is a protocol-stack project rather than
  an OS-fundamentals one.
- **Binary compatibility with anything.** No Linux syscall ABI, no POSIX.
