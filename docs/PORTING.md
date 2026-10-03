# The second architecture

StratumOS runs on RISC-V 64 as well as i386. Not the whole kernel — the boot
path, the trap layer, the timer and Sv39 paging, plus the parts of the
existing kernel that turned out to be genuinely architecture-independent,
which is the number this document exists to report.

```bash
make ARCH=riscv64                   # clang + ld.lld, no new toolchain needed
make ARCH=riscv64 run               # qemu-system-riscv64, no firmware
make ARCH=riscv64 test-boot              # 58 checks, unattended, with an exit code
make portability               # measure how much of the kernel builds for it
```

```
  .-----------------------------------------------------.
  | StratumOS 0.13.0  -  riscv64 (rv64imac) on QEMU virt |
  '-----------------------------------------------------'
[    0.002] INFO  boot: hart 0, device tree at 0x87e00000
[    0.004] INFO  boot: machine mode: mtvec installed, CLINT timer at 100 Hz, 10 exception(s) delegated to supervisor mode
[    0.005] INFO  boot: misa 0x001411ad, mvendorid 0
[    0.006] INFO  boot: supervisor mode reached by mret; sstatus 0x00000000
[    0.007] INFO  boot: stvec installed
[    0.007] INFO  paging: Sv39 enabled: root at 0x8000a000, 2 gigapages identity-mapped, 1 of 8 static tables used

rvtest: running the *shared* code, compiled from the same sources the x86 kernel links
rvtest: core/printf.c ... PASS
rvtest: core/string.c ... PASS
rvtest: running the riscv64 arch layer
rvtest: traps       ... PASS
rvtest: timer       ... PASS
rvtest: sv39        ... PASS

[    0.021] INFO  boot: traps taken: 4 total, 2 timer, 1 ecall, 1 illegal
rvtest: summary 58 check(s), 0 failure(s)
rvtest: ALL CHECKS PASSED
```

---

## Why do this at all

Every other phase of this project added a subsystem. This one asks a question
about the subsystems that already exist: **is "architecture-independent" true,
or is it just a directory name?**

The claim in `docs/ROADMAP.md` before this work was that most of
`kernel/core` and `kernel/mm` was free of x86. That was a guess. The only way
to find out is to build the sibling of `kernel/arch/x86` and see what breaks,
and the answer turned out to be specific and more interesting than either
"most of it" or "none of it".

---

## The measured answer

`make portability` compiles every portable-by-intent kernel source for
riscv64 with `-Werror` and counts. It is a tool rather than a paragraph
because a quoted number goes stale the first time somebody adds a file.

**55% of the kernel outside the architecture layer compiles for RISC-V 64
unmodified** — 16 of 29 files. And the whole of this ports with zero changes:

| | |
| --- | --- |
| `core/printf.c`, `core/string.c`, `core/div64.c`, `core/log.c` | the formatter, the string layer, 64-bit division, the levelled logger |
| `net/net.c`, `net/arp.c`, `net/ipv4.c`, `net/udp.c`, `net/tcp.c` | the entire network stack above the driver |
| `fs/fat16.c`, `fs/blockdev.c` | the filesystem and the block layer |
| `mm/heap.c`, `mm/pmm.c` | the allocators |
| `core/elf.c`, `core/ksyms.c`, `core/usermode.c` | the program loader and the symbol table |

Four of those are linked into the RISC-V kernel and **run** there, against
the same assertions: `printf.c`, `string.c`, `div64.c` and `log.c`. The rest
compile but are not linked, for reasons given below rather than left implied.

### What the failures actually were

Before any of this was fixed, seven files failed with **88 errors**. Every
single one was the same thing:

```
error: cast to 'void *' from smaller integer type 'u32' (aka 'unsigned int')
```

`paddr_t` and `vaddr_t` were `typedef u32`. Correct on i386, silently
truncating on anything wider, and the root cause of every portability
failure in `mm/`, `core/` and `fs/`. The fix is one line:

```c
typedef uintptr_t paddr_t;
typedef uintptr_t vaddr_t;
```

`uintptr_t` was available the whole time. The second-largest cause was
`u64` being `uint64_t`, which on LP64 is `unsigned long` — so every `%llu`
in the kernel became a format mismatch. Pinning `u64` to `unsigned long long`
fixed all of them at once, because the two have the same width everywhere
this kernel builds and only the *type* differed.

So the honest summary of the portability work is: **the code was far more
portable than it looked, and was being held back by two typedefs.**

### The one structural finding

Exactly one failure was not a cast. `kernel/mm/heap.c` failed on its
*include line*:

```c
#include <arch/io.h>   /* for irq_save() and irq_restore() */
```

The heap needs interrupt masking and nothing else from the processor, and it
was getting it from the x86 **port I/O** header — which also declares `inb`,
`outb` and the privileged instructions, and which does not exist on an
architecture with no port I/O space.

That is the more interesting failure, because it is structural rather than
arithmetic: portable code was reaching into the architecture layer through a
door labelled with one architecture's name. The fix is
`<kernel/irqflags.h>`, which names the thing portable code actually wants and
lets each architecture answer:

```c
#if defined(__i386__) || defined(__x86_64__)
#include <arch/io.h>
#elif defined(__riscv)
#include <arch/riscv64/irqflags.h>
#else
#error "StratumOS: no interrupt-flag implementation for this architecture"
#endif
```

Eleven files still include `<arch/io.h>` directly. Most of them legitimately
want port I/O; the ones that only want interrupt flags are listed in
`docs/ROADMAP.md` as follow-up, because changing them is mechanical and
changing them *all* in the same commit as the port would have obscured which
change did what.

### How little the arch layer owes the portable code

The whole shared set needs **two** symbols from the architecture:

| Symbol | Needed by | RISC-V implementation |
| --- | --- | --- |
| `console_putc` | `core/printf.c` | a volatile store to a 16550 at `0x10000000` |
| `timer_ms` | `core/log.c` | `mtime / 10000`, because `mtime` is a real clock |

That is the entire interface. `ksnprintf`, every `%` conversion, the levelled
logger, its rate limiter and its expected-error windows all came across for
the cost of those two functions.

---

## The boot arc

The same shape as the x86 one, for the same kind of reason: the mode you
start in cannot do the thing you need next.

```
x86:     16-bit real mode  ->  32-bit protected mode  ->  paging
RISC-V:  machine mode      ->  supervisor mode        ->  paging (Sv39)
```

A hart comes out of reset in **machine mode**, which can do anything — and
which cannot use `satp`, because M-mode instruction fetches and data accesses
bypass translation entirely. So paging is not something an M-mode kernel can
switch on for itself; it has to drop to supervisor mode first. `mret` is how,
with `mstatus.MPP` naming the mode to return *to* and `mepc` the address: the
direct analogue of the far jump that reloads `CS`.

### What the loader hands you, and what it does not

QEMU's `virt` board with `-bios none` starts hart 0 at the base of RAM with
`a0` = hart id and `a1` = a pointer to the device tree. Compared to the x86
boot:

| | x86 | RISC-V |
| --- | --- | --- |
| Where execution starts | 0x7C00, from a 512-byte sector the BIOS loaded | the base of RAM, from an ELF the loader placed |
| Memory map | ask the BIOS (E820), in real mode, before anything else | a device tree pointer, in a register |
| Other processors | held in reset until sent INIT-SIPI-SIPI | **already running the same code** |
| `.bss` | cleared by stage 2 | cleared by nobody; the kernel must do it |
| A stack | set up by stage 1 | `sp` is undefined at entry |

The third row is the one that bites. Every hart enters `_start` at the same
instant, so the first instruction has to be a check:

```asm
        csrr    t0, mhartid
        bnez    t0, park_hart
```

They are parked rather than brought up — waking them needs the CLINT's
software-interrupt registers rather than x86's INIT-SIPI-SIPI, and that is a
separate exercise. Parked deliberately, and said so, rather than left to race
through the same initialisation.

### The physical memory protection trap

One thing with no x86 counterpart at all. Before `mret` can hand control to
supervisor mode, a PMP entry has to exist that permits S-mode to reach
memory:

```asm
        li      t1, 0x3fffffffffffff    # NAPOT, the whole address space
        csrw    pmpaddr0, t1
        li      t1, 0x1f                # NAPOT | R | W | X
        csrw    pmpcfg0, t1
```

With no PMP entry configured, some implementations deny S-mode everything,
and the first supervisor instruction faults. Configuring a protection region
that protects nothing is the documented way to say "S-mode may access all of
it", which reads strangely and is correct.

---

## Traps

A RISC-V trap does far less for you than an x86 one. There is no stack
switch, no automatic register push, no error code, and no separate vector per
cause: control arrives at `mtvec`/`stvec` with the cause in `mcause`/`scause`
and **every register still holding whatever the interrupted code had in it**.

So the entire frame is the kernel's responsibility — `pushad` has no
counterpart, and `trap_entry.S` saves 31 registers by hand.

There are also **two** vectors rather than one IDT. The machine timer stays
in M-mode because `mtimecmp` is an M-mode device and the architecture does not
permit delegating machine interrupts; everything a supervisor kernel should
see is delegated to `stvec` through `medeleg`. The x86 side has one table;
this port has to get both right.

### The bug worth recording

`sepc` points **at** the instruction that trapped, not past it — unlike an
x86 trap gate, whose saved `EIP` is already beyond a software interrupt. So
returning to `sepc` unchanged re-executes it and traps forever.

The first version stepped over it with `sepc + 4`, with a comment asserting
that `ecall` and `unimp` were both four bytes. `ecall` is. `unimp` assembles
to the **compressed** `c.unimp`, which is two — so the handler resumed two
bytes into the *next* instruction, trapped again on whatever decoded from
there, and reported two illegal instructions where the test expected one. It
also corrupted a global on the way, because execution briefly continued
through bytes that were not an instruction boundary.

The fix is to decode the length rather than assume it:

```c
/* RISC-V encodes the length in the first halfword: if its low two bits are
 * not both set the instruction is a 16-bit compressed one. */
static u64 instruction_length(u64 pc)
{
    u16 first = *(const volatile u16 *)(uptr)pc;

    return ((first & 0x3u) == 0x3u) ? 4 : 2;
}
```

A wrong constant that is right for one of its two callers is the worst kind.
x86 has no equivalent exposure, because its trap frames carry the length
implicitly.

---

## Sv39

Three levels of 512 entries, 4 KiB pages, a 39-bit address split 9 + 9 + 9 +
12. An entry at the root maps a 1 GiB *gigapage*, one at the middle level a
2 MiB *megapage*, one at the leaf a 4 KiB page.

**None of `kernel/mm/vmm.c` ports**, and the reason is not the entry format
or the level count. It is that x86's two-level paging lets the kernel point
the last page-directory entry at the directory itself, which makes every page
table appear at a computable virtual address — the recursive mapping the
whole of `vmm.c` is built around. With three levels there is no single window
that reaches all of them.

So this port does the other thing: it identity-maps physical memory and walks
the tables through those addresses. Simpler, costs an identity map the kernel
must keep, and is what Linux does on RISC-V. **The policy ports; the
mechanism does not** — which is the clearest single answer the port produced
to "how much of the memory manager is portable".

Three smaller differences, each of which had to be found:

- A PTE is not "the address with flags in the low bits". The physical page
  number lives in bits 53:10, so it is shifted right by 12 and then **left
  by 10**. Getting that wrong produces a page table that faults on
  everything, which is at least loud.
- A non-leaf entry is one with `V` set and `R`, `W` and `X` all clear. There
  is no dedicated page-size bit: a large page is *inferred* from having
  permissions. More economical than x86's `PS`, and much easier to walk into
  by mistake.
- The `A` and `D` bits have to be set by software up front. RISC-V permits an
  implementation either to set them in hardware on first access or to raise a
  page fault so software can; QEMU is the first kind, so leaving them clear
  works until it is run on hardware of the second kind.

Addresses must also be **canonical**: bits 63:39 all equal bit 38, so the
address space has a hole in the middle. x86-64 has the same rule; 32-bit x86
does not, and the kernel being ported from had nowhere to express it. The
check in `paging_map_page()` is new code rather than ported code, and the
suite tests it.

---

## What is not ported, and why

Stated file by file, because "a partial port" with no inventory is not a
result:

| | |
| --- | --- |
| `core/sched.c` | compiles except for casts, but context switching *is* the architecture: `switch.asm` saves x86 registers and a RISC-V version saves different ones. The scheduler's policy would port; its mechanism is a sibling file that does not exist. |
| `mm/vmm.c` | see above. Replaced, not ported. |
| `core/ktest.c` | one translation unit holding every x86 suite — the APIC, the page tables, the ATA driver. The RISC-V kernel has its own small harness instead, and carving `ktest.c` into per-subsystem files is a refactor of the *x86* side rather than part of this port. |
| `mm/heap.c`, `mm/pmm.c` | compile cleanly and are **not linked**, because the heap grows through `vmm_alloc_at()` and the frame allocator is initialised from the x86 boot info. Both need a RISC-V VMM under them first, and that is the next piece of real work rather than a cast away. |
| `shell/shell.c`, `core/kmain.c` | every command reaches into an x86 subsystem. |
| Userspace, SMP, the e1000 | no ring-3 equivalent set up, the other harts are parked, and the driver assumes cache-coherent DMA — which x86 guarantees and RISC-V does not, so a port needs explicit cache maintenance that this driver does not have. The driver says so in a comment, and that comment was written before this port existed, which is the one prediction that held. |

## The build

One variable selects the toolchain, the flags, the source set, the linker
script, the object tree, and what `run` and `test` mean:

```bash
make                     # ARCH=x86, the kernel's home architecture
make ARCH=riscv64        # rv64imac on QEMU's virt board
make ARCH=riscv64 test
make toolchain           # which architecture, and which tools
```

Each architecture's build lives **beside the code it builds**, for the same
reason the sources do:

```
kernel/arch/x86/sources.mk      what it compiles       (30 lines)
kernel/arch/x86/arch.mk         how                   (489 lines)
kernel/arch/riscv64/sources.mk                         (39 lines)
kernel/arch/riscv64/arch.mk                           (123 lines)
Makefile                        everything shared     (318 lines)
```

Adding a third architecture means adding a directory, not editing a switch
statement in the middle of the build system. The list of available
architectures is **discovered** from `kernel/arch/*/arch.mk` rather than
written down, so the error message stays true:

```
$ make ARCH=sparc
Makefile:47: *** unknown ARCH 'sparc' - available: riscv64 x86.  Stop.
```

Objects and artefacts go to `build/$(ARCH)`, so both architectures can be
built side by side and neither can pick up the other's stale objects.

### Two phases, because that is what the dependency is

`sources.mk` declares *what* an architecture compiles; `arch.mk` defines
*how*. They are separate files for a specific reason: a rule's prerequisites
are expanded when the rule is **read**, so the shared object list has to
exist before `arch.mk`'s link rule is seen — and the shared object list
depends on which shared sources `sources.mk` selected.

The first attempt included one file twice to break that circle. It worked,
and warned about an overriding recipe for every rule in it. Two phases is
what the dependency actually is, so saying so is cheaper than working around
it.

### The asymmetry is the platform, not the effort

`kernel/arch/x86/arch.mk` is 489 lines; the riscv64 one is 123. That is
worth noticing rather than apologising for: i386 needs a two-stage
bootloader, a FAT16 image, a GRUB ISO, embedded ring-3 programs and a
three-pass link to embed a symbol table. A riscv64 kernel is an ELF the
loader places in RAM.

### What the unification changed, and what it did not

The shared source list is now defined **once**, and an architecture selects
from it either by directory (`ARCH_SHARED_DIRS`, which x86 uses for all six)
or by named file (`ARCH_SHARED_FILES`, which riscv64 uses for four). That
asymmetry is the measurement, expressed as a build dependency: riscv64 names
files rather than taking `core` because `kernel/core` also holds the
scheduler and the x86 test suite, and claiming the directory would overstate
the result.

The object-collision guard also became *one* guard instead of two. It had
been duplicated per build tree - which is how the riscv64 port walked into
the same collision the x86 guard existed to catch - and with one object tree
per architecture there is one check, run for whichever architecture is being
built. That is what the earlier version should have been.

`make` with no arguments does what it always did, and all 12 x86 scenarios
still pass.

---

## Two things the toolchain made easy, and one it did not

**No new compiler.** clang is a cross compiler for every target it supports,
so `--target=riscv64-unknown-elf` and `ld.lld` were already installed by the
fuzzing work. A gcc-based port needs a separate toolchain per architecture.

**`-mcmodel=medany`** is required and the default is wrong. The default
(`medlow`) assumes everything sits in the low 2 GiB and produces relocations
the linker cannot resolve for a kernel at `0x80000000`.

**The link address cost an hour.** The kernel was linked at `0x80200000` —
where OpenSBI hands control to a payload, and therefore where a kernel booted
the normal way belongs. With `-bios none` the hart's reset vector jumps
straight to the base of RAM and **the ELF entry point is not consulted**, so
QEMU executed empty memory. A perfectly well-formed ELF, the right entry
address in its header, and no output whatsoever.

---

## Testing

`make ARCH=riscv64 test-boot` boots the kernel with no firmware and asserts 17
expectations against its output, then checks the exit status. Unattended the
same way the x86 side is: the SiFive test device at `0x100000` terminates the
machine when written to, which is the role `-device isa-debug-exit` plays on
x86.

Its convention is **not** x86's, which cost one debugging round:
`isa-debug-exit` exits with `((code << 1) | 1)`, so the x86 harness expects 3
for a pass; the SiFive device treats `0x5555` as a clean shutdown and exits
`0`. Assuming the formula carried over made all 17 expectations pass and the
scenario fail — a small illustration that porting a *harness* costs something
on top of porting a kernel.

The 58 checks are deliberately weighted towards the shared code, because that
is the claim under test:

| Suite | What it establishes |
| --- | --- |
| `core/printf.c` | every `%` conversion, width, precision, flag and the `snprintf` truncation contract — from the same file the x86 kernel links, on a target with a different calling convention and a different `va_list`. `%p` must print 16 digits where x86 prints 8, from the same code. |
| `core/string.c` | including `memmove` with overlapping ranges in both directions — the function the ELF fuzzer caught calling `memcpy` with overlapping arguments in phase 7. The fix was in shared code, so the test is shared too. |
| `traps` | an `ecall` and an illegal instruction, each counted exactly once. Twice means the handler resumed mid-instruction, which is the bug above. |
| `timer` | that the machine timer still fires after the privilege transition, which is the part that is easy to break. |
| `sv39` | all three levels, by mapping a 4 KiB page at a high canonical address and proving a write through it is visible through the identity map. Plus the gigapage offset mask, which is 30 bits wide and not 12. |

The deliberate rejections in the Sv39 suite log at ERROR level on purpose, so
they run inside `log_expect_errors()` — which is in the shared `log.c` and
came across with it. Nothing in the RISC-V kernel had to implement it, and
the check asserts the window swallowed exactly two, because a window that
swallows nothing means a rejection happened silently.

---

## What this was worth

The honest answer to "is this kernel portable" turned out to be: **yes, much
more than it looked, and it was being held back by two typedefs and one
`#include`.**

That is a better result than either outcome I would have guessed. It is also
only knowable by doing it — `paddr_t` being `u32` is invisible on i386,
passes every test, and is exactly the kind of thing that is obvious in
retrospect and undetectable in advance.
