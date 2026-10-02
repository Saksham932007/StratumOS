# Roadmap

Ordered by what each item would teach, with an honest estimate of what it
takes. Items are sized assuming the existing structure, which was built with
most of these in mind.

---

## Done since v0.4.0

### Per-process address spaces, fork, copy-on-write, exec, wait

Landed in v0.5.0. A page directory per process with the kernel's half
copied in, a `CR3` switch in `switch_to()`, `fork` with copy-on-write
(software PTE bit 10, per-frame reference counts in the PMM, a fault
handler with a no-copy shortcut for the last sharer), `exec` by rewriting
the syscall's own trap frame, and `wait` with real zombies. Two ring-3
programs are embedded so `exec` has a different image to switch to.

Two new in-kernel suites (`vmspace`, `proc`, 56 assertions between them)
and fifteen new CI expectations. Documented in
[PROCESSES.md](PROCESSES.md).

What it still lacks: `argv`, file descriptors, signals, and loading the
image from a file rather than the embedded table — the last of which the
filesystem work below has since fixed.

### Hardening

Landed in v0.6.0. The kernel's own `.text` and `.rodata` are mapped
read-only and `CR0.WP` makes that binding on ring 0; SMEP and SMAP are
detected, enabled and read back from CR4, with `stac`/`clac` around each of
the five places the kernel deliberately touches user memory; every task
stack moved out of the heap into a region where each one sits above an
unmapped guard page, with a canary checked on every context switch.

Enabling SMAP found an undeclared user access in the `vmspace` suite on its
first boot, which is the feature working.

It also fixed a latent bug that had nothing to do with security: a kernel
page table created after the first `fork` would have been missing from every
existing address space, which the growing heap would eventually have hit.
All 255 kernel directory slots are now backed at `vmm_init()`, and creating
one later is a panic.

A new `harden` suite (24 checks) asserts the configuration, and two CI
scenarios assert the enforcement by requiring a panic — the half of a
mitigation that a passing test cannot check. Documented in
[SECURITY.md](SECURITY.md).

### A disk, a partition table and a filesystem

Landed in v0.7.0. An ATA PIO driver (IDENTIFY, LBA28 and LBA48, bounded
waits, ATAPI recognised and skipped), an MBR partition-table parser reading
the same sector stage 1 boots from, and a read-only FAT16 driver. `/bin/INIT`
is read off the disk before ring 3 is entered, and `exec` resolves names
against the filesystem, falling back to the kernel's embedded copies on the
GRUB ISO path where there is no partition at all.

`tools/mkfat.py` writes the filesystem image rather than driving mtools, so
its layout is chosen deliberately and the test suite can assert on specific
clusters. `tools/check-image.py` validates every built image: the boot
signature, stage 1 not overlapping its own partition table, the stage 2
header pointing at a real ELF, the FAT geometry, and every file in `/BIN`
being an i386 ELF with `INIT` among them.

Two new suites (`storage`, `fs`, 100 assertions between them) and a logging
facility that counts deliberately-provoked errors instead of printing them,
so a refusal that happens *silently* fails a test. The `fs` suite also found
a real design flaw: the FAT driver's single-sector cache thrashed between
allocation-table and data sectors, halving its hit rate. Splitting it into
two slots by purpose took the hit rate from 47% to 89%. Documented in
[STORAGE.md](STORAGE.md).

---

## Next

### 1. NX, and therefore real W^X

The one piece of hardening that is a project rather than a patch. A 32-bit
page table entry has no execute-disable bit, so the kernel's text is
read-only but its data is still executable as far as the hardware is
concerned.

Getting NX means PAE: 64-bit page table entries, a three-level walk through
a page-directory-pointer table, and every function in `mm/vmm.c` rewritten
around a different entry format. The recursive-window trick survives, the
software bits move from 9-11 to 9-11 and 52-62, and `CR4.PAE` has to go on
before `CR0.PG` - which means `_start` changes too.

Worth doing because it is the difference between claiming W^X and having it,
and because it is the natural rehearsal for the 4-level paging that long mode
needs. Everything else in [SECURITY.md](SECURITY.md#what-is-missing) is
smaller: UMIP is a CR4 bit, KASLR is relocations, and `-fstack-protector`
wants the per-CPU area the SMP work brings.

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

### 5. Writing to the filesystem, and a VFS

Reading is done. Writing means allocating from the FAT, updating both copies
of it, extending a directory entry's size and cluster chain, and surviving
being interrupted between any two of those - which is where the interesting
part is, and it is a journalling discussion rather than a filesystem-format
one.

A VFS layer belongs with it rather than before it: one filesystem behind an
interface is an interface with one implementation, and the second
implementation is what shows whether the interface was right. Long filenames
and `argv` for `exec` are the two smaller gaps the current driver leaves.

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

### 9. A `/proc`

The introspection the shell does through direct calls - `ps`, `meminfo`,
`irq`, `disk`, `mount`, `harden` - belongs behind readable files instead.
That needs a VFS with at least two filesystems in it (item 5) and write
support for the synthetic one, so it follows them rather than preceding
them.

---

## Smaller items

| Item | Notes |
| --- | --- |
| Tab completion in the shell | the command table is already the single source of truth |
| ANSI colour over serial | the console layer would need per-sink escape handling |
| `kmalloc` call-site tracking | a `__builtin_return_address(0)` in the block header would make leaks attributable |
| A watchdog on the NMI | `nmi_handler` currently only warns |
| UMIP (CR4 bit 11) | stops ring 3 reading descriptor-table bases with `sgdt`/`sidt`; one bit and a CPUID check |
| Framebuffer support | the Multiboot2 framebuffer tag is already requested but ignored |
| PS/2 mouse | IRQ 12, and the 8042 is already driven for A20 |
| `cpuid` leaf 4 cache topology | `cpu.c` reports only the line size |

---

## Further out

These are bigger than the items above, and each one is a project in its own
right rather than a weekend:

### x86-64 long mode

The natural continuation of the boot story this project is about: 16-bit
real mode to 32-bit protected mode to 64-bit long mode, with 4-level
paging. It is a second architecture port of the whole kernel, not a flag.
Mentioned here because the boot path is the part of this project most worth
extending, and the existing two-stage loader already does the hard half
(A20, E820, a protected-mode GDT, ELF program headers).

### A network stack

An e1000 driver, then ARP, IP, UDP and a minimal TCP. It is a
protocol-stack project rather than an OS-fundamentals one, which is why it
is last, but it is also the one that exercises interrupt latency, DMA and
buffer lifetime management in a way nothing else here does.

### A second architecture

RISC-V or AArch64. Most of `kernel/core` and `kernel/mm` is already free of
x86, and `kernel/arch/x86` is where everything that is not would have to
grow a sibling. The value is in finding out how much of that claim is true.

---

## Not planned

Things deliberately out of scope, so the roadmap is not read as a to-do list
of everything:

- **A GUI.** The VGA text driver is a means, not an end.
- **Binary compatibility with anything.** No Linux syscall ABI, no POSIX.
  The syscall surface is small and deliberately its own.
- **A port to a machine without a BIOS.** UEFI boot is a different loader
  for a different firmware, and the BIOS path is the thing being
  demonstrated.
