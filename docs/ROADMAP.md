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

### Symmetric multiprocessing

Landed in v0.8.0, with an explicit boundary: every processor is brought up and
runs kernel code, locks are real, processors can interrupt each other and TLB
shootdown is acknowledged — and the **scheduler still runs only on the boot
processor**. Per-CPU run queues are the large remaining piece.

ACPI's RSDP, RSDT/XSDT and MADT, every length and checksum validated; the
local APIC mapped uncached and enabled, with ExtINT on LINT0 so the 8259s keep
working; a 176-byte real-mode trampoline and INIT-SIPI-SIPI; per-CPU GDT
entries, TSSes, stacks with guard pages; `spinlock_t` as a real
test-and-test-and-set lock that masks interrupts first and is bounded; IPIs
and TLB shootdown.

Two bugs. Masking every local vector table entry — which looks correct —
stopped the timer dead, because enabling the local APIC puts the 8259s behind
LINT0 and a masked LINT0 cuts them off; the symptom was a kernel that booted
perfectly with every log line sharing one timestamp. And the compiler was
hoisting a `temp_unmap()` above the code still using the mapping, because it
cannot see that a store to a page table entry changes where a later access
*goes* — latent since the copy-on-write work, and now prevented by volatile
page-table pointers and explicit barriers.

Identifying the current processor was measured rather than assumed. Reading
the local APIC's id register costs 453 cycles; reading the task register,
which is already per-CPU because each processor needs its own TSS, costs 3.6.
That is 125x, and it is on the context-switch path. Documented in
[SMP.md](SMP.md).

### Fuzzing

Landed in v0.9.0, in two halves. Four libFuzzer targets compile the **real**
kernel sources — `elf.c`, `fat16.c`, `heap.c`, `acpi.c` — for the host under
AddressSanitizer and UndefinedBehaviorSanitizer, with a shim that `mmap`s
pages at the addresses the kernel asks for so the ELF loader is fuzzed
completely unmodified. And `user/fuzz.c` issues 40,000 deterministic malformed
system calls from ring 3, which is the only seat the pointer validation can be
attacked from.

Six bugs, all pre-existing, all in code the other four test layers covered and
passed:

| | Found by |
| --- | --- |
| `memmove` called `memcpy` with overlapping ranges (undefined behaviour, latent) | ASan's `memcpy` interceptor |
| ACPI trusted the RSDP's own `length` field and read past the structure | `fuzz_acpi` |
| the heap stopped coalescing after a shrinking `krealloc` | `heap_check()` under `fuzz_heap` |
| **any ring-3 page fault panicked the kernel** | `fuzz_elf`, via an ELF whose entry point lay outside its segments |
| the ELF loader leaked every mapped page on a mid-segment failure | the "nothing may be left mapped" assertion |
| a ring-3 process could flood the kernel console with log output | the syscall fuzzer |

Honest about what it is not: the ring-3 fuzzer has no coverage feedback,
because that needs instrumentation in the kernel and a way to export the
counters. The drivers are not fuzzed. Nothing fuzzes concurrency. Documented,
with the reasoning for each accommodation the shim makes, in
[FUZZING.md](FUZZING.md).

### 64-bit long mode

Landed in v0.10.0 as a tested transition, explicitly not a port. The boot
processor goes from 32-bit protected mode into 64-bit long mode, runs a
payload there, and comes back with the 32-bit kernel still running: a
four-level page table (PML4 -> PDPT -> PD with 2 MiB pages, one gigabyte
identity-mapped), the documented enable sequence, a 64-bit code descriptor
whose L bit is the only part doing any work, and the reverse sequence out.

Six stage flags, each set by the code that reached that point, and a payload
chosen to be *impossible* in 32-bit mode rather than merely different: a
64-bit immediate, `0xFFFFFFFF + 1` carrying past bit 31 in one instruction,
`r15`, RIP-relative addressing checked against the address the trampoline was
copied to, `EFER.LMA` read back with `rdmsr`, and a read through a 64-bit
pointer at a physical address the identity map does not cover.

Tested on both kinds of processor, because `qemu-system-i386` masks
`CPUID.80000001H:EDX.LM` even with `-cpu max`: 17 checks asserting the kernel
declines correctly there, 51 asserting the transition itself under
`qemu-system-x86_64`.

One bug worth recording. The probe frame - the strongest check - was
allocated with `pmm_alloc_frame()` and landed at 3.5 MiB, *inside* the 4 MiB
identity map, where the read would have succeeded whether the four-level walk
worked or not. The test passed and proved nothing. Fixed with
`pmm_alloc_frame_above()`, which is the general primitive memory zones exist
for. Documented in [LONGMODE.md](LONGMODE.md).

**What a port still needs**, in the order it has to happen:

1. A 64-bit IDT. Gate descriptors are 16 bytes rather than 8, and there is an
   interrupt stack table to set up. Nothing else can be developed first,
   because an interrupt-free window is not somewhere a kernel can be built.
2. The interrupt stubs rewritten for the new calling convention: arguments in
   registers, a red zone, 16-byte stack alignment.
3. `SYSCALL`/`SYSRET` with `STAR`, `LSTAR` and `SFMASK`, replacing `int 0x80`.
4. A four-level VMM. The recursive page-directory mapping this kernel uses to
   reach its own tables is specific to two levels and does not generalise, so
   that is a design decision to make again rather than port.
5. An audit of every `u32` that is really an address - `vaddr_t`, `paddr_t`,
   and everything that has ever been assigned one.

That is one commit that either boots or does not, which is why it is a
project rather than a phase.

---

## Next

### 1. Per-CPU run queues

The scheduler runs on the boot processor and the other processors idle. What
remains is the part that makes four processors useful rather than merely
present: a run queue per processor, `current` becoming per-CPU, work stealing
when one queue empties, a reschedule IPI, and every scheduler invariant
re-examined for two processors entering it at once.

It needs the local APIC timer first - the single 8254 cannot drive four
processors - which is item 8 below, and that makes this the one item in this
list whose dependencies are not already in place.

### 2. NX, and therefore real W^X

The one piece of hardening that is a project rather than a patch. A 32-bit
page table entry has no execute-disable bit, so the kernel's text is
read-only but its data is still executable as far as the hardware is
concerned.

Getting NX means PAE: 64-bit page table entries, a three-level walk through
a page-directory-pointer table, and every function in `mm/vmm.c` rewritten
around a different entry format. The recursive-window trick survives, the
software bits move from 9-11 to 9-11 and 52-62, and `CR4.PAE` has to go on
before `CR0.PG` - which means `_start` changes too.

Worth doing because it is the difference between claiming W^X and having it.
The 64-bit entry format is no longer unfamiliar ground either: the long-mode
transition builds a four-level table out of the same entries, so the layout
and the PAE-before-paging ordering are already written down and tested in
`kernel/arch/x86/longmode.c`. Everything else in [SECURITY.md](SECURITY.md#what-is-missing) is
smaller: UMIP is a CR4 bit, KASLR is relocations, and `-fstack-protector`
wants the per-CPU area the SMP work brings.

### 3. Interrupt-driven serial transmit

`console_write` currently holds interrupts off for the whole of a polled UART
write. On QEMU that is free; on real hardware at 115200 it is ~87 µs per
character and can cost timer ticks during heavy logging.

Needs: a transmit ring buffer, the THR-empty interrupt enabled in IER, and a
drain path in the existing `serial_irq`. The panic path has to keep polling,
because it cannot rely on interrupts.

### 4. Reclaim empty page tables

Unmapping the last page in a 4 MiB region leaves its table allocated. Needs a
per-table mapped-page count, decremented in `vmm_unmap`, freeing the frame and
clearing the directory entry at zero — and a `tlb` flush of the recursive
window entry for that slot.

### 5. Symbolising the profiler's call graph

Samples are attributed to the leaf function only, so a helper called from
several places aggregates all of its callers together. Walking the
frame-pointer chain at sample time would fix it, and the backtrace code
already exists - see [PERFORMANCE.md](PERFORMANCE.md).

### 6. Writing to the filesystem, and a VFS

Reading is done. Writing means allocating from the FAT, updating both copies
of it, extending a directory entry's size and cluster chain, and surviving
being interrupted between any two of those - which is where the interesting
part is, and it is a journalling discussion rather than a filesystem-format
one.

A VFS layer belongs with it rather than before it: one filesystem behind an
interface is an interface with one implementation, and the second
implementation is what shows whether the interface was right. Long filenames
and `argv` for `exec` are the two smaller gaps the current driver leaves.

A negative-lookup cache belongs here too, and it closes a known bug rather
than adding a feature: `exec()` of a path that does not exist does a disk
lookup every time, so a ring-3 loop can make the kernel do unbounded
synchronous polled-PIO I/O. The syscall fuzzer found it — it is most of the
600 µs average cost of a call during a fuzz run — and it is left open
deliberately, because caching a name's absence needs the invalidation story
that arrives with write support. It is resource exhaustion, not a memory
safety bug; see [FUZZING.md](FUZZING.md).

### 7. A slab allocator over the heap

The heap is one arena, so a long-lived small allocation can keep a large region
from coalescing. Per-size caches for the common fixed-size objects (`struct
task`, page-table wrappers) would fix the fragmentation and speed up the common
path. `heap_check()` already exists to validate the result.

### 8. The I/O APIC, and the local APIC timer

The MADT's I/O APIC entries and its interrupt source overrides are already
parsed - including that IRQ 0 arrives as GSI 2 on QEMU, which a kernel that
assumed otherwise would lose its timer to. Nothing is programmed yet: device
interrupts still go through the 8259s to the boot processor, so there is no
interrupt distribution and no affinity.

The local APIC timer belongs with it, and is the prerequisite for item 1: the
single 8254 cannot drive four processors, so per-CPU preemption needs a timer
per CPU.

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

### A 64-bit kernel

The transition itself landed in v0.10.0 and is listed under "Done" above;
what remains is the port, and the five steps it needs are enumerated there.
It is one commit that either boots or does not, which is why it is kept
separate from the phase that made it testable.

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
