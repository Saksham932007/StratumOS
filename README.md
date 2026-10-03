# StratumOS

A 32-bit x86 kernel and bootloader, written from scratch in C and assembly.
It takes a machine from the BIOS's first instruction in 16-bit real mode to a
preemptively scheduled, higher-half, paged kernel with per-process address
spaces, copy-on-write `fork`, `exec` from a FAT16 filesystem on disk, and a
hardened kernel/user boundary enforced by SMEP, SMAP and `CR0.WP`.

[![CI](https://github.com/Saksham932007/StratumOS/actions/workflows/ci.yml/badge.svg)](https://github.com/Saksham932007/StratumOS/actions/workflows/ci.yml)
![language](https://img.shields.io/badge/C11%20%2B%20NASM-22.4k%20lines-blue)
![arch](https://img.shields.io/badge/arch-x86%20(i686)-lightgrey)
![license](https://img.shields.io/badge/license-MIT-green)

The same kernel binary boots two ways — through a bootloader written for this
project, and through GRUB via Multiboot2 — and every push runs 576 assertions
across 92 host unit tests, 19 in-kernel suites, and eight QEMU boot scenarios.
Two of those scenarios are required to **panic**, because a mitigation has two
halves and only one of them can be checked by a test that passes.

```
BIOS ─► stage 1 ─────► stage 2 ─────► protected mode ─► kernel ──────► /bin/INIT ─► ring 3
        512 B MBR      A20 gate       flat GDT          higher half    ATA PIO      fork
        LBA + CHS      E820 map       CR0.PE            @0xC0000000    MBR table    COW
        retry/verify   32 KiB reads   ELF32 loader      paging, heap   FAT16        exec
        + partition                                     SMEP/SMAP                   wait
          table                                         W^X                       int 0x80

               GRUB ─► Multiboot2 ─────────────────────► (same kernel binary)
```

---

## Contents

- [The parts worth looking at](#the-parts-worth-looking-at)
- [What this actually does](#what-this-actually-does)
- [Quick start](#quick-start)
- [What it looks like running](#what-it-looks-like-running)
- [Feature matrix](#feature-matrix)
- [Two boot paths, one kernel](#two-boot-paths-one-kernel)
- [Performance](#performance)
- [Two architectures](#two-architectures)
- [Networking](#networking)
- [64-bit long mode](#64-bit-long-mode)
- [Testing](#testing)
- [Fuzzing](#fuzzing)
- [Repository layout](#repository-layout)
- [Design decisions worth defending](#design-decisions-worth-defending)
- [Bugs this project found and fixed](#bugs-this-project-found-and-fixed)
- [What is deliberately not here](#what-is-deliberately-not-here)
- [Documentation](#documentation)

---

## The parts worth looking at

Nine things in here were harder than they look, and each has a document that
explains the reasoning rather than the code.

**One binary, two boot protocols.** `build/stratum.elf` is a single file.
GRUB finds a Multiboot2 header in it; the bootloader in `boot/` parses its ELF
program headers and copies the segments itself. The kernel works out which one
loaded it from the magic in `EAX`, and CI boots both paths on every push — so
a regression that silently falls back to one is caught.
→ [docs/BOOT.md](docs/BOOT.md)

**Copy-on-write that is proved, not claimed.** `fork` marks every writable
page read-only in *both* parent and child and bumps a per-frame reference
count. Marking only the child is the subtle wrong answer, and its symptom is a
parent whose data is occasionally, quietly wrong. The ring-3 program writes to
an inherited page and the parent checks its own copy is intact, which is the
only externally visible difference between correct copy-on-write and a shared
page.
→ [docs/PROCESSES.md](docs/PROCESSES.md)

**`exec` returns by rewriting its own trap frame.** It cannot return normally:
the code that issued `int 0x80` was in the image it just unmapped. So it
edits `EIP` and `ESP` in the frame the syscall is about to `IRET` through, and
the ordinary interrupt-return path delivers control into the new program.
→ [docs/PROCESSES.md](docs/PROCESSES.md#exec)

**SMAP turned "anywhere" into a list of five.** Enabling it forced every place
the kernel deliberately touches user memory to declare itself with
`stac`/`clac`. On its first boot it faulted the test suite — which writes to a
user page from ring 0 to prove copy-on-write works and had never declared the
access. A mitigation that finds a defect in the same change that introduces it
has earned its two instructions.
→ [docs/SECURITY.md](docs/SECURITY.md#smep-and-smap)

**A cache decision made by measurement, against my own reasoning.** The FAT
driver shipped with one cached sector and a comment explaining that a second
would buy nothing. The test suite reads a file in 64-byte chunks — eight
touches per sector, so seven of eight should hit — and fewer than half did,
because FAT sectors and data sectors were evicting each other. Splitting the
cache by purpose took the hit rate from 47% to 89%.
→ [docs/STORAGE.md](docs/STORAGE.md#the-cache-and-what-measuring-it-changed)

**A second architecture, which found a latent bug in the first.** The kernel
also runs on RISC-V 64 — machine mode to supervisor mode to Sv39 paging, with
the formatter, string layer and logger linked from the *same source files*
the x86 kernel uses. The port's real output is a number: **55% of the kernel
outside the architecture layer compiles for RISC-V unmodified**, and 88 of
the 89 failures had one root cause — `paddr_t` and `vaddr_t` were `u32`.
Invisible on i386, passes all 633 assertions, and only findable by building
for something wider.
→ [docs/PORTING.md](docs/PORTING.md)

**A network stack verified against a packet capture.** An e1000 driver,
Ethernet, ARP, IPv4, ICMP, UDP and a small TCP — the machine answers a ping
and accepts a TCP connection. The first working driver sent every frame with
a source MAC of zero: the log printed the right address because it printed
the driver's copy, while the stack sent frames built from a second copy that
was never filled. Transmit worked, receive was silent, every log line was
correct. Only a pcap showed it, which is why CI now parses one and
recomputes the checksums independently.
→ [docs/NETWORK.md](docs/NETWORK.md)

**16-bit to 32-bit to 64-bit, and back.** The boot processor goes from
protected mode into 64-bit long mode on a four-level page table and returns
with the 32-bit kernel still running. The payload is chosen to be
*impossible* in 32-bit mode rather than merely different — a 64-bit
immediate, `0xFFFFFFFF + 1` carrying past bit 31 in one instruction,
RIP-relative addressing, `EFER.LMA` read back from the processor — and the
strongest check reads through a 64-bit pointer at an address the identity map
does not cover, so only the four-level walk can resolve it. It is a tested
transition, deliberately not a half-finished port.
→ [docs/LONGMODE.md](docs/LONGMODE.md)

**Fuzzing the real kernel sources, not a copy of them.** `build/fuzz/fuzz_elf`
links `kernel/core/elf.c` — byte for byte the file that boots — against a shim
whose `vmm_alloc_at()` is `mmap(MAP_FIXED_NOREPLACE)` *at the address the
kernel asked for*, because the loader maps a page at an address an untrusted
header chose and then writes through it as a raw pointer. So the loader is
fuzzed unmodified, and a write past what it mapped takes `SIGSEGV` exactly as
it would in the kernel. Six pre-existing bugs, including one that let any
unprivileged process panic the machine with a malformed program file.
→ [docs/FUZZING.md](docs/FUZZING.md)

And one piece of test design that the rest depends on: **two CI scenarios
whose expected result is a panic.** The `harden` suite proves the kernel's
`.text` has no write bit in its page table entry; it cannot prove the CPU acts
on that, because the correct outcome of trying is a dead kernel. So
`fault-text` and `fault-stackguard` each boot a kernel, type one command, and
require the panic to name the right address, reason and region — and require
QEMU to exit 35.
→ [docs/TESTING.md](docs/TESTING.md#scenarios-that-must-panic)

---

## What this actually does

Concretely, from power-on:

1. **The BIOS** loads one 512-byte sector and jumps to it.
2. **Stage 1** probes for INT 13h extensions, reads stage 2 off the disk by LBA
   (falling back to CHS with retries and a controller reset), verifies a magic
   number, and hands over.
3. **Stage 2** enables the A20 gate — trying the BIOS, the 8042 controller and
   the fast port in turn, verifying each with a memory-wraparound test — reads
   the firmware memory map with INT 15h/E820, streams the kernel off disk in
   32 KiB chunks, loads a flat GDT, sets `CR0.PE`, and then, in 32-bit code,
   **parses the kernel's ELF program headers** and copies each segment to its
   physical address.
4. **`_start`** builds a page directory, enables paging, and jumps from 1 MiB
   to `0xC0100000` — the kernel is *loaded* low and *linked* high, so the top
   gigabyte of every address space is the kernel's and the bottom three are
   free for user processes.
5. **The kernel** detects which protocol loaded it from the magic in `EAX`,
   normalises the memory map, installs 256 interrupt vectors, remaps the PIC
   off the CPU's exception vectors, brings up the timer and keyboard, *then*
   enables interrupts, builds a physical frame allocator from the firmware
   map, widens its linear map and drops the boot identity mapping, creates a
   guarded kernel heap, enumerates PCI, and starts a round-robin scheduler.
6. **The disk** is probed with ATA IDENTIFY, its MBR partition table parsed
   out of the same sector stage 1 booted from, and the FAT16 partition that
   follows the kernel on that disk is mounted at `/`.
7. **A user program** — a genuinely separate ELF, linked for user space — is
   read from `/bin/INIT` on that filesystem, given an address space of its
   own, and entered at ring 3, where it probes the syscall boundary from the
   untrusted side, `fork`s, proves copy-on-write from inside the child,
   `wait`s for it, and then `exec`s a different image into a second child.
8. **Every other processor** is found through ACPI's MADT and taken out of
   reset with INIT-SIPI-SIPI, each one repeating the 16-bit → 32-bit → paging
   → higher-half journey in a 176-byte trampoline before landing in C with
   its own GDT entry, TSS, stack and local APIC.
9. **The shell** runs as a scheduled task, reachable from the VGA console or
   over a serial line, with line editing and command history.

Along the way the kernel narrows its own permissions: its `.text` and
`.rodata` become read-only, SMEP and SMAP are enabled where the CPU has them,
and every task stack gets an unmapped guard page below it. `harden` reports
what is actually switched on, read back from CR4 and the page tables.

Every subsystem listed above is implemented and tested rather than announced.

---

## Quick start

```bash
# Ubuntu / Debian
sudo apt install build-essential nasm gcc-multilib libc6-dev-i386 \
                 qemu-system-x86 grub-pc-bin grub-common xorriso mtools

git clone https://github.com/Saksham932007/StratumOS.git
cd StratumOS

make              # kernel, disk image, GRUB ISO, and the test images
make run          # boot through the custom bootloader
make run-iso      # boot through GRUB / Multiboot2
make test         # host unit tests + seven QEMU boot scenarios
```

`make toolchain` reports exactly which tools were found. An `i686-elf-gcc`
cross-compiler is used automatically if one is on `PATH`; otherwise the host
GCC is driven with `-m32`, which works on any x86-64 Linux with multilib.

Other useful targets:

| Target | What it does |
| --- | --- |
| `make run-serial` | boot headless, serial on stdio — the shell works over it |
| `make debug` | start QEMU stopped, waiting for GDB on `:1234` |
| `make gdb` | attach GDB with symbols, break at `kmain` |
| `make sections` | dump the image layout and re-run the pre-boot validator |
| `make bench` | run the microbenchmarks and a profile, then exit |
| `make test-host` | host unit tests only (no emulator, ~1 second) |
| `make fs` | rebuild the FAT16 filesystem image and describe its layout |
| `make image-check` | validate a built disk image: MBR, BPB, `/BIN` contents |
| `make lines` | line counts by subsystem |

---

## What it looks like running

Boot, over serial, through the custom bootloader:

```
StratumOS stage1
StratumOS stage2
  [ok] A20 gate
  [ok] BIOS memory map
  [..] kernel ...
  [ok] kernel image staged
  [->] entering protected mode

  .-----------------------------------------------------.
  | StratumOS 0.12.0  -  x86 kernel: real mode to ring 3 |
  '-----------------------------------------------------'
[    0.000] INFO  boot: serial COM1        [ok] 115200 8N1
[    0.000] INFO  boot: CPU detect         [ok] GenuineIntel
[    0.000] INFO  boot: boot protocol      [ok] StratumOS native (ELF handoff)
[    0.000] INFO  boot: GDT + TSS          [ok] flat 4 GiB, rings 0 and 3
[    0.000] INFO  boot: IDT                [ok] 256 vectors
[    0.000] INFO  boot: PIC remap          [ok] IRQ 0-15 -> vector 32-47
[    0.000] INFO  timer: PIT at 100 Hz requested, 99.998 Hz actual (divisor 11932)
[    0.010] INFO  boot: interrupts         [ok] enabled
[    0.010] INFO  pmm: 32736 frames total (127 MiB), 32417 free (126 MiB), metadata 35 KiB at phys 0x00136000
[    0.030] INFO  vmm: paging: kernel at 0xc0000000, linear map 16 MiB, identity map dropped
[    0.030] INFO  vmm: kernel half fully backed: 255 page tables (1020 KiB), so every address space sees identical kernel mappings
[    0.030] INFO  heap: kernel heap at 0xd0000000, 1024 KiB committed, 64 MiB maximum
[    0.040] INFO  vmm: kernel .text and .rodata mapped read-only (43 pages); CR0.WP makes that binding on ring 0 too
[    0.050] INFO  harden: SMEP enabled, SMAP enabled
[    0.050] INFO  boot: hardening          [ok] W^X, guard pages, SMEP + SMAP
[    0.040] INFO  pci: 6 PCI devices found
[    0.040] INFO  acpi: MADT: 4 processor(s), 1 I/O APIC(s), 5 interrupt override(s), local APIC at 0xfee00000, 8259 PICs present
[    0.050] INFO  apic: local APIC at 0xfee00000 -> 0xce003000, id 0, version 14, 6 LVT entries
[    0.050] INFO  smp: 4 of 4 processor(s) online
[    0.060] INFO  ata: hd0: QEMU HARDDISK, 36864 sectors (18 MiB), LBA48
[    0.070] INFO  blk: hd0p1: type 0e (FAT), LBA 2048 + 32768 sectors (16384 KiB)
[    0.070] INFO  fat: mounted hd0p1: FAT16 "STRATUM", 16384 KiB, 8167 clusters of 2 KiB
[    0.070] INFO  boot: filesystem         [ok] FAT16 "STRATUM" on hd0p1
[    0.070] INFO  sched: scheduler ready; boot context adopted as pid 0 (idle)
[    0.070] INFO  syscall: syscall gate installed at int 0x80 (11 calls available)
[    0.080] INFO  boot: StratumOS 0.12.0 is up: 127 MiB RAM, 6 PCI devices, 19 test suites
```

Ring 3, exercising the syscall boundary from the untrusted side, then
forking, copying on write, and `exec`ing a different image:

```
[    0.080] INFO  user: pid 4 entering ring 3 at 0x004000f0 in its own address space (7 user pages mapped, image from the filesystem)
  [ring3] hello from user mode - privilege level 3
  [ring3] getpid() returned 4
  [ring3] slept 50 ms via syscall
  [ring3] asking the kernel to read a kernel address on my behalf
[    0.120] WARN  syscall: pid 4 passed an unreadable buffer 0xc0100000+16 to write()
  [ring3] kernel refused it (EFAULT) - the pointer check works
  [ring3] unknown syscall correctly rejected
  [ring3] fork(): duplicating this process
[    0.130] INFO  sched: fork: pid 4 -> pid 6, address space 0x0024c000
  [ring3] fork() returned 6 here - one call, two return values
    [child] fork() returned 0 here; my pid is 6, my parent is 4
    [child] I inherited 0x5a5a5a5a and am about to write over it
    [child] my copy now reads 0x1234abcd
    [child] exiting with 7
  [ring3] wait() collected pid 6 with exit code 7
  [ring3] my own copy still reads 0x5a5a5a5a - copy-on-write gave the child a private page
  [ring3] exec(): forking a child to replace its own image
    [child] exec("nonexistent") failed cleanly and I am still here
[    0.160] INFO  user: pid 7 exec("hello") from the filesystem: replacing 7 user pages
[    0.140] INFO  user: pid 7 now running "hello" at 0x00400000, 6 user pages
  [exec] hello: a different image, running in the same process
  [exec] getpid() returned 7 - the pid survived exec, the image did not
  [ring3] the exec'd child (pid 7) exited with 0
  [ring3] calling exit(0)
```

That child writing `0x1234abcd` over a page it inherited, while the parent's
copy of the same address still reads `0x5a5a5a5a`, is the only externally
visible difference between copy-on-write done correctly and a kernel that
simply shared the page. And `getpid()` returning 7 *after* the `exec` is the
proof that a process survived having its image replaced.

The shell:

```
stratum> meminfo
Physical memory
  total     : 127 MiB (32736 frames of 4096 B)
  in use    : 2 MiB (578 frames)
  free      : 125 MiB (32158 frames)
Virtual memory
  paging    : enabled
  kernel at : 0xc0000000
  linear map: 0xc0000000 - 0xc1000000 (16 MiB of physical memory)
  tables    : 6 page tables, 4352 pages mapped
  faults    : 4 total
  spaces    : 3 address spaces created
  COW       : 4 faults, 3 needed a copy, 1 resolved by dropping the last sharer
  shared    : 0 frames held by more than one address space
Kernel heap
  window    : 0xd0000000 (+1 MiB committed, max 64 MiB)
  in use    : 32 KiB across 4 blocks (2 free)
  integrity : consistent
Firmware memory map (6 regions)
  [ 0] 00000000 - 0009fbff  usable             639 KiB
  [ 3] 00100000 - 07fdffff  usable             126 MiB
  ...

stratum> ps
   PID  PPID  NAME           STATE     RING VMSPACE      TICKS  SWITCH
     0     0  idle           ready     ring0 kernel          36       3
     1     0  statusd        sleeping  ring0 kernel           0       6
     2     0  shell          running   ring0 kernel         104       7
     3     2  init           zombie    ring3 -                2       4
  4 tasks, 22 context switches total

stratum> cpus
Processors (4 online of 4 reported)
   CPU  APIC  ROLE STATE          STACK    PINGS      TLB   IDLE LOOPS
     0     0   bsp online    0x00000000        0        0            0  <- this one
     1     1    ap online    0xe800a000        0        0            1
     2     2    ap online    0xe800f000        0        0            1
     3     3    ap online    0xe8014000        0        0            1
  The scheduler runs on cpu 0 only; the others service interrupts. See docs/SMP.md.

stratum> ipi
broadcasting a ping IPI to every other processor...
  cpu 1: 0 -> 1 ping(s)
  cpu 2: 0 -> 1 ping(s)
  cpu 3: 0 -> 1 ping(s)
3 of 3 other processor(s) answered

now a TLB shootdown, which waits for every processor to acknowledge:
  1 shootdown(s) sent, 3 served (was 0)

stratum> disk
ATA drives
  DEV  MODEL                           SECTORS    ADDR
  hd0  QEMU HARDDISK                     36864   LBA48
  2 read(s), 2 sector(s), 2 command(s), 0 error(s), 0 timeout(s)
Block devices
  NAME     TYPE        FIRST LBA      SECTORS
  hd0      disk                0        36864
  hd0p1    0e               2048        32768

stratum> mount
FAT16 "STRATUM" on hd0p1, mounted at /
  geometry  : 512-byte sectors, 4 per cluster (2 KiB clusters)
  layout    : 1 reserved, FAT at +1 (2 x 32 sectors), root at +65 (512 entries), data at +97
  size      : 32768 sectors, 8167 clusters
  activity  : 52 sector read(s), 30 cache hit(s), 8 lookup(s), 2 file(s) read whole

stratum> ls /bin
  NAME               SIZE  ATTR
  .                     0  d---
  ..                    0  d---
  HELLO              8752  ----
  INIT              12524  ----
  2 file(s), 21276 bytes; 2 director(y|ies)

stratum> cat /etc/MOTD.TXT
console=vga,serial
init=/bin/INIT

stratum> programs
Programs embedded in the kernel image (exec's namespace):
  init   (started at boot)
  hello
Programs on the filesystem (what exec() prefers):
  /bin/HELLO            8752 bytes
  /bin/INIT            12524 bytes
  1 exec() call(s) this boot; 2 image(s) loaded from disk, 0 from the kernel image

stratum> harden
Kernel/user separation
  null page       : unmapped
  CR0.WP          : set - ring 0 honours read-only pages
  kernel .text    : read-only
  kernel .rodata  : read-only
  SMEP (CR4.20)   : enabled - ring 0 cannot execute user pages
  SMAP (CR4.21)   : enabled - ring 0 cannot touch user pages without EFLAGS.AC
Stacks
  task stacks at  : 0xe0000000, 16 KiB each
  guard pages     : one unmapped page below every stack
  canaries        : checked on every context switch (0 failures)
  this task's guard page at 0xe000a000 is unmapped, as it should be

stratum> irq
  IRQ  HANDLER               COUNT
    0  pit                     161
    1  ps2-keyboard              0
    4  16550-uart                3
  spurious: 0    syscalls: 0
```

Diagnostics are a feature, not an afterthought. `fault null` on purpose:

```
stratum> fault null
  faulting address: 0x00000000
  access          : write from ring 0
  reason          : nothing is mapped at that address
  region          : the null page - almost certainly a NULL dereference

================================================================
 KERNEL PANIC - StratumOS 0.3.0
================================================================
 page fault at 0x00000000 in the null page
  EAX 00000000  EBX 00120466  ECX 0000000a  EDX 0000002d
  EIP 0010c760  CS  0008      EFLAGS 00000286
  vector 14 (Page Fault)  error 00000002
  CR0 80010011  CR2 00000000  CR3 00123000  CR4 00000000
Call trace (return addresses; the faulting frame is EIP above):
  [0] 0x0010dbb6     <- addr2line resolves these to
  [1] 0x0010e157        shell_run_line / shell_task /
  [2] 0x00101b02        thread_trampoline
```

---

## Feature matrix

Everything marked ✅ is implemented and covered by a test.

### Bootloader

| | |
| --- | --- |
| ✅ | Two-stage design, stage 1 fitting in 315 of the MBR's 510 usable bytes |
| ✅ | INT 13h extended (LBA) reads, with capability probing |
| ✅ | True LBA→CHS translation fallback, one sector at a time |
| ✅ | Read retries with controller reset between attempts |
| ✅ | A20 gate via BIOS / 8042 / fast port, each verified by a wraparound test |
| ✅ | E820 memory map, with E801 and AH=88h fallbacks |
| ✅ | ACPI 3.0 extended-attribute handling, zero-length entry filtering |
| ✅ | 16→32 bit protected-mode transition with a flat 4 GiB GDT |
| ✅ | ELF32 program-header loader, including `.bss` zeroing from `p_memsz` |
| ✅ | Direct 16550 output, so the whole boot is visible headlessly |
| ✅ | Build-time size assertions — overflowing a sector is a build error |

### Kernel

| | |
| --- | --- |
| ✅ | Multiboot2 and native boot protocols behind one normalised interface |
| ✅ | Higher-half: loaded at 1 MiB, linked at `0xC0100000`, via `AT()` |
| ✅ | Early page tables in assembly, then an absolute jump into the higher half |
| ✅ | Linear map of low physical memory; boot identity map dropped |
| ✅ | Flat GDT with ring-0/ring-3 descriptors, plus a TSS |
| ✅ | All 256 IDT vectors filled from a generated stub table |
| ✅ | Full register dump, page-fault error decoding, EBP backtrace |
| ✅ | 8259 PIC remapping, per-line masking, spurious-interrupt detection |
| ✅ | Bitmap physical frame allocator, built from the firmware map |
| ✅ | Paging: 4 KiB pages, recursive page directory, `CR0.WP`, unmapped null page |
| ✅ | `map` / `unmap` / `protect` / `translate`, with frame-ownership tracking |
| ✅ | Kernel heap: first-fit, coalescing, guard magics, grows on demand |
| ✅ | Preemptive round-robin scheduler, per-task stacks, sleep/yield/exit |
| ✅ | **A page directory per process**, kernel half shared, `CR3` switched on context switch |
| ✅ | **`fork` with copy-on-write** — software PTE bit, per-frame reference counts, no copy for the last sharer |
| ✅ | **`exec`** by rewriting the syscall's own trap frame, so the return lands in the new image |
| ✅ | **`wait` and real zombies** — resources released by either the reaper or the parent, exit status held until collected |
| ✅ | Ring 3 via a forged IRET frame; `int 0x80` with pointer validation |
| ✅ | Two real user programs: separate ELFs, linked for user space, own pages |
| ✅ | Defensive ELF32 loader — no segment may reach into kernel space |
| ✅ | **W^X for the kernel's own image** — `.text`/`.rodata` read-only, enforced by `CR0.WP` |
| ✅ | **SMEP and SMAP**, detected and read back from CR4, with `stac`/`clac` around every deliberate kernel access to user memory |
| ✅ | **Guard pages** below every kernel stack, plus a canary checked on every switch |
| ✅ | Every address space sees identical kernel mappings, by construction |
| ✅ | **ATA PIO disk driver** — IDENTIFY, LBA28 and LBA48, bounded waits, ATAPI recognised and skipped |
| ✅ | **MBR partition table** parsed from the same sector stage 1 boots from |
| ✅ | **Read-only FAT16** — BPB validation, cluster chains, subdirectories, 8.3 names, a two-slot sector cache |
| ✅ | **`/bin/INIT` is read off the disk** before ring 3 is entered; the embedded copies are the fallback for the ISO boot path |
| ✅ | **ACPI**: RSDP, RSDT/XSDT and the MADT, every length and checksum validated |
| ✅ | **Local APIC** mapped uncached, enabled, with ExtINT on LINT0 so the 8259s keep working |
| ✅ | **Every processor brought up** — 176-byte real-mode trampoline, INIT-SIPI-SIPI, per-CPU GDT entry, TSS, stack and guard page |
| ✅ | **Real spinlocks** — test-and-test-and-set, `PAUSE`, bounded, interrupts masked first, contention counted |
| ✅ | **IPIs and TLB shootdown**, acknowledged by every processor before the sender continues |
| ✅ | Drivers: 16550 (in and out), VGA text, PIT, PS/2 keyboard, CMOS RTC, PCI |
| ✅ | `kprintf` with width/precision/64-bit support, levelled logging |
| ✅ | 64-bit division helpers — the kernel links against nothing at all |
| ✅ | 31-command shell with line editing, history, and fault injection |
| ✅ | Embedded symbol table: panics print `function+0x1c`, no addr2line needed |
| ✅ | 14 TSC-calibrated microbenchmarks, overhead-subtracted, median of 24 |
| ✅ | Timer-driven sampling profiler with symbol attribution |

---

## Two boot paths, one kernel

This is the part of the project worth the most scrutiny, because it is where
the two source projects genuinely merge rather than merely sit side by side.

`build/stratum.elf` is one file. GRUB finds a Multiboot2 header in it and loads
it. Stage 2, which has never heard of Multiboot, reads the same file off a raw
disk, walks its ELF program headers, and copies the same segments to the same
addresses. Both then jump to `_start` with a protocol magic in `EAX` and an
information pointer in `EBX`.

```c
/* kernel/core/bootinfo.c - the only file that knows the difference */
switch (magic) {
case STRATUM_BOOT_MAGIC:          /* 0x53545241, our stage 2  */
    ok = parse_stratum(info_addr);
    break;
case MULTIBOOT2_BOOTLOADER_MAGIC: /* 0x36d76289, GRUB         */
    ok = parse_multiboot2(info_addr);
    break;
}
```

Everything downstream reads one `struct boot_params`. Adding a third protocol
means adding one parser and changing nothing else. The CI boots both paths on
every push, so neither can rot while the other is being worked on.

See [docs/BOOT.md](docs/BOOT.md) for the full walkthrough, including the exact
memory layout at each stage and why the A20 gate still matters.

---

## Performance

The kernel measures itself. `make bench` calibrates the TSC against the PIT,
runs eleven microbenchmarks, takes a sampling profile, and exits — and CI
asserts on every figure.

Measured under QEMU TCG at a calibrated 2100 MHz, median of 24 samples, with
the harness's own 2306-cycle overhead subtracted:

| Benchmark | Cycles | Time | |
|---|---:|---:|---|
| virtual → physical translation | 55 | 26.3 ns | two loads through the recursive window |
| heap integrity walk | 58 | 27.8 ns | every block's header and footer magic |
| context switch | 654 | 311.3 ns | `CR3` swap, stack swap, canary check, bookkeeping |
| physical frame alloc + free | 733 | 349.2 ns | bitmap scan, 32 frames per word |
| `kmalloc(64)` + `kfree` | 962 | 458.0 ns | first-fit, split, coalesce, guards |
| map + unmap a 4 KiB page | 1054 | 502.1 ns | includes `invlpg` |
| `int 0x80` round trip | 1488 | 708.6 ns | heavily emulation-distorted |
| `ksnprintf` | 2666 | 1269.3 ns | width, precision and 64-bit division |
| `memset` 4 KiB | 4977 | 2369.8 ns | dword path |
| `memcpy` 4 KiB | 6767 | 3222.2 ns | 1.65 cycles/byte, dword path |

**These are emulated costs, not silicon timings** — and the harness says so
itself, detecting the hypervisor via CPUID and printing the caveat with every
run. The *ratios* hold; the absolute numbers need KVM or real hardware. A
benchmark that quietly reported QEMU's timings as hardware numbers would be
worse than no benchmark.

The sampling profiler hooks the timer interrupt, which already has the
interrupted `EIP` in its frame, and attributes it through an embedded symbol
table:

```
stratum> profile run 1500

  SAMPLES   SHARE  FUNCTION
       32   45.7%  kmalloc
       24   34.2%  kfree
       14   20.0%  emit_number      <- the formatter's digit loop
```

That workload was allocator traffic plus integer formatting, and the profile
finds exactly those three — including separating `emit_number` from the
`ksnprintf` that calls it.

CI asserts all three appear with a share, and that the *top* entry is one of
them rather than some unrelated function. Which of the three leads moves
between runs with host scheduling noise — 29/23/18 one time, 32/24/14 the
next — so naming a specific winner would be asserting a coin flip. The
property worth testing is that attribution still discriminates, and a flaky
test is worse than a loose one.

The same 612-symbol table makes panics readable with nothing but a serial log.
This is a real one, from the CI scenario that deliberately writes to the
kernel's own `.text`:

```
 page fault at 0xc0103000 in the kernel's own code or constants, which are read-only

  EIP c01184fe  CS  0008      EFLAGS 00000286
  at  cmd_fault+0x17e
  vector 14 (Page Fault)  error 00000003
  CR0 80010011  CR2 c0103000  CR3 00101000  CR4 00300000

Call trace (return addresses; the faulting frame is EIP above):
  [0] 0xc011a8c6  shell_run_line+0xe6
  [1] 0xc011ae67  shell_task+0x507
  [2] 0xc0103b02  thread_trampoline+0xb
```

Embedding that table is circular — it changes the addresses it describes — so
the build links three times and then **verifies** the table still matches,
rather than trusting that it does. Full methodology, per-benchmark analysis and
the profiler's limits are in [docs/PERFORMANCE.md](docs/PERFORMANCE.md).

---

## Two architectures

```bash
make riscv64 && make run-riscv64     # clang + ld.lld, no new toolchain
make portability                     # measure how portable the kernel is
```

```
  .-----------------------------------------------------.
  | StratumOS 0.12.0  -  riscv64 (rv64imac) on QEMU virt |
  '-----------------------------------------------------'
[    0.004] INFO  boot: machine mode: mtvec installed, CLINT timer at 100 Hz, 10 exception(s) delegated to supervisor mode
[    0.006] INFO  boot: supervisor mode reached by mret; sstatus 0x00000000
[    0.007] INFO  paging: Sv39 enabled: root at 0x8000a000, 2 gigapages identity-mapped

rvtest: running the *shared* code, compiled from the same sources the x86 kernel links
rvtest: core/printf.c ... PASS
rvtest: core/string.c ... PASS
rvtest: traps       ... PASS
rvtest: timer       ... PASS
rvtest: sv39        ... PASS
rvtest: summary 58 check(s), 0 failure(s)
```

The boot arc is the same shape as the x86 one, for the same kind of reason —
the mode you start in cannot do the thing you need next:

```
x86:     16-bit real mode  ->  32-bit protected mode  ->  paging
RISC-V:  machine mode      ->  supervisor mode        ->  paging (Sv39)
```

A hart comes out of reset in machine mode, which can do anything *except* use
`satp`: M-mode fetches bypass translation entirely, so paging is not
something an M-mode kernel can switch on for itself. `mret` is the analogue
of the far jump that reloads `CS`.

**But the point of the port is the measurement, not the boot.** Every other
phase added a subsystem; this one tests an assertion about the subsystems
that already existed — that most of `kernel/core` and `kernel/mm` was free of
x86. That had been written in the roadmap for several phases, and it was a
guess.

`make portability` compiles every portable-by-intent source for RISC-V with
`-Werror` and counts, so the number is generated rather than quoted:

**55% compiles unmodified** — 16 of 29 files — and the whole of this is in it:

| | |
| --- | --- |
| the entire network stack above the driver | `net/{net,arp,ipv4,udp,tcp}.c` |
| the filesystem and block layer | `fs/{fat16,blockdev}.c` |
| both allocators | `mm/{heap,pmm}.c` |
| the formatter, strings, 64-bit division, the logger | `core/{printf,string,div64,log}.c` |

Four of those are **linked into the RISC-V kernel and run there**, against
the same assertions — including `memmove` with overlapping ranges, the
function the ELF fuzzer caught in phase 7.

**The findings are the valuable part.** Before any fixing, seven files failed
with 88 errors, and every single one was this:

```
error: cast to 'void *' from smaller integer type 'u32' (aka 'unsigned int')
```

`paddr_t` and `vaddr_t` were `typedef u32`. Correct on i386, silently
truncating on anything wider, invisible to 633 passing assertions, and fixed
by one line — `uintptr_t` was available the whole time. That is a latent
correctness bug in shared code that **no amount of testing on one
architecture could have surfaced**, which is the argument for porting as a
testing activity rather than a feature one.

Exactly one failure was not a cast, and it is the more interesting one.
`mm/heap.c` failed on its *include line*: it needs interrupt masking and
nothing else from the processor, and was getting it from `<arch/io.h>` — the
x86 **port I/O** header, which does not exist on an architecture with no port
I/O. Portable code was reaching into the arch layer through a door labelled
with one architecture's name. Now there is `<kernel/irqflags.h>`.

The whole shared set needs **two** symbols from the architecture:
`console_putc` and `timer_ms`. Everything else — every `%` conversion, the
levelled logger, its rate limiter, its expected-error windows — came across
for free.

What is *not* ported, file by file with reasons, is in
[docs/PORTING.md](docs/PORTING.md): the scheduler's mechanism, `vmm.c` (x86's
recursive page-directory trick does not generalise past two levels), ring 3,
SMP and the e1000, whose reliance on cache-coherent DMA was predicted in a
comment written before this port existed.

---

## Networking

```bash
make run-net                      # boot with an Ethernet card
stratum> ping 10.0.2.2 3
```

```
pinging 10.0.2.2, 3 time(s), 32 bytes of payload
seq 1: reply from 10.0.2.2 in 0 ms
seq 2: reply from 10.0.2.2 in 0 ms
seq 3: reply from 10.0.2.2 in 0 ms
3 sent, 3 received, 0 lost
```

An Intel e1000, Ethernet, ARP, IPv4, ICMP, UDP and a TCP that completes a
three-way handshake, buffers and retransmits, and closes gracefully. Tested
against a real host-side peer through QEMU's `hostfwd` — the host's own TCP
did the other half of the handshake:

```
TCP echo result: (b'hello from the host\n', b'second line\n')
```

Two separate writes echoed in order, connection closed to TIME_WAIT, zero
retransmits.

**The bug worth telling you about.** The first working driver transmitted
every frame with a source MAC of `00:00:00:00:00:00`. `struct net_device`
was `const` and its `.mac` was never filled in, so the boot log printed the
driver's own copy — correctly — while `eth_output()` built frames from a
second copy that was all zeros. The peer replied, politely, *to the zero
address*, and the controller's own receive filter dropped the reply because
it was not addressed to the card.

The result: transmit worked, 204 ARP requests went out, the device's own
counter confirmed them, receive was completely silent, and **every log line
was right**. No assertion inside the kernel could have caught it, because
every value the kernel could compare was consistent with itself.

So CI does not read the kernel's log for this. It asks QEMU to dump every
frame to a pcap and parses the bytes, with its own checksum implementation:

```python
# Every IPv4 header the kernel sent, checksummed here rather than trusted.
if body[12:16] == GUEST_IP:
    if inet_checksum(body[:ihl]) != 0:
        bad_ip_checksum += 1
```

The two agreeing is the whole value, so the duplication is deliberate. The
scenario also requires that no frame had a zero source hardware address, so
that bug can never come back silently. The checksum test vectors in the
`net` suite are real headers lifted out of a capture — generated by somebody
else's stack, because a checksum tested against its own output is a checksum
tested against itself.

**What it deliberately does not have**, because a stack that quietly lacks
these is one that works in a lab: no congestion control (the biggest
omission, and why this is not a general-purpose TCP), no out-of-order
reassembly, no IP fragment reassembly, no DHCP, no routing table beyond one
gateway, and no sockets API — ring 3 cannot open a connection, because this
kernel has no file descriptors for a socket to be. Each has its reasoning in
[docs/NETWORK.md](docs/NETWORK.md).

---

## 64-bit long mode

The repository this project merges was called
*Advanced-Bootloader-16-bit-to-32-bit-C-Kernel*. This is the third step of
that arc:

```
16-bit real mode  ->  32-bit protected mode  ->  64-bit long mode  ->  back
   boot/stage1          boot/stage2 + _start      longmode_tramp.asm
```

```bash
make run-x86-64      # the same 32-bit image on a CPU that has x86-64
stratum> longmode
```

```
Long mode (x86-64)
  CPUID       : CPUID.80000001H:EDX.LM is set
  transition  : 32-bit protected -> 64-bit long -> 32-bit protected

Paging
  4-level     : pml4 0x00358000 -> pdpt 0x00359000 -> pd 0x0035a000
  identity    : 1024 MiB in 2 MiB pages (512 PD entries)
  entries     : 64 bits wide; CR4.PAE required, so paging is disabled to set it

Stages reached
  [ok] the trampoline's own GDT, 32-bit, identity-mapped
  [ok] CR0.PG set with the PML4: IA-32e compatibility mode
  [ok] far jump to a descriptor with L set: 64-bit mode
  [ok] the 64-bit payload ran to completion
  [ok] back to 32-bit code, still on 64-bit paging
  [ok] kernel CR3, kernel GDT, kernel stack restored

What 64-bit mode proved
  CS          : 0x18 (the descriptor whose L bit is set)
  EFER        : 0x00000500  LME set, LMA set
  CR4.PAE     : set
  64-bit imm  : 0x0123456789abcdef
  0xffffffff+1: 0x0000000100000000 (32-bit mode gives 0)
  r15         : 0xfeedfacecafebeef (a register 32-bit mode lacks)
  lea [rip+x] : 0x00009191, expected 0x00009191
  [64-bit ptr]: 0x5452415455004f53 from phys 0x00400000 (only the 4-level walk maps it)

  round trip  : 551716 cycles, trampoline 608 bytes
  kernel      : still 32-bit, still running - CR4.PAE clear, paging on
```

**This is a tested transition, not a 64-bit kernel**, and saying so clearly
matters more than the feature does. A port needs a 64-bit IDT (16-byte gate
descriptors), every assembly stub rewritten for a new calling convention,
`SYSCALL`/`SYSRET` instead of `int 0x80`, a four-level VMM — the recursive
page-directory trick this kernel uses does not generalise past two levels —
and an audit of every `u32` that is really an address. That is one commit
that either boots or does not, which is why it is kept separate from the
phase that made it testable.

Three things in it were the actual work:

**Paging has to come off.** `CR4.PAE` cannot be written while `CR0.PG` is
set, and long mode requires PAE. With paging off, `EIP` is a physical
address — so the only code that survives is code whose virtual and physical
addresses are the same. The kernel is at `0xC0100000` and loaded at
`0x00100000`, so none of it qualifies. The trampoline is therefore assembled
into the image but **copied to physical `0x9000` and executed there**, with
every absolute reference computed rather than written as a symbol, exactly as
the SMP bring-up already does.

**The far jump is the step people leave out.** Setting `EFER.LME` and
`CR0.PG` gets IA-32e *compatibility* mode: 64-bit paging under 32-bit code.
`EFER.LMA` is set, the tables are live, and it looks like it worked. The
processor decodes 64-bit instructions only once `CS` holds a descriptor whose
**L** bit is set — and in 64-bit mode there is no direct far jump to get back
out with, because opcode `EA` is invalid there, so the return goes through
memory.

**The strongest check caught itself being wrong.** The probe reads through a
64-bit pointer at a physical address that only the four-level walk can
resolve. The first version allocated that frame with `pmm_alloc_frame()`,
which hands out frames in ascending order, so it landed at 3.5 MiB — *inside*
the 4 MiB identity map, where the read would have succeeded whether the PML4
worked or not. The test passed and proved nothing. A warning that fired on
the first run is the only reason that is not still the case.

Tested on both kinds of processor, because `qemu-system-i386` masks
`CPUID.80000001H:EDX.LM` even with `-cpu max`: **17 checks** asserting the
kernel declines correctly there, **51** asserting the transition itself under
`qemu-system-x86_64`. The CI scenario checks the *check count*, because a run
where the suite quietly took its declining path would otherwise pass while
testing none of the feature.

More in [docs/LONGMODE.md](docs/LONGMODE.md).

---

## Testing

An OS that "boots on my machine" is not evidence of much. This project is set
up so that a regression is caught by a script, not by someone squinting at
QEMU.

| Layer | What runs | Count |
| --- | --- | --- |
| **Host unit tests** | the kernel's real `printf`/`string`/`div64` sources, compiled for the host, diffed against glibc | 92 checks |
| **Pre-boot validation** | Multiboot2 header and checksum, ELF type, entry point inside a load segment, load address, `.bss` alignment, the higher-half split, every embedded ring-3 program, absence of SSE | 20 failure conditions, every link |
| **Image validation** | the boot signature, stage 1 not overlapping its own partition table, the stage 2 header pointing at a real ELF, every partition inside the image, the FAT geometry, and every file in `/BIN` being an i386 ELF with `INIT` among them | every image, every build |
| **In-kernel suites** | allocator, paging, address spaces, copy-on-write, heap coalescing, interrupts, scheduler, processes, hardening, ATA and partitions, FAT16, ACPI/APIC/locks/IPIs, the 32→64→32 transition, checksums and the wire, syscall pointer validation, ELF rejection, symbol lookup, profiler attribution — all against real hardware state | 633 checks in 21 suites |
| **Boot scenarios** | custom bootloader unattended, **the same image on four processors**, the same image on a CPU with SMEP and SMAP, **the same image on a CPU with x86-64**, **the frames it puts on the wire, parsed from a pcap**, the same image with no Ethernet at all, GRUB/Multiboot2 unattended, 50 shell commands typed over serial, benchmarks + profile | 9 scenarios |
| **Deliberate faults** | a write to the kernel's own `.text`, and a write below a task's stack — each must panic, naming the address, the reason and the region, and exit with the panic code | 3 scenarios |
| **Fuzzing** | four libFuzzer targets over the **real** `elf.c`/`fat16.c`/`heap.c`/`acpi.c` under AddressSanitizer, plus 40,000 malformed system calls issued from ring 3 | 6 bugs found |

```
$ make test
  RUN     host unit tests
92 checks, 0 failures

  RUN     QEMU boot tests (both boot paths)
  [interactive-shell] shell driven over the serial console, command by command
    commands run     : 41
    result           : PASS
  [syscall-fuzz] 40,000 malformed system calls issued from ring 3
    calls accepted   : 18230
    calls refused    : 21770
    result           : PASS
  [long-mode] 32-bit protected mode to 64-bit long mode, and back
    in-kernel suites : 21/21 passed
    result           : PASS
  [network] an e1000, and the frames the kernel actually transmits
    frames captured  : 70
    in-kernel suites : 21/21 passed
    result           : PASS
  [benchmarks] microbenchmarks and a sampling profile
    result           : PASS
  [custom-bootloader] two-stage BIOS bootloader from a raw disk image
    in-kernel suites : 19/19 passed
    result           : PASS
  [multiboot2-grub] Multiboot2 via GRUB from an ISO
    in-kernel suites : 19/19 passed
    result           : PASS

run-tests: all 12 scenario(s) passed
```

Three details that make this work unattended:

- The kernel takes `autotest` on its command line, runs its suites, and then
  asks QEMU to exit via `isa-debug-exit`. No timeouts, no screen scraping for
  a prompt.
- Test output is a fixed, greppable format (`ktest: heap ... PASS (57 checks)`),
  so the harness asserts on data rather than on prose.
- The interactive scenario drives the shell the way a person would — waiting
  for the prompt, sending one command, reading the reply. Piping a script into
  the serial port does not work, because the UART's 16-byte FIFO overruns long
  before the kernel starts reading it. That is the kind of thing you only learn
  by trying it.

More in [docs/TESTING.md](docs/TESTING.md).

---

## Fuzzing

The four layers above test the kernel against inputs someone thought of.
Fuzzing tests it against inputs nobody thought of — which, for a kernel, is
the case that matters: every parser it has takes bytes from somewhere
untrusted. An ELF a user hands to `exec`. A FAT16 boot sector on a disk anyone
can image. An ACPI table from firmware. A system call argument from ring 3.

```bash
make fuzz                      # four targets, 20,000 runs each, in CI
make fuzz FUZZ_RUNS=5000000    # a real campaign
```

**The targets compile the real kernel sources.** `build/fuzz/fuzz_elf` links
`kernel/core/elf.c` — byte for byte the file that boots — against a shim that
supplies what a kernel would. Extracting the parsers into host-testable copies
is the more common shape, and it was rejected for one reason: a fuzzer that
finds bugs in a rewritten copy of a parser is finding bugs in the rewrite.

The accommodation that made it possible is `vmm_alloc_at()`, which the shim
implements as `mmap(MAP_FIXED_NOREPLACE)` **at the address the kernel asked
for** — because `elf_load_user()` maps a page at an address an untrusted header
chose and then writes through it as a raw pointer. So `elf.c` is fuzzed
completely unmodified, and a write one page past what it mapped takes
`SIGSEGV` exactly as it would in the kernel. That is also why the targets build
`-m32`: an image asking for `0x00400000` can only be honoured in a 32-bit
address space.

The other half is `user/fuzz.c`, an ordinary ELF loaded off the disk by the
ordinary loader, which issues 40,000 deterministic malformed system calls from
ring 3 — the only seat the kernel's pointer validation can actually be
attacked from. Its pass condition is that the kernel is still running
afterwards.

Six bugs, all pre-existing, every one of them in code the other four layers
covered and passed:

| Bug | Found by |
| --- | --- |
| `memmove` called `memcpy` with overlapping ranges — undefined behaviour that worked only because this `memcpy` happens to be a forward byte loop | ASan's `memcpy` interceptor |
| ACPI trusted the RSDP's own `length` field; `0xFFFFFFFF` reads 4 GiB from `0xE0000` | `fuzz_acpi` |
| the heap stopped coalescing after a shrinking `krealloc`, fragmenting one block at a time | `heap_check()` under `fuzz_heap` |
| **any ring-3 page fault panicked the kernel** — so a malformed program file took the machine down | `fuzz_elf`, via an ELF whose entry point lay outside every segment it mapped |
| the ELF loader leaked every mapped page when allocation failed mid-segment — a denial of service that repeats | the target's "nothing may be left mapped" assertion |
| a ring-3 process could flood the kernel console with log output, drowning out everything else | the syscall fuzzer |

And what it does not cover, stated because a fuzzing claim without a boundary
is advertising: the ring-3 fuzzer has no coverage feedback, the drivers are not
fuzzed, and nothing fuzzes concurrency.

More in [docs/FUZZING.md](docs/FUZZING.md), including the reasoning behind
every accommodation the shim makes.

---

## Repository layout

```
boot/                      the bootloader — real mode, then the PM switch
  stage1.asm               512-byte MBR: load stage 2 and verify it
  stage2.asm               A20, E820, disk I/O, PM switch, ELF32 loader
  serial.inc               16550 output from real mode

kernel/
  arch/x86/                everything that is x86 rather than kernel
    boot.asm               Multiboot2 header + the unified entry point
    isr.asm                256 generated interrupt stubs, one common tail
    switch.asm             context switch and the new-task trampoline
    usermode.asm           the forged IRET frame that reaches ring 3
    gdt.c idt.c irq.c      descriptor tables, dispatch, 8259 PIC
    cpu.c                  CPUID, reset, QEMU exit
    acpi.c                 RSDP, RSDT/XSDT, the MADT
    apic.c                 the local APIC, IPIs, INIT-SIPI-SIPI
    ap_boot.asm            176 bytes: a second CPU, real mode to the higher half
  mm/
    pmm.c                  bitmap physical frame allocator
    vmm.c                  paging, recursive page directory, fault reporting
    heap.c                 guarded first-fit kmalloc
  fs/
    blockdev.c             MBR partitions, and the bounds check for each
    fat16.c                read-only FAT16: BPB, chains, 8.3 names
  core/
    bootinfo.c             the two boot protocols, normalised
    sched.c                scheduler, tasks, fork, wait, the zombie reaper
    smp.c                  per-CPU state, bring-up, IPIs, TLB shootdown
    spinlock.c             real locks: test-and-test-and-set, bounded
    syscall.c              int 0x80 and userspace pointer validation
    elf.c                  defensive ELF32 loader for untrusted images
    usermode.c             address space + image for a process, and exec
    bench.c profile.c      microbenchmarks and the sampling profiler
    ksyms.c                the embedded symbol table
    printf.c log.c panic.c string.c div64.c ktest.c kmain.c
  drivers/                 serial, vga, timer, keyboard, rtc, pci, ata
  shell/shell.c            31 commands, line editing, history
  include/                 headers, grouped by subsystem

user/                      ring-3 programs, built as separate ELFs
  init.c                   started at boot; probes the boundary, forks, execs
  hello.c                  what exec() replaces a process with
  syscall.h                the stubs, and the little runtime a libc-less
                           program needs
  user.ld                  linked at 0x00400000 — a user address

tools/
  mkimage.py               assemble a bootable image, patch stage 2's header
  mkfat.py                 write a FAT16 filesystem from a directory tree
  check-image.py           validate a built image: MBR, BPB, /BIN contents
  check-kernel.py          validate a linked kernel before it is ever booted
  run-tests.py             the QEMU harness
  grub.cfg grub-test.cfg

tests/host/                unit tests that need no emulator
linker/kernel.ld           the link script, with its constraints documented
docs/                      architecture, boot, memory, testing, debugging
```

---

## Design decisions worth defending

The full set is in [docs/DESIGN-DECISIONS.md](docs/DESIGN-DECISIONS.md). Four
that shaped the most code:

**A recursive page directory instead of identity-mapping all of RAM.**
Once `CR0.PG` is set, a page table can only be edited through a virtual
address — but page tables come from the physical allocator, which can return a
frame anywhere. Mapping all of physical memory to solve that does not scale.
Instead the last page directory entry points at the directory itself, so the
hardware walk resolves `0xFFFFF000` to the directory and
`0xFFC00000 + i*4096` to the table for entry *i*. Every page table in the
system becomes addressable, at the cost of one 4 MiB slot.

**The kernel is linked high and loaded low.**
`_start` cannot run at `0xC0100000` — nothing is mapped there yet — so it
lives in a section whose virtual address equals its load address, builds a
page directory with a temporary identity map, enables paging, and jumps.
`vmm_init` then drops that identity map, which is what frees the bottom of
every address space for user processes and makes a NULL dereference fault for
free. `check-kernel.py` verifies the arrangement at build time rather than
trusting it.

**Preemption happens after the PIC's end-of-interrupt, not before.**
The timer handler only *requests* a reschedule; the switch happens at the tail
of the interrupt path. Switching earlier parks the outgoing task mid-handler
with the interrupt still in service — and if the incoming task then blocks
anywhere other than an interrupt, nothing ever sends that EOI and the timer
stops permanently.

**One generated table of 256 interrupt stubs.**
The conventional approach is hand-written `isr0..isr31` plus `irq0..irq15` in
two files with near-identical tails. They drift, and the remaining 200 vectors
point at nothing. Here the preprocessor generates all 256 and publishes a table
of their addresses; there is exactly one place the register-saving convention
is defined, and therefore exactly one place it can be wrong.

---

## Bugs this project found and fixed

StratumOS merges two earlier repositories
([Advanced-Bootloader-16-bit-to-32-bit-C-Kernel](https://github.com/Saksham932007/Advanced-Bootloader-16-bit-to-32-bit-C-Kernel)
and [Advanced-Kernel](https://github.com/Saksham932007/Advanced-Kernel)).
Merging them meant reading both carefully, which turned up real defects —
several of which meant the advertised features could not have worked:

| Where | Defect |
| --- | --- |
| `isr.asm`, `irq.asm` | The stubs called the C handlers without pushing the register frame, so every handler received a stale segment selector (`0x10`) as its context pointer. The keyboard and timer paths could not have worked. |
| `idt.c` | `sti()` ran *before* the PIC was remapped, so the first timer tick arrived on vector 8 — the double-fault vector. |
| `boot.asm` | `org 0x7C00` combined with `ds = 0x07C0` double-counts the segment base; every memory reference landed 0x7C00 bytes high. |
| `boot.asm` | The kernel was loaded to `es=0x1000` (physical 0x10000) but called at `0x1000`. |
| `boot.asm` | `ENTRY(kmain)` has no effect in a flat binary, so `call 0x1000` entered whichever function the compiler emitted first. |
| `Makefile` | `run` depended on `$(ISO)`, but the only rule was named `iso` — the target could never build. |
| `README`, `ARCHITECTURE` | Documented paging, virtual memory, a kernel heap with `malloc`/`free`, and system calls. None of them existed. |

The last row is the one that mattered most. The features are now implemented,
and the test suite exists so that the documentation cannot quietly drift away
from the code again.

The same suite has since caught defects in *this* code, which is the more
useful demonstration — a test that has never failed has not been shown to
work:

| Where | Defect | Caught by |
| --- | --- | --- |
| `sched.c` | `wait()` marked a collected zombie's slot reusable while the task was still linked into the run queue, holding a stack and a page directory. The next `task_create()` would hand out that slot and splice it into the list twice. | writing the `proc` suite |
| `heap.c` | A 4-byte footer made header+footer 28 bytes, so payloads were only 4-byte aligned after a block split. | `KT_ASSERT(IS_ALIGNED(p, 8))` |
| `bootinfo.c` | The command line and loader name were pointed at, not copied, out of stage 2's memory — and dangled the moment `vmm_init()` dropped the identity map. | `version` in the shell, page-faulting in `strlen` |
| `vmm.c` | The boot identity map covered page 0, so a write to address zero did not fault and NULL-dereference detection was silently off. | `fault null` returning success |

The `bootinfo.c` one is also what the embedded symbol table earned its keep
on: the panic read `at strlen+0x6` under `emit_number / kprintf / cmd_version`,
which made it a two-line diagnosis instead of a bisection.

Six more came from the fuzzing harness, which is the strongest version of the
same argument: those four were found by tests someone wrote on purpose, and
these were found by inputs nobody thought of, in code that every one of those
tests had already covered and passed. The worst of them let any unprivileged
process panic the kernel with a malformed program file. They are tabulated
under [Fuzzing](#fuzzing) above.

---

## What is deliberately not here

Being clear about scope is more useful than a longer feature list.

- **The filesystem is read-only.** A correct write allocates from the FAT,
  updates both copies, extends a directory entry's chain, and survives being
  interrupted between any two of those — a journalling problem rather than a
  filesystem-format one.
- **No long filenames, no `argv`, no file descriptors.** FAT's 8.3 names only;
  `exec` takes a path and nothing else.
- **No DMA and no disk interrupts.** The ATA driver is PIO and polled, which
  burns a timeslice per read on real hardware. The trade-off, and what it
  buys, is in [docs/STORAGE.md](docs/STORAGE.md#ata-by-programmed-io).
- **The scheduler runs on one processor.** Every processor is brought up,
  runs kernel code, holds real locks and services IPIs — but run queues are
  still per-system rather than per-CPU, so the other processors idle. That is
  the next large piece; see [docs/SMP.md](docs/SMP.md#what-is-missing).
- **The I/O APIC is parsed but not programmed.** Device interrupts still go
  through the 8259s to the boot processor, so there is no interrupt
  distribution and no affinity.
- **No `argv`, environment or file descriptors.** `exec` takes a program
  name and nothing else; `write` goes to the console unconditionally, so
  `fork` has no descriptor table to duplicate.
- **No signals, process groups or `kill`.** A process leaves through `exit`
  or a fault.
- **No demand paging.** A program's image is mapped eagerly; copy-on-write
  is the only laziness in the memory manager, and every other page fault is
  a bug and is reported as one.
- **No NX, so W^X is only half.** A 32-bit page table entry has no
  execute-disable bit, so the kernel's text is read-only but its data is
  still executable as far as the hardware is concerned. Getting NX means PAE:
  64-bit entries and a three-level walk. That and KASLR are the two largest
  pieces of hardening still outstanding — see
  [docs/SECURITY.md](docs/SECURITY.md#what-is-missing).
- **Serial transmit is polled**, which holds interrupts off for the duration of
  a write. The fix is a transmit ring buffer, in
  [docs/ROADMAP.md](docs/ROADMAP.md).
- **Empty page tables are never reclaimed.** Unmapping the last page in a
  4 MiB region leaves its table allocated.

---

## Documentation

| Document | Contents |
| --- | --- |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | subsystem map, initialisation order and why it is forced |
| [docs/BOOT.md](docs/BOOT.md) | both boot paths instruction by instruction |
| [docs/MEMORY.md](docs/MEMORY.md) | address-space layout, the higher-half transition, the three allocators |
| [docs/USERSPACE.md](docs/USERSPACE.md) | the user programs, the ELF loader, and the privilege boundary |
| [docs/PROCESSES.md](docs/PROCESSES.md) | address spaces, `fork`, copy-on-write, `exec`, `wait` |
| [docs/SECURITY.md](docs/SECURITY.md) | W^X, SMEP/SMAP, guard pages, and what is deliberately missing |
| [docs/STORAGE.md](docs/STORAGE.md) | the ATA driver, the partition table, FAT16, and where a program comes from |
| [docs/SMP.md](docs/SMP.md) | ACPI, the APIC, the AP trampoline, real locks, TLB shootdown |
| [docs/LONGMODE.md](docs/LONGMODE.md) | 32-bit to 64-bit long mode and back, the four-level table, and the boundary of the claim |
| [docs/NETWORK.md](docs/NETWORK.md) | the e1000, the ring protocol, the checksum's two classic bugs, and what the stack deliberately lacks |
| [docs/PORTING.md](docs/PORTING.md) | the RISC-V port, the measured portability of the rest, and the two typedefs that were holding it back |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | benchmark methodology, results with analysis, the profiler and its limits |
| [docs/TESTING.md](docs/TESTING.md) | the five test layers and how to add to each |
| [docs/FUZZING.md](docs/FUZZING.md) | both fuzzing harnesses, the shim's design, and the six bugs they found |
| [docs/DEBUGGING.md](docs/DEBUGGING.md) | GDB against QEMU, reading a panic, common symptoms |
| [docs/DESIGN-DECISIONS.md](docs/DESIGN-DECISIONS.md) | the trade-offs, with the alternatives that were rejected |
| [docs/ROADMAP.md](docs/ROADMAP.md) | what is next, and what each item would take |
| [CONTRIBUTING.md](CONTRIBUTING.md) | build, style and test requirements |

### References used

- Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 3A —
  protected mode, paging, interrupt and exception handling
- [Multiboot2 Specification 2.0](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html)
- [OSDev Wiki](https://wiki.osdev.org/) — A20, 8259 PIC, 8254 PIT, 8042, PCI
- Ralf Brown's Interrupt List — INT 13h and INT 15h behaviour

---

## License

MIT — see [LICENSE](LICENSE).
