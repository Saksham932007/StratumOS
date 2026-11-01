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

What it still lacks: `argv` and signals. File descriptors and loading the
image from a file have both since landed - see below.

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

### A network stack

Landed in v0.11.0: an Intel e1000, Ethernet, ARP, IPv4, ICMP, UDP and a small
TCP. The machine answers a ping, answers a who-has, accepts a TCP connection
and echoes on it - verified against a real host-side peer through QEMU's
`hostfwd`, which did the other half of the handshake.

The driver is descriptor rings with the one-slot-unused rule, interrupts, the
PCI bus-mastering bit, and `RCTL.SECRC` to strip the Ethernet CRC. TCP is a
passive open with a real send queue, MSS-sized segments inside the peer's
window, retransmission with a bounded retry, a half-open timeout, and resets
built from the offending segment per RFC 793.

Two things worth recording. The first working driver sent every frame with a
source MAC of zero - `struct net_device` was `const` and its `.mac` was never
filled, so the log printed the driver's own correct copy while the stack sent
frames from a second one that was empty. Transmit worked, the device's counter
confirmed it, receive was silent, and every log line was right. Only a packet
capture showed it, which is why the `network` scenario parses a pcap and
recomputes the checksums independently rather than reading the kernel's log.

The second: QEMU attaches a default e1000 unless told otherwise, so two runs
intended to exercise opposite paths both reported the same check count. Both
suite paths are now asserted by count, and there is a `no-network` scenario
with `-nic none`.

**What it deliberately does not have**, in rough order of how much it
matters - as of the work below, which closed the first, the fourth and the
fifth: out-of-order reassembly (the largest remaining gap: fast retransmit
makes the *sender* side of a single loss cheap, and the receiver still costs a
round trip per segment after the hole); IP fragment reassembly; a routing
table beyond one gateway; window scaling, SACK and timestamps; ICMP error
generation; TLS, and therefore `https`. Each has its reasoning in
[NETWORK.md](NETWORK.md).

### File descriptors, and a VFS with three backends

Landed after v0.13.0, and it was the item at the top of this list for a
reason: there was no `open`, so there was nothing a descriptor could be, so
`read` and `write` had nothing to name. Adding sockets without it would have
meant a second parallel set of calls that happened to return small integers -
which is to say a descriptor table, built twice, badly.

Two refcounted layers: a per-process table of small integers, and a global
pool of open files they point at. `dup` shares a position, `fork` duplicates
the table and not the open files, and the last close is what releases. Both
tables are fixed-size, so a ring-3 `open` loop hits EMFILE rather than the
allocator.

Three backends from the day it landed, which matters more than it sounds: an
interface with one implementation is a guess about what varies. A FAT file is
seekable and has a size, the console is neither, and a socket is a stream with
no position at all - and every NULL entry in `struct vfs_ops` is a place the
abstraction would have been wrong if it had been designed against files alone.

The ring-3 fuzzer found a real bug immediately: `user_range_ok()` did not
check writability, so a pointer into a program's own read-only `.text` passed
validation and then faulted in ring 0, where `CR0.WP` makes it a panic. It had
been latent since `wait()`'s status pointer.

### A writable filesystem, from the ATA command up

`ata_write` with the per-sector DRQ handshake and a cache flush, because a
drive acknowledges a write once it is in its write-back cache and not once it
is on the platter - and the whole difficulty of writing a filesystem is
reasoning about exactly that ordering.

FAT16 writing is crash-*ordered* rather than crash-safe, and the distinction
is the honest part. Three structures have to agree afterwards and no two can
be updated atomically, so the question is which order leaves the least damage:
the allocation table first (both copies), then the data, then the directory
entry. *The name is published last and withdrawn first.* A crash leaks
clusters, which is repairable by a scan; the reverse order double-allocates
them, which is two files silently sharing bytes.

Verified against mtools, an independent FAT implementation, and the write
tests are idempotent because six CI scenarios boot the same mutable image in
sequence - leftover files would pass once and fail thereafter.

### TCP that is not a demo, and a machine that is on the internet

Landed together, because each of three gaps was blocked on the same thing: one
connection. A sockets API needs a listener and an accepted connection at once;
an outgoing connection needs the echo port still listening; and a congestion
window on a stack that holds one connection cannot be tested against anything,
because there is no second flow to be fair to.

So: a connection table demultiplexed on the four-tuple, with handles that
carry a generation counter so a handle to a reused slot is rejected rather
than silently addressing a different peer. Then congestion control by RFC
5681 - slow start, congestion avoidance, fast retransmit on three duplicate
acknowledgements, fast recovery - and a retransmission timeout that is
measured by Jacobson's estimator with the variance term, instead of the
constant 500 ms it was. Karn's rule is why a retransmitted segment produces
no sample.

Then DHCP, DNS and an HTTP client, which are the whole distance between "the
stack works" and "the machine is on the internet". Each turned out to be the
first user of something underneath it that had never run: DHCP needed a
broadcast output path and found that UDP verified every checksum against this
machine's address rather than the datagram's own destination, so every
broadcast reply failed silently. DNS needed bound ports with a receive queue.
HTTP needed an active open, and found that `tcp_connect` reported failure
whenever the first SYN could not be transmitted - which is always, on a cold
ARP cache. A stack with a retransmission timer does not need its first
transmission to succeed, and designing as if it does is how the timer comes to
have an untested path.

And sockets, which is now a short file: `read` is a receive, `write` is a
send, `poll_read` is what makes a blocking receive work, and the three calls
with no file equivalent - bind, listen, accept - are the only ones that needed
anything new. That is the argument for having built the descriptor table
first.

Two things found along the way. A loopback path, because a controller does not
receive its own transmissions and QEMU's user-mode networking does not reflect
a frame sent to the guest's own address either: without it `ping` on one's own
address times out, and a program that connects to a server it is itself
running cannot work. It is a queue and not a direct call, so that a send from
inside a receive cannot nest.

And a receive-ring race in the e1000 driver that predated all of this.
`e1000_poll()` derives its next descriptor from RDT and publishes by writing
RDT forward, and it is called from both the interrupt handler and the poll
loop. An interrupt landing inside the loop made the interrupted copy write a
stale tail *backwards*, and a few of those wedged the ring with the head far
ahead of the tail: receive stopped while transmit carried on working. It
presented as a SYN retransmitted five times and never answered, on the third
HTTP fetch of a run and never the first two. The guard is a flag rather than a
lock, because the second entry is on the same processor and a lock would
deadlock against itself - and its counter reads 30 on a run that does four
fetches, so the race was happening constantly.

`dns example.com` resolves and `fetch http://example.com/` downloads, over a
chunked body. There is no TLS, so there is no `https`, and the refusal says
so rather than quietly trying port 443: TLS is a certificate store, a chain
validator and four primitives where being approximately right is the same as
being broken.

---

## Next

### 1. Per-CPU run queues

The scheduler runs on the boot processor and the other processors idle. What
remains is the part that makes four processors useful rather than merely
present: a run queue per processor, `current` becoming per-CPU, work stealing
when one queue empties, a reschedule IPI, and every scheduler invariant
re-examined for two processors entering it at once.

It needs the local APIC timer first - the single 8254 cannot drive four
processors - which is item 7 below, and that makes this the one item in this
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

### 6. A slab allocator over the heap

The heap is one arena, so a long-lived small allocation can keep a large region
from coalescing. Per-size caches for the common fixed-size objects (`struct
task`, page-table wrappers) would fix the fragmentation and speed up the common
path. `heap_check()` already exists to validate the result.

### 7. The I/O APIC, and the local APIC timer

The MADT's I/O APIC entries and its interrupt source overrides are already
parsed - including that IRQ 0 arrives as GSI 2 on QEMU, which a kernel that
assumed otherwise would lose its timer to. Nothing is programmed yet: device
interrupts still go through the 8259s to the boot processor, so there is no
interrupt distribution and no affinity.

The local APIC timer belongs with it, and is the prerequisite for item 1: the
single 8254 cannot drive four processors, so per-CPU preemption needs a timer
per CPU.

### 8. A `/proc`

The introspection the shell does through direct calls - `ps`, `meminfo`,
`irq`, `disk`, `mount`, `harden` - belongs behind readable files instead.
The VFS and its two real filesystems are in place, so what remains is a
third backend whose files are generated on read - which is the one thing
neither `devfs` nor the FAT driver does, and the reason this is still an item
rather than done.

### 9. A negative-lookup cache for `exec`

The one item the VFS work left open deliberately. `exec()` of a path that does
not exist does a disk lookup every time, so a ring-3 loop can make the kernel
do unbounded synchronous polled-PIO I/O. The syscall fuzzer found it - it is
most of the 600 µs average cost of a call during a fuzz run - and caching a
name's *absence* needs an invalidation story, which the write support has now
provided. It is resource exhaustion rather than a memory safety bug; see
[FUZZING.md](FUZZING.md).

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

### A second architecture

Landed in v0.12.0: StratumOS runs on RISC-V 64. Machine mode to supervisor
mode by `mret`, its own trap vectors for both, the CLINT timer, and Sv39
three-level paging - with the formatter, string layer, 64-bit division and
levelled logger linked from the *same source files* the x86 kernel uses, and
58 assertions run against them on the target.

The deliverable is the measurement rather than the boot. The claim that used
to sit in this document - that most of `kernel/core` and `kernel/mm` was free
of x86 - was a guess, and `make portability` now answers it on every push:
**55% of the kernel outside the architecture layer compiles for riscv64
unmodified**, including the entire network stack above the driver, the
filesystem, the block layer, both allocators and the formatter.

Two findings made it worth doing:

- Before any fixing, seven files failed with 88 errors and every single one
  had the same cause: `paddr_t` and `vaddr_t` were `typedef u32`. Correct on
  i386, silently truncating on anything wider, invisible to 633 passing
  assertions, and fixed by one line. A latent correctness bug in shared code
  that no amount of testing on one architecture could have surfaced.
- Exactly one failure was structural rather than arithmetic: `mm/heap.c`
  failed on its include line, because it wanted interrupt masking and was
  getting it from `<arch/io.h>` - the x86 port I/O header, on an
  architecture with no port I/O. That is now `<kernel/irqflags.h>`.

What is deliberately not ported, with a reason per file, is in
[PORTING.md](PORTING.md): the scheduler's mechanism, `mm/vmm.c` (x86's
recursive page-directory window does not generalise past two levels), ring 3,
the other harts, and the e1000 - whose reliance on cache-coherent DMA was
flagged in a comment written before this port existed.

**Follow-up this created.** The first item is done; the rest are open, in
order of how much they are worth:

1. ~~Unify the build around an `ARCH` variable.~~ Done in v0.13.0. One
   variable selects the toolchain, the flags, the source set, the linker
   script, the object tree and what `run` and `test` mean, and each
   architecture's build lives beside its code - `sources.mk` for what,
   `arch.mk` for how. It also collapsed the object-collision guard from two
   copies to one, which is what let the riscv64 port walk into the very
   collision the x86 copy existed to catch.
2. Move the remaining `<arch/io.h>` includes that only want interrupt flags
   over to `<kernel/irqflags.h>`. Eleven files include it; most legitimately
   want port I/O, and separating them is mechanical.
3. Carve `core/ktest.c` into per-subsystem files. It is one translation unit
   holding every x86 suite, which is why the riscv64 kernel has its own small
   harness instead of sharing the real one - the single largest piece of
   shared code that *should* have ported and could not.
4. A riscv64 frame allocator and VMM, which is what `mm/heap.c` and
   `mm/pmm.c` need under them before they can be linked rather than merely
   compiled.

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
