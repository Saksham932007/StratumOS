# 64-bit long mode

The repository this project merges was called
*Advanced-Bootloader-16-bit-to-32-bit-C-Kernel*. This is the third step of
that arc:

```
16-bit real mode  ->  32-bit protected mode  ->  64-bit long mode  ->  back
   boot/stage1          boot/stage2 + _start      longmode_tramp.asm
```

```bash
make run-x86-64        # boot on a processor that has x86-64
stratum> longmode      # enter 64-bit mode, prove it, return
stratum> selftest longmode
python3 tools/run-tests.py --only long-mode
```

---

## What this is, and what it is not

**It is** a complete, reversible transition into 64-bit mode with a payload
that proves the processor is really there, under test on both kinds of
machine, with the 32-bit kernel still running afterwards.

**It is not a 64-bit kernel**, and being clear about that matters more than
the feature does. StratumOS's own code is 32-bit and stays 32-bit:
`vaddr_t` is a `u32`, the IDT holds 8-byte gates, the syscall path is
`int 0x80`, the scheduler saves 32-bit register frames, and the VMM's
recursive-mapping trick is specific to two levels.

A port means all of:

| | |
| --- | --- |
| a 64-bit IDT | gate descriptors are 16 bytes, not 8, and there is an interrupt stack table to set up |
| a new calling convention | every assembly stub: arguments in registers, a red zone, 16-byte stack alignment |
| `SYSCALL`/`SYSRET` | with `STAR`, `LSTAR` and `SFMASK`, instead of a software interrupt |
| a four-level VMM | including a replacement for the recursive PD mapping, which does not generalise |
| an audit of every `u32` | that is really an address |

Each is tractable. Together they are a port, not a phase — they are scheduled
in [ROADMAP.md](ROADMAP.md).

So what is here is the part that genuinely had to come first. Until the
processor can be driven into the mode and back out again, under test, none of
the rest can be developed at all: there is nothing to run the first 64-bit
interrupt handler *in*.

---

## The transition

### Why it runs from a copy at a low physical address

`CR4.PAE` cannot be written while `CR0.PG` is set, and long mode requires
PAE. So paging has to come **off** for a few instructions — and with paging
off, `EIP` is a physical address.

The kernel is linked at `0xC0100000` and loaded at `0x00100000`. None of it
can survive that switch. So `longmode_tramp.asm` is assembled into the kernel
image but **copied to physical `0x9000` and executed there**, with every
absolute reference computed as

```nasm
%define PHYS(label) (LM_TRAMPOLINE_PHYS + ((label) - lm_trampoline_start))
```

rather than written as a symbol — a symbol here would be a kernel virtual
address, which is exactly what is not mapped. `kernel/arch/x86/ap_boot.asm`
does the same thing for the same reason, and `0x9000` is the next page up
from the one it uses. The whole first megabyte is reserved in the PMM, so
nothing else is ever handed either page.

The caller switches to a page directory that identity-maps the first 4 MiB
before calling in. That directory is `vmm_create_bootstrap_pd()`, written for
the SMP bring-up and reused here unchanged — the kernel's own directory has
no low mapping, because dropping it is what frees user space.

So the trampoline's page is mapped at its own address under all four regimes
the code passes through: 32-bit paging, no paging, 64-bit paging, and back.

### Enabling it

Intel SDM volume 3A, section 10.8.5. The order is not negotiable:

```nasm
        ; 1. paging off - legal only because this code is identity-mapped
        mov     eax, cr0
        and     eax, ~CR0_PG
        mov     cr0, eax

        ; 2. CR4.PAE - long mode is PAE with a fourth level, and this bit
        ;    cannot be written while paging is on, which is why step 1 exists
        mov     eax, cr4
        or      eax, CR4_PAE
        mov     cr4, eax

        ; 3. CR3 = the PML4
        mov     eax, [PHYS(lm_pml4)]
        mov     cr3, eax

        ; 4. EFER.LME - the one step that is a model-specific register
        mov     ecx, MSR_EFER
        rdmsr
        or      eax, EFER_LME
        wrmsr

        ; 5. paging on - now in IA-32e *compatibility* mode
        mov     eax, cr0
        or      eax, CR0_PG
        mov     cr0, eax

        ; 6. the step that is easy to leave out
        jmp     LM_SEL_CODE64:PHYS(lm_entry64)
```

**Step 6 is the one worth dwelling on.** Setting `LME` and `PG` gets
compatibility mode: 64-bit paging under 32-bit code. The processor decodes
64-bit instructions only once `CS` holds a descriptor whose **L** bit is set,
and the only way to load `CS` is a far transfer. A transition that stops at
step 5 appears to work — `EFER.LMA` is set, the page tables are live — and
then every 64-bit instruction decodes as something else.

The descriptor that does the work:

```
0x18  dq 0x00AF9A000000FFFF      ; L=1, D=0
```

In 64-bit mode the base and limit are ignored. The `0000FFFF` and the
granularity bit are there because the descriptor format still has the fields,
not because anything reads them. The only bit doing any work is L.

### The four-level page table

```
PML4[0]     -> PDPT
PDPT[0]     -> PD
PD[0..511]  -> 2 MiB each, identity = the first gigabyte
```

Three tables, not four: a PD entry with the `PS` bit set maps a 2 MiB page
directly, which means 512 entries instead of 262144 and one page instead of
513. A gigabyte is far more than the trampoline needs; it costs one page, and
it means any physical address this machine can have is readable from 64-bit
mode — which is what makes the pointer probe below worth doing.

The entries are **64 bits wide**, and that is the whole difference from the
kernel's 32-bit tables. It is also why PAE has to be set first: PAE
introduced this entry format, and long mode is PAE with one more level on
top.

The processor walks page tables by physical address, so the tables themselves
need no mapping for the walk to work. That is the *opposite* of the
constraint on the trampoline, and worth stating because the two are easy to
confuse. The tables are reached from C through the MMIO window
(`vmm_map_mmio`) purely so they can be written.

### Getting back out

The reverse sequence, with one wrinkle: **64-bit mode has no direct far
jump.** Opcode `EA` is invalid there, so the destination goes through memory:

```nasm
        jmp     far dword [rel lm_far_back32]
...
lm_far_back32:  dd      PHYS(lm_back32)
                dw      LM_SEL_CODE32
```

Then paging off, `LME` off, `CR4` restored **verbatim** rather than having
PAE cleared — this kernel also keeps SMEP, SMAP and PSE in there, and
restoring the register wholesale cannot get any of them wrong — then the
bootstrap directory back in `CR3`, `CR0` restored verbatim, and only then the
kernel's own GDT, whose base is a kernel virtual address and is unmapped
until that moment.

The caller's `CR3` goes back in C, after the trampoline returns: the
trampoline has to leave the bootstrap directory loaded, because that is the
only one with a mapping for the instruction doing the restoring.

---

## What the payload proves

Each of these is an assertion in the `longmode` suite, not a line in a log.
Several are chosen because they are *impossible* in 32-bit mode rather than
merely different.

| Check | Why it is evidence |
| --- | --- |
| `mov rax, 0x0123456789ABCDEF` | a 64-bit immediate — an encoding (`movabs`) that does not exist in 32-bit mode |
| `mov eax, 0xFFFFFFFF; add rax, 1` → `0x100000000` | the same two lines leave `0` in 32-bit mode; here the result needs 33 bits and gets them |
| `mov r15, 0xFEEDFACECAFEBEEF` | a register 32-bit mode does not have, reached through the REX prefix |
| `lea rax, [rel lm_marker]` | RIP-relative addressing, new in 64-bit mode. Checked against the address the trampoline was *copied to*, computed by searching the trampoline's own bytes for the marker — so it tests the addressing mode, not a constant |
| `EFER.LMA` | the **processor's** statement that it is in long mode, read back with `rdmsr`, rather than the kernel's belief that it put it there |
| `CR4.PAE` | which long mode does not permit to be clear |
| `CS == 0x18` | the payload really was running on the descriptor with L set |
| a read through a 64-bit pointer | the probe, below |

### The probe, and the bug it caught

The strongest single check is a `mov rbx, [rax]` where `rax` holds a physical
address the **4 MiB identity map does not cover**. Nothing but the four-level
walk can resolve it: the kernel's 32-bit directory does not map it, and
neither does the bootstrap directory the transition runs under. Reading the
magic number back is therefore a test of the walk rather than of memory.

The first version allocated that frame with `pmm_alloc_frame()` like the
other three. The allocator hands out frames in roughly ascending order, so it
landed at `0x0035B000` — 3.5 MiB, **inside** the identity map, where the read
would have succeeded whether the PML4 worked or not. The test passed for the
wrong reason.

A warning that fired on the very first run is the only reason that is not
still the case. The fix was `pmm_alloc_frame_above()`, which is the general
primitive — the same shape of constraint is why real kernels have memory
zones, with ISA DMA needing a frame below 16 MiB — and the warning became a
hard failure, plus an assertion in the suite:

```c
KT_ASSERT(r, lm.probe_phys >= 4 * MIB);
KT_ASSERT(r, lm.probe_phys < (paddr_t)lm.identity_mib * MIB);
```

### The stage flags

```
  [ok] the trampoline's own GDT, 32-bit, identity-mapped
  [ok] CR0.PG set with the PML4: IA-32e compatibility mode
  [ok] far jump to a descriptor with L set: 64-bit mode
  [ok] the 64-bit payload ran to completion
  [ok] back to 32-bit code, still on 64-bit paging
  [ok] kernel CR3, kernel GDT, kernel stack restored
```

Six bits, each set by the code that reached that point. During development
this was the difference between a diagnosis and a reboot: a transition that
dies between steps 5 and 6 leaves the first two set, which says exactly where
to look. The suite asserts each individually rather than the aggregate, so a
regression names its own stopping point.

---

## Interrupts, and the honest cost

**Interrupts are off for the whole window, and they have to be.** Between the
trampoline's `lgdt` and the one that puts the kernel's back, there is no
descriptor table a handler could be dispatched through — and long mode needs
a different IDT format anyway, so the same table could not serve both halves.

An NMI or a machine check in the window would be fatal. That is an accepted
cost of a demonstration and would **not** be acceptable in a port, which is
one of several reasons a port needs its own IDT before it needs anything
else.

The window is short — a few hundred thousand cycles under emulation, almost
all of it TLB flushes and mode switches — but "short" is not "safe", and the
distinction is the point.

### What it does not disturb

The suite asserts all of this, because the transition touches `CR0`, `CR3`
and `CR4` and a caller must not be able to tell:

```c
KT_EQ(r, read_cr3(), cr3_before);
KT_EQ(r, vmm_count_user_pages(), pages_before);
KT_ASSERT(r, (read_cr4() & CR4_PAE_BIT) == 0);
KT_ASSERT(r, (read_cr0() & 0x80000000u) != 0);
KT_ASSERT(r, irq_enabled());
```

**Other processors are unaffected.** `CR0`, `CR3`, `CR4`, `GDTR` and `EFER`
are per-processor, so the boot processor's excursion is invisible to the
others, which stay in their idle loop on the kernel's directory throughout.
Verified by running the suite under `-smp 4`.

**It is repeatable.** The tables are built once and reused, so a second run
takes a path the first does not — and a transition that only works from a
cold start has a bug in its cleanup. The suite runs it twice and asserts the
tables were reused rather than rebuilt.

---

## Testing it on both kinds of machine

`qemu-system-i386` masks `CPUID.80000001H:EDX.LM` **even with `-cpu max`**,
because long mode is not available on a 32-bit target. So on every scenario
CI boots by default, this kernel cannot find long mode — and that is worth
testing rather than skipping:

| Where | What the `longmode` suite asserts | Checks |
| --- | --- | --- |
| `qemu-system-i386` (every other scenario) | the kernel declines correctly: nothing attempted, nothing built, a sentence rather than a flag, and the machine left alone | 17 |
| `qemu-system-x86_64` (the `long-mode` scenario) | the transition itself, every stage and every probe | 51 |

That is the same arrangement as the `hardened-cpu` scenario, for the same
reason: a feature test that silently skips is a feature test that stops being
read. And the second configuration is the realistic one — a 32-bit kernel on
a 64-bit-capable processor is what actual hardware looks like.

The harness grew a per-scenario emulator for this (`Scenario.qemu`), and
skips loudly with the binary's name if it is not installed, so a skip cannot
read as a pass.

---

## One build trap, now caught

`longmode.c` and `longmode.asm` compiled to the same object path, and
whichever rule ran second silently won. It presents as an undefined reference
to a symbol that is plainly there in the source, which is a confusing half
hour.

The asm file is now `longmode_tramp.asm`, and the Makefile fails loudly if it
ever happens again:

```make
OBJECT_COLLISIONS := $(strip $(filter $(ASM_OBJECTS),$(C_OBJECTS)))
ifneq ($(OBJECT_COLLISIONS),)
$(error two sources compile to the same object: $(OBJECT_COLLISIONS) - \
rename one of them, as gdt.c and gdt_flush.asm already do)
endif
```

Worth catching once, not twice.
