# Symmetric multiprocessing

What "SMP" means here, precisely, because the phrase covers a lot of ground:

- every processor the firmware reports is taken out of reset, brought through
  real mode into the higher half, given its own GDT entry, TSS, stack and
  local APIC configuration, and runs kernel code;
- `spinlock_t` is a real spinlock rather than an interrupt mask;
- processors can interrupt each other, and TLB shootdown works and is
  acknowledged;
- **the scheduler still runs only on the boot processor.** Application
  processors park in their own idle loop and service interrupts.

That last line is the honest boundary. Per-CPU run queues are a separate
change with their own set of ways to be subtly wrong, and claiming them before
they exist would be exactly the kind of overstatement this project was started
to correct — the two repositories it merges documented paging, a heap and
system calls that did not exist.

```
stratum> cpus
Firmware
  ACPI      : RSDT from "BOCHS ", 4 table(s), MADT parsed
  reports   : 4 processor(s), 1 I/O APIC(s), 5 interrupt override(s)
              ISA IRQ 0 arrives as GSI 2
              ISA IRQ 5 arrives as GSI 5
              ISA IRQ 9 arrives as GSI 9
              ISA IRQ 10 arrives as GSI 10
              ISA IRQ 11 arrives as GSI 11
Local APIC
  enabled   : id 0, version 14; 12 IPI(s) sent, 0 EOI(s), 0 spurious, 0 error(s)
Processors (4 online of 4 reported)
   CPU  APIC  ROLE STATE          STACK    PINGS      TLB   IDLE LOOPS
     0     0   bsp online    0x00000000        0        0            0  <- this one
     1     1    ap online    0xe800a000        0        0            1
     2     2    ap online    0xe800f000        0        0            1
     3     3    ap online    0xe8014000        0        0            1

stratum> ipi
broadcasting a ping IPI to every other processor...
  cpu 1: 0 -> 1 ping(s)
  cpu 2: 0 -> 1 ping(s)
  cpu 3: 0 -> 1 ping(s)
3 of 3 other processor(s) answered

now a TLB shootdown, which waits for every processor to acknowledge:
  1 shootdown(s) sent, 3 served (was 0)
```

---

## The route a second processor takes

```
  ACPI RSDP  ->  RSDT  ->  MADT          how many processors, and the APIC's address
                                ↓
  local APIC enabled                     the only thing that can interrupt another CPU
                                ↓
  trampoline copied to 0x8000            a CPU out of reset starts in 16-bit real mode
                                ↓
  INIT - SIPI - SIPI                     the universal startup algorithm
                                ↓
  16-bit real mode  ->  CR0.PE  ->  32-bit  ->  CR0.PG  ->  higher half  ->  ap_main()
```

A processor taken out of reset begins executing in 16-bit real mode at
`CS = vector << 8`, `IP = 0`. So its first instruction is at a physical
address below 1 MiB, with no paging, no GDT it can trust and a 64 KiB
addressing limit — which is where the boot processor started, a second and a
half earlier. The trampoline covers the same ground in 176 bytes.

### ACPI, and only the four facts that matter

`kernel/arch/x86/acpi.c` reads four things and ignores the rest: how many
processors there are, their local APIC ids, where the local APIC's registers
live, and where the I/O APIC is. Power management, device enumeration and the
AML bytecode interpreter are a different project.

ACPI rather than the older MP Floating Pointer Structure because QEMU and
every machine built this century publish ACPI, many publish no MP table at
all, and the MADT is simpler to parse than the MP configuration table.

Everything is validated, and for a specific reason: the lengths in these
tables are what the loops iterate over, and the entry counts are what the
arrays are indexed by. Each table carries a checksum — the sum of its bytes,
truncated to 8 bits, which must be zero — precisely because the firmware's
authors did not trust themselves either. A zero-length MADT entry would loop
forever; one whose length runs past the table would read whatever follows it.
Both are firmware bugs and both are survivable here.

The tables also live outside the linear map. QEMU puts them at `0x07FE0000`
on a 128 MiB machine — eight times beyond the 16 MiB the kernel addresses
physically — so there is now an MMIO window at `0xCE000000` with a bump
allocator, used for the ACPI tables and for the local APIC's registers. Those
have to be mapped **uncached**: a cached read of the interrupt command
register can return a value the APIC has already changed.

### The trampoline

Assembled into the kernel image but run from a copy at physical `0x8000`, so
every absolute reference is computed rather than written as a symbol:

```asm
%define PHYS(label) (AP_TRAMPOLINE_PHYS + ((label) - ap_trampoline_start))

                lgdt    [PHYS(ap_gdtr)]
                mov     eax, cr0
                or      eax, CR0_PE
                mov     cr0, eax
                jmp     dword SEL_KERNEL_CODE:PHYS(ap_entry32)
```

A symbol here would be a kernel virtual address, which is exactly what is not
mapped yet. The parameter block is at a fixed offset and its field offsets are
exported from the assembly as absolute symbols, so the layout has one
definition rather than one in assembly and one in C.

The whole first mebibyte is already reserved by the physical allocator, so
nothing else can be using `0x8000`.

### The page directory an application processor enables paging with

Not the kernel's. The kernel's page directory has no identity mapping — it was
dropped in `vmm_init()`, which is what frees the bottom of the address space
for user processes — so enabling paging with it would unmap the instruction
after `mov cr0`.

The boot processor solved this at startup by having a temporary low mapping.
An application processor is instead handed a directory that identity-maps the
first 4 MiB *and* contains the kernel's half, and `ap_main()` switches to the
real one once it is running at a kernel address.

Doing it this way rather than briefly adding a low mapping to the kernel's own
directory means **the kernel's address space is never in a state its own test
suite would reject**. The `harden` suite asserts that page zero is unmapped;
that assertion holds throughout bring-up rather than being true only at the
moments it happens to be checked.

### INIT-SIPI-SIPI

```
  1. assert INIT          resets the target processor
  2. deassert it
  3. wait 10 ms           for the reset to complete
  4. send STARTUP         with the trampoline's page number
  5. wait 200 us
  6. send STARTUP again
```

The second STARTUP is not belt-and-braces: the specification requires it,
because the first can be lost if the processor was still finishing its reset.
A processor that has already started ignores the duplicate, so sending it
twice is always safe and sometimes necessary.

The delays happen before the processors exist, so there is nothing to sleep on
and the scheduler is not running. They are spins on reads of port `0x80`, the
unused POST diagnostic register — the traditional way to consume about a
microsecond of bus time without a calibrated timer.

Processors are started **one at a time**. The trampoline has one parameter
block, so two starting at once would race for the stack pointer in it, and the
failure would be two processors sharing a stack. That costs about 10 ms per
processor and is what every kernel does.

### Per-processor state

| Thing | Why it has to be per-CPU |
|---|---|
| TSS | The CPU reads `ss0`/`esp0` out of whichever TSS its *own* task register names. One shared TSS means a ring-3 interrupt on one processor switching to another processor's kernel stack. |
| GDT entry | `ltr` names a descriptor, so each processor needs a TSS descriptor. The GDT itself is shared: the only per-CPU thing in it is the TSS, and duplicating four identical flat descriptors eight times to avoid eight extra entries would be the wrong trade. |
| Stack | A processor needs one before it can execute a line of C. These cannot come from the task allocator, because the scheduler does not exist when an application processor arrives. They live at `CPUSTACK_BASE`, laid out exactly like task stacks: one guard page, then the stack. |
| Local APIC | One per processor, built into it. Each configures its own. |
| IDT | Not per-CPU. An interrupt descriptor table has no per-CPU content, so application processors load the same one — which is why there is `idt_load()` separate from `idt_init()`. |

---

## Identifying the current processor

This turned out to be the most interesting measurement in the phase.

The obvious way is to read the local APIC's id register. It works, and it is
an uncached access to a device:

```
  cpu-index    med=453.256c   = 215.719 ns
  spinlock     med=579.892c   = 275.989 ns      (two cpu-index calls each)
  ctxsw        med=886.405c
```

216 nanoseconds to answer "which processor am I" — and it is on the
context-switch path, because `tss_set_kernel_stack()` has to write *this*
processor's TSS. An uncontended spinlock cost about as much as a system call,
and most of that was two APIC reads.

The better way was already there. Each processor executed
`ltr SEL_TSS(cpu)` with its own selector, because each needs its own TSS. So
**the task register is a per-CPU identity the kernel was obliged to set up
anyway**, and `str` reads it back in one instruction that touches no memory:

```c
static inline u32 smp_cpu_index(void)
{
    u16 tr;

    __asm__("str %0" : "=rm"(tr));

    u32 index = (u32)(tr >> 3);

    if (index < GDT_TSS_FIRST || index >= GDT_TSS_FIRST + SMP_MAX_CPUS)
        return 0;   /* before any ltr, TR is null - and then there is one CPU */

    return index - GDT_TSS_FIRST;
}
```

```
  cpu-index    med=3.636c     = 1.731 ns       125x
  spinlock     med=67.564c    = 32.175 ns        8.5x
  ctxsw        med=579.901c                       1.5x
```

Medians under emulation, so the absolute figures are the emulator's. The
ratios are the finding, and the benchmarks exist because the cost was a design
decision worth measuring rather than asserting.

The textbook alternative is a per-CPU GDT descriptor whose base points at the
`struct cpu`, reached as `%gs:offset` — what Linux does, and better still,
because it gets the whole structure rather than an index. It also needs the
interrupt stubs to load a per-CPU GS on every kernel entry, which needs the
processor already identified. `str` is how you would break that circle. It is
not needed yet.

---

## Locks that actually exclude

Two things have to be excluded, and they are not the same thing:

- **another processor**, excluded by spinning on a shared word until an atomic
  compare-exchange wins it;
- **an interrupt handler on this processor**, excluded by masking interrupts —
  which spinning cannot help with, because a handler that interrupts a lock
  holder and then waits for that lock deadlocks a processor against itself.

So every acquisition does both, and the order is not arbitrary: interrupts off
first, then the word. Taking the lock before masking leaves a window in which
an interrupt arrives while the lock is held, and the deadlock that follows is
timing-dependent and rare, which is the worst combination.

### Test-and-test-and-set

```c
for (;;) {
    if (!__atomic_load_n(&lock->locked, __ATOMIC_RELAXED)) {
        if (__atomic_compare_exchange_n(&lock->locked, &expected, 1, ...))
            return;
    }
    cpu_relax();
}
```

The read before the write is the point. An atomic read-modify-write on a
contended cache line bounces that line between processors on every attempt;
reading until the lock *looks* free and only then attempting the exchange
leaves the line shared while waiting. On two processors it barely matters; on
more it is the difference between a lock and a traffic jam.

`cpu_relax()` is `PAUSE`. It tells the processor this is a spin loop, which on
a real machine avoids a memory-order violation penalty when the lock is
finally released, and on a hyperthreaded one yields the pipeline to the
sibling — which may well be the holder.

### What the lock refuses to do

- **Spin forever.** Two hundred million attempts and it panics, naming the
  lock and the processor holding it. The alternative to a bound is a kernel
  that hangs with no output, which is the failure mode this project keeps
  refusing.
- **Be acquired recursively.** With interrupts masked, the only way to reach a
  lock this processor already holds is a nesting bug, and failing immediately
  puts the stack trace at the culprit rather than at a hang. This check
  predates SMP — it existed when the "lock" was only an interrupt mask,
  specifically so the bug would be caught before it could matter. It was.
- **Be released by a processor that does not hold it.**

It also counts acquisitions and contentions. A lock that is never contended
could be something cheaper; one that always is wants splitting. Neither is
visible without counting.

---

## TLB shootdown

A TLB is per-processor. Unmapping a page on this processor leaves every other
processor's cached translation for it intact — and still usable. Any change
that removes or narrows a mapping another processor could be using has to tell
them.

```c
void smp_tlb_shootdown(vaddr_t va)
{
    invlpg(va);                       /* always, even alone */
    if (!smp_enabled()) return;

    spin_lock(&shootdown_lock);       /* one at a time: one global address */
    shootdown_addr = va;
    shootdown_acks = 0;
    apic_broadcast_ipi(APIC_VECTOR_IPI_TLB);

    /* wait for every other processor to confirm */
}
```

Three details carry weight:

**It invalidates locally whether or not anyone is listening.** A caller should
not have to ask how many processors there are to get correct behaviour on its
own.

**The handler acknowledges *after* the invalidation**, not before. The sender
waits on that count precisely so it knows every other processor has already
dropped the translation; an acknowledgement sent first would make the wait
meaningless.

**The wait is bounded, and a short count is an error rather than a shrug.** A
processor wedged with interrupts disabled would otherwise hang the sender, and
an unacknowledged shootdown is a correctness problem worth a loud complaint.

The measured cost, including every acknowledgement, is about 128,000 cycles on
four emulated processors — which says more about IPI delivery under TCG than
about silicon, but is the right order of magnitude to expect: a shootdown is
expensive, which is why real kernels batch them.

`panic()` uses the same machinery to stop the other processors before printing
anything. A kernel that has decided it cannot continue should not leave three
processors running in the state that made it decide that — and more
immediately, two processors interleaving output through one console would make
the one message that matters unreadable.

---

## Two bugs, and what found them

### The timer stopped, and every log line shared a timestamp

Enabling the local APIC and masking every local vector table entry — which
looks like exactly the right thing to do with entries the kernel does not
handle — produced a kernel that booted perfectly and then never advanced its
clock. Every line timestamped `0.040`, and the autotest hanging before its
first suite.

Before the local APIC is enabled, the 8259 pair drives the processor's INTR
pin directly. Enabling the local APIC puts that pin behind **LINT0**, in the
arrangement the firmware set up and the specification calls virtual wire mode.
A masked LINT0 means no 8259 interrupt reaches the processor at all — and this
kernel's timer, keyboard and serial input all arrive through the 8259s.

So the boot processor takes `ExtINT` on LINT0 — "the vector comes from the
external controller, not from this register" — and `NMI` on LINT1, which is
what it is wired to on every PC. Application processors mask both: the 8259
has one output, it is already going to the boot processor, and a second
processor accepting ExtINT would race it for the same interrupt and
acknowledge a controller it was not talking to.

The `smp` suite now asserts LINT0 is ExtINT and unmasked, *and* that the timer
is advancing — because the second is the property that mattered and the first
is only how it is achieved.

### The compiler unmapped a page the kernel was still using

This one is older and worse, and SMP work is what exposed it.

Both bootstrap-directory functions edit a page directory through a temporary
mapping: map the frame at a fixed kernel address, write it, unmap. The
disassembly of the first version:

```
  mov    %eax,0xfff3c000    <- temp_map:   write the PTE  (mapping ON)
  invlpg (%ecx)
  movl   $0x0,0xfff3c000    <- temp_unmap: clear the PTE  (mapping OFF)  *** HOISTED ***
  mov    0xcf000000,%eax    <- read through a mapping that is now gone   -> #PF
  movl   $0x0,0xcf000000
  invlpg (%ecx)             <- temp_unmap's invlpg, left behind
```

The store that **tore down the mapping** was hoisted above the code still
using it. The compiler is entitled to do this: it sees a store to one absolute
address and accesses to another, has no way to know that the first changes
where the second *goes*, and reorders them freely. `invlpg`'s `"memory"`
clobber constrains ordering relative to the asm — and the store stayed on its
own side of its own `invlpg`. Nothing connected it to the accesses in between.

It had been latent since the copy-on-write work, because `vmm_clone_current`,
`cow_fault` and `vmm_destroy_address_space` all use the same pattern and
happened to survive whatever GCC decided about their surrounding code. That is
the worst kind of luck.

The fix is two things, and neither is sufficient alone:

```c
/* A page table entry is not ordinary memory: writing one changes where every
 * subsequent access to a virtual address *goes*, and the compiler cannot see
 * that. */
static inline volatile u32 *pt_entries(u32 pdi)
{
    return (volatile u32 *)PT_VADDR(pdi);
}
```

`volatile` keeps page-table accesses in order relative to **each other**, so
none can be merged, elided or reordered. It says nothing about ordinary
accesses, so `temp_map` and `temp_unmap` each carry a `barrier()` — an empty
`asm volatile` with a memory clobber — that keeps accesses *through* the
mapping on the correct side of the stores that create and destroy it.

The `bootpd` suite is the regression test. It creates a bootstrap directory,
reads every entry back through a temporary mapping, and destroys it; if either
half of the fix is removed the failure is immediate and fatal rather than
subtle.

---

## Testing

Every scenario except one runs on a single processor, because that is the path
that has to keep working. `four-processors` boots the identical test image with
`-smp 4`, and nothing a uniprocessor boot touches is exercised by it: the
trampoline, the per-CPU GDT entries, the real spinlocks and the IPI paths all
only exist when there is more than one processor.

The `smp` suite runs on both and checks either the real thing or the absence
of it — never "skip", which is how a test suite quietly stops testing
anything.

| Group | What it establishes |
|---|---|
| ACPI | The root table parsed; the MADT's local APIC address is page aligned; every processor it describes has a distinct APIC id; a table that is not there comes back NULL; the FADT is findable, which proves the walk reaches more than the one table SMP needs |
| Local APIC | The task priority register is zero, so this processor does not silently refuse interrupts; the software enable and the spurious vector are what `configure_local` wrote; **LINT0 is ExtINT and unmasked**, and the timer really is advancing; no error is latched |
| Per-CPU state | `smp_cpu_index()` agrees with the APIC id; every online processor has a distinct APIC id and its index matches its slot; every application processor's stack is in the per-CPU region with an unmapped guard page below it; every application processor reached its idle loop |
| Locks | A fresh lock is unlocked; `trylock` succeeds then fails; the counters move; a lock taken with interrupts enabled gives them back and one taken with them disabled leaves them disabled |
| IPIs | Every other processor answers a ping; a shootdown is acknowledged by exactly `cpu_count - 1` processors; on one processor both paths are still callable and still invalidate locally |

---

## What is missing

- **Per-CPU run queues.** The scheduler runs on the boot processor and the
  others idle. This is the large remaining piece: a run queue per processor
  with work stealing, `current` becoming per-CPU, a reschedule IPI, and every
  scheduler invariant re-examined for two processors entering it at once.
- **The I/O APIC.** The MADT is parsed and its I/O APIC entries and interrupt
  source overrides are read — including that IRQ 0 arrives as GSI 2 on QEMU,
  which a kernel that assumed otherwise would lose its timer to. Nothing is
  programmed yet: device interrupts still go through the 8259s to the boot
  processor, so there is no interrupt distribution and no affinity.
- **The local APIC timer.** Per-CPU preemption needs it; the single 8254 cannot
  drive four processors.
- **x2APIC.** The id is read as 8 bits, which caps this at 255 processors —
  academic next to the `SMP_MAX_CPUS` of 8.
- **Lock ordering.** There is no lock hierarchy and nothing checks for one, so
  a future deadlock between two locks taken in opposite orders would be caught
  by the spin bound rather than prevented.
- **Processor hot-plug.** Processors ACPI marks online-capable-but-disabled are
  reported and left alone.
