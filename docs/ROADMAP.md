# Roadmap

Ordered by what each item would teach, with an honest estimate of what it
takes. Items are sized assuming the existing structure, which was built with
most of these in mind.

---

## Next

### 1. Relocate the kernel to the higher half

The single most structurally significant change, and a prerequisite for real
user processes. See [MEMORY.md](MEMORY.md) for why it was deferred.

Needs: a link script using `AT()` to separate virtual from load addresses;
early page tables built in assembly before C runs; a jump to the virtual entry
point; `KERNEL_VIRT_BASE` subtracted wherever a physical address is computed
from a symbol. Both boot paths need checking, since stage 2 copies to
`p_paddr` and would need the physical addresses to stay correct while the
virtual ones move.

Roughly: a weekend, and the boot tests will earn their keep.

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

### 4. A real ELF loader for userspace

Ring 3 currently runs a payload linked into the kernel image. Loading a
separate binary needs per-process address spaces (which wants item 1 first), a
`CR3` switch in `context_switch`, and a loader that maps `PT_LOAD` segments
with `PTE_USER` — the logic stage 2 already has, applied to virtual rather
than physical addresses.

### 5. `fork` and `exec`

Follows from 1 and 4. Copy-on-write makes `fork` interesting: mark both copies
read-only, and have the page-fault handler duplicate on write. The handler is
already structured to tell a resolvable fault from a fatal one — right now it
reports every fault as fatal, with a comment saying where demand paging goes.

---

## After that

### 6. A block device and a filesystem

ATA PIO first, because it needs no DMA and no interrupts. Then FAT16 read-only,
which is enough to load an init binary and is well documented. The existing
`pci.c` already finds the IDE controller.

### 7. A slab allocator over the heap

The heap is one arena, so a long-lived small allocation can keep a large region
from coalescing. Per-size caches for the common fixed-size objects (`struct
task`, page-table wrappers) would fix the fragmentation and speed up the common
path. `heap_check()` already exists to validate the result.

### 8. APIC and the HPET

The 8259 and 8254 are legacy. The local APIC timer and the I/O APIC are what
real hardware uses, and the local APIC is a prerequisite for SMP. `cpu.c`
already detects the APIC feature bit.

### 9. SMP

Needs: ACPI MADT parsing to find the other cores, a trampoline to bring them
out of reset in real mode, per-CPU data, and — at last — `spinlock_t` becoming
a real spinlock. `kernel/core/spinlock.c` is written so that this is a change
of implementation rather than a change of every call site.

### 10. A proper VFS and a `/proc`

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
| Symbol table in the image | would let `backtrace()` print names instead of needing `addr2line` |

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
