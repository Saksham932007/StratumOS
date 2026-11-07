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

### The hierarchy, and why it is checked rather than documented

Two locks are enough to deadlock. One processor takes the heap's and wants
the VMM's; another takes the VMM's and wants the heap's; neither ever
finishes. Nothing about either acquisition is wrong on its own, which is what
makes the bug hard: it is a property of the pair, and it only shows up when
both happen at once, which under emulation may be never and on real hardware
may be Tuesday.

The standard answer is to fix a global order and acquire only upwards in it.
This kernel does that, and then checks it, because a documented order that
nothing enforces is a comment:

| Level | Lock | Protects |
|---|---|---|
| 1 | `sched` | the run queues, the task table |
| 2 | `fs` | descriptor tables, the VFS, FAT, the ATA driver |
| 3 | `net` | interfaces, sockets, the TCP and ARP tables |
| 4 | `heap` | the free list and the block headers |
| 5 | `vmm` | the page tables, the temp slots, the MMIO window |
| 6 | `pmm` | the frame bitmap and the refcounts |
| 7 | `console` | the VGA buffer and the UART |

The order is not arbitrary — it is the order the subsystems already call each
other in. The filesystem allocates buffers, so `fs` is above `heap`. The heap
grows by mapping pages, so `heap` is above `vmm`. Mapping a page takes a
frame, so `vmm` is above `pmm`. And anything at all may print, which is why
the console is last: taking the console lock has to be legal from under every
other lock, and nothing the console calls takes a lock of its own.

Each `spinlock_t` carries its level, each processor records the highest level
it currently holds, and `spin_lock()` panics if the new level is not strictly
above it:

```
cpu 2 tried to acquire lock 'heap' at level 4 while already holding level 5;
that inverts the lock hierarchy and would deadlock against a processor doing
it the other way round
```

That panic fires on the *first* processor to do it, alone, in a test run that
would otherwise pass — rather than as a hang on a customer's machine when a
second processor finally does the mirror image. Locks declared with
`SPINLOCK_INIT` instead of `SPINLOCK_INIT_AT` have level 0 and are exempt,
which is right for a lock that nests under nothing, like the TLB shootdown's.

The panic releases the lock before printing, because printing takes the
console lock.

### What the wrappers are for

Putting a subsystem behind one lock means every public entry point has to take
it, and the entry points call each other. `kzalloc()` is `kmalloc()` plus a
`memset`; `pmm_free_frames()` is a loop over `pmm_free_frame()`. If those
inner calls go through the public function they take the lock a second time,
and this lock answers that with a panic.

So each subsystem's bodies are `static` and named `_locked`, the public names
are thin wrappers that take the lock and call them, and the bodies call each
other's `_locked` form directly. It is more lines than a recursive lock would
be, and it is the right trade: a recursive lock would make all of this
compile and hide exactly the nesting the hierarchy check exists to find.

The network stack is the one exception, and it is a considered one. There are
some ninety entry points into it — the shell drives ARP, ICMP, UDP, TCP, DNS
and HTTP directly, and the socket layer drives the same functions from a
system call — and they call each other freely. Ninety duplicated functions
would be ninety chances to call the wrong one, which is a worse bug than the
one the discipline prevents. So `net_lock()` counts its depth, and an entry
point is correct whether it was reached from inside the stack or outside it.
The ordering guarantee is unaffected: the level is recorded on the outermost
acquisition only, so the network lock is still required to be taken between
the filesystem's and the heap's. Nothing else in the kernel gets this, which
is why it is written down here rather than left to be discovered.

Two callers are deliberately not wrappers:

- **The page-fault handler** takes the VMM lock itself, around the
  copy-on-write break only, and releases it before the path that can reach
  `task_exit()`. Killing a task takes the scheduler's lock, which is *below*
  the VMM's — holding both in that order is what the check forbids, and
  `task_exit()` never returns, so the lock would also never be released.
- **The console** takes its lock only if this processor does not already hold
  it. A panic prints, and the panicking processor is quite likely to be
  holding the console lock already; spinning on itself there would replace a
  readable panic with a silent hang. `spin_held_by_self()` exists for this one
  caller and should not acquire a second.

`kprintf()` brackets its whole format with one acquisition rather than letting
the per-character sink take it 80 times, because per-character atomicity lets
two processors interleave a line character by character, which is worse than
useless.

### What the check found on its first day

The hierarchy was not a formality. Within minutes of the filesystem and the
network stack going behind locks, the kernel refused to boot:

```
cpu 0 tried to acquire lock 'fs' at level 2 while already holding level 3;
that inverts the lock hierarchy and would deadlock against a processor doing
it the other way round

  [0] level_enter+0x67
  [1] fs_lock+0x12
  [2] fd_table_init+0x11
  [3] task_create+0x163
  [4] net_start+0x2f
```

`net_start()` creates the network thread, creating a task allocates a
descriptor table, and allocating a descriptor table is filesystem work. The
network lock was being held across it for no reason at all — `net_start()`
runs once, at boot, before there is a second thread to exclude.

The second finding was structural rather than incidental, and worth more. The
socket layer looks like part of the network stack and is not: it is glue
between two subsystems. `socket_create()` allocates a socket, then an open
file, then a descriptor — net, then fs, then fs. `sock_read()`, the VFS
operation, runs the other way round: the filesystem took its lock at the
boundary and now calls TCP. Both orders are natural and together they are a
cycle, which is exactly the shape of bug a hierarchy exists to make
impossible.

So the socket layer holds no lock of its own. It resolves a descriptor
through the filesystem, which locks, and then calls TCP or UDP, which lock;
the only state that is actually its own is the socket table, and that is
locked in the three functions that touch it. The cycle disappears because
nothing holds both locks at once.

Neither of these would have been found by testing. The kernel boots on one
processor either way, and on four it boots too — the window is a handful of
instructions wide. They were found because the machine was asked to check an
invariant rather than a behaviour.

### Sleeping with a lock held, which is to say not doing that

A lock that masks interrupts must not be held across anything that blocks.
Three paths in this kernel do block, and each is a case where the obvious
code is wrong:

- A **blocking console read** reaches `keyboard_getchar()` from inside
  `vfs_read()`, with the filesystem lock held. It would wait for a keystroke
  with the whole filesystem shut to every other processor — and `sched_yield()`
  takes the scheduler's lock, which is below the filesystem's, so the check
  would catch it first.
- A **blocking socket receive** sleeps waiting for a segment that arrives
  *through the stack it has locked*. That is not a performance problem, it is
  a deadlock that presents as a timeout.
- A **blocking accept** is the same, one layer up.

Each drops its locks around the sleep, innermost first, and takes them again
in hierarchy order after — `fs_sleep_begin()`/`fs_sleep_end()` and
`net_sleep_begin()`/`net_sleep_end()`. The network pair returns the *depth*,
not a flag, because that lock counts; the filesystem pair returns whether it
actually released, because a caller may not have held it.

What a sleep window costs is the invariant the lock was providing: anything
read before it must be read again after. In all three cases the code already
re-tests its condition in a loop, which is why these were the right places
to open one.

### What locking did to the profiler

Every acquisition masks interrupts and every release restores them. A timer
tick that arrives while a lock is held is not delivered where it happened: it
waits for the `sti` in the release path, and the processor takes it with EIP
pointing at that instruction.

Once the heap, the page tables and the frame allocator each took a lock, the
sampling profiler reported this:

```
  SAMPLES   SHARE  FUNCTION
       53   75.7%  spin_unlock
       16   22.8%  emit_number
        1    1.4%  kfree
```

Three quarters of the kernel's time in `spin_unlock`, which is a dozen
instructions. The measurement was wrong, not the kernel: every deferred tick
in the system was landing on the same instruction. The profiler now follows
the frame-pointer chain up out of the interrupt window — `spin_lock`,
`spin_unlock`, `irq_save`, `irq_restore` — and charges the sample to the
caller, which gives the honest answer:

```
  SAMPLES   SHARE  FUNCTION
       28   40.0%  autobench_task
       22   31.4%  kmalloc
       20   28.5%  emit_number
```

Samples whose chain cannot be followed are counted as `deferred` and reported
separately, rather than attributed to a guess.

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

## The local APIC timer

The 8254 has one output and it is wired to one processor. So a kernel whose
only clock is the PIT can only ever preempt on the boot processor, however
many it has brought up — which is most of why the other three idled.

Each local APIC has a timer of its own: a 32-bit count that decrements at the
bus clock divided by a programmable divisor, and reloads itself in periodic
mode. The only awkward part is that nothing reports what the bus clock is, so
it has to be measured. The PIT is the reference:

```c
apic_write(APIC_TIMER_DIV, APIC_TIMER_DIV_16);
apic_write(APIC_LVT_TIMER, APIC_LVT_MASKED);
apic_write(APIC_TIMER_ICR, 0xFFFFFFFFu);

timer_busy_wait_ms(CALIBRATE_MS);

u32 elapsed = 0xFFFFFFFFu - apic_read(APIC_TIMER_CCR);
u64 bus_hz = (u64)elapsed * 16u * 1000u / CALIBRATE_MS;
```

Masked while calibrating, so a tick cannot be delivered on a vector whose
handler is not installed yet. The divisor of 16 is what keeps a 32-bit count
from wrapping during the measurement at a gigahertz bus clock, and 50 ms is
five PIT ticks, so the quantisation error is under one percent. On QEMU this
measures around 835 MHz, and it is measured on every boot rather than cached,
because it is a property of the machine.

A result that cannot be right is **reported rather than used**. Below 1 MHz
means the counter barely moved, which is what a timer the hypervisor does not
implement looks like; the kernel then keeps preempting from the PIT and the
boot log says so, rather than silently running with no tick on three of four
processors.

Application processors do not calibrate. Every processor in a machine shares
the bus clock, so an AP measuring its own would be measuring the same number
more slowly — and would have to do it with the PIT, which is not its to use.
`apic_timer_init_ap()` just programs the register from the boot processor's
figure.

Two consequences worth stating:

- **The PIT stays.** It remains the kernel's timebase — `timer_ticks()`, the
  uptime and every sleep deadline — because one monotonic clock is worth more
  than symmetry, and because a machine with no usable local APIC still has to
  keep time. What moved is preemption, and only preemption, which is why the
  PIT handler now asks `apic_timer_running()` before calling `sched_tick()`.
  Without that the boot processor would be preempted twice as often as every
  other one, for no reason.
- **The EOI goes first.** In this handler, unlike the IPI handlers, the
  acknowledgement happens before the scheduler is called. Preemption is
  performed at the tail of the interrupt path, and a processor that switched
  away before acknowledging its own APIC would leave the interrupt in
  service — so the next tick would never arrive and that processor would stop
  preempting for good. Exactly the failure the note at the top of
  `core/sched.c` describes, one controller along.

The evidence is per-processor, because a summed count would be satisfied by
one busy CPU. `struct cpu` carries its own `timer_ticks`, the `smp` suite
asserts every online processor's is non-zero, and `cpus` prints the column:

```
stratum> cpus
Local APIC
  enabled   : id 0, version 14; 7 IPI(s) sent, 412 EOI(s), 0 spurious, 0 error(s)
  timer     : 100 Hz on every processor, bus clock 835.322 MHz measured; 1648 tick(s)
Processors (4 online of 4 reported)
   CPU  APIC  ROLE STATE          STACK    PINGS      TLB      TICKS   IDLE LOOPS
     0     0   bsp online    0xe8104000        0        0        412        91823  <- this one
     1     1    ap online    0xe8204000        1        0        412      1402119
     2     2    ap online    0xe8304000        1        0        412      1398774
     3     3    ap online    0xe8404000        1        0        412      1401002
```

On a kernel preempting from the 8254 alone, three of those four columns are
zero and stay zero.

---

## Scheduling on every processor

The run queue used to be the boot processor's. Every other processor came up,
loaded its descriptor tables, started its timer, serviced IPIs — and idled,
because `current` and `idle` were two file-scope variables in `sched.c`, and a
machine with one of each can run one task at a time.

They are fields of `struct cpu` now, along with `need_resched`, a switch
count, and one more that turns out to be the whole design: `prev`.

### One queue, not one per processor

"Per-CPU run queues" is the usual phrase and this is not that, deliberately.

A queue per processor buys two things: no contention on a shared lock, and
cache locality from a task tending to return to the processor whose cache
holds its data. It costs a balancer — code that moves tasks between queues,
decides when to, and is where every scheduler that has one keeps its hardest
bugs.

This kernel has 32 task slots and at most 8 processors. Selection under the
lock is a walk of a short list, and the contention a shared queue would have
to show before a balancer earned its complexity is not reachable at this
size. So: one ring threaded through `task->next`, one global round-robin
cursor, one lock, and no affinity. The processor a task last ran on is
recorded in `task->cpu` and reported by `ps`, not respected.

The cursor is global rather than per-processor on purpose. Two processors
picking at the same instant advance the same cursor and get different tasks,
rather than both starting at the head of the queue and one of them losing.

Idle tasks are not in the ring at all. An idle task belongs to exactly one
processor — it owns that processor's stack and cannot run anywhere else — so
it is the explicit fallback rather than a last entry that must never be
chosen. Each occupies the `tasks[]` slot with its processor's index, which
makes the processor-to-idle-task mapping impossible to get wrong, and each is
pid 0, because pid 0 is how the rest of the kernel asks "is this a real
task".

An application processor builds nothing itself. It waits in `ap_main()` for
`sched_ready_for_aps()`, which `sched_init()` sets with a release after it has
built an idle task for every processor already online; the acquire on the
other side is what guarantees the processor can see the task that was made for
it. Then it calls `sched_start()` — which is to say, it *is* its idle task,
standing on the stack it has been standing on since the trampoline.

### The lock cannot be held across the switch

This is the part that took the care, and it is not obvious.

A `spinlock_t` records the interrupt state it found in the lock. A context
switch hands control to a task that will eventually release that lock from a
different stack, with a different interrupt state, having arrived there from a
different place. Holding the scheduler's lock across `context_switch()` means
the release happens somewhere the acquisition cannot reason about.

So `do_switch()` lets go first. In order:

1. `next` is marked RUNNING **inside** the lock, so no other processor can
   pick it between here and the stack swap.
2. `prev` is left marked RUNNING and recorded as this processor's `prev`. It
   is therefore unpickable too.
3. The lock is released. Interrupts stay masked, because every acquisition of
   this lock is wrapped in an explicit `irq_save()` and the release of a lock
   taken with interrupts already off is a no-op for the interrupt flag.
4. The stacks are swapped.
5. Whoever resumes calls `sched_handoff()`, which marks the processor's `prev`
   READY — now that its stack pointer has actually been written.

**Step 2 is the step that is easy to miss.** Marking `prev` READY before the
switch is the obvious thing to write, and it is a memory-corruption bug on a
machine with two processors: the other one picks the task up and begins
restoring a context this one has not finished saving. The symptom is a task
resuming with a stack pointer from the middle of a switch, which is to say no
symptom you could read.

`sched_handoff()` is also why both assembly trampolines changed.
`thread_trampoline` and `fork_trampoline` are where a brand-new task and a
forked child arrive, and neither arrives through `do_switch()`, so each has to
discharge the obligation itself. Both now have `IF` clear in the EFLAGS word
their stack was built with — `0x002`, not `0x202` — call `sched_handoff()`
first, and only then let interrupts in.

The same reasoning fixed a second-order bug that predated SMP.
`task_sleep_ms()` and `task_block()` used to set the state and then call
`sched_yield()`, two critical sections with a gap between them. In that gap
the task is marked SLEEPING and still running, and another processor's
`wake_sleepers()` can set it READY and run it — in two places at once. Both
now go through `sched_switch_away(state, deadline)`, which parks the task and
switches under one acquisition.

### Four bugs that only a second processor could find

Three of these were already in the kernel. The scheduler change did not cause
them; it made them reachable.

**The lock's own recursion check was broken.** `spin_unlock()` never cleared
`holder`, and the "nobody holds it" value was `0` — a perfectly good processor
index. So an untaken lock looked like a lock held by processor 0, and, worse,
between a processor winning the exchange and recording itself as the holder,
whichever processor had held the lock last would see its own index there and
panic with "recursively acquired". The window is a handful of instructions.
Finding it needs a contended lock and a second processor, which is exactly
what the scheduler's lock became:

```
cpu 3 recursively acquired lock 'sched'
  [1] sched_switch_away+0x29
  [2] demo_worker+0xab
  [3] thread_trampoline+0x10
```

The sentinel is `SPIN_NO_HOLDER` now, and the holder is cleared before the
lock word rather than left stale.

**`sched_preempt()` was only called for vectors 32–47.** The interrupt
dispatcher called it at the tail of the hardware-IRQ path — the 8259s' range —
and returned straight out of the `handlers[]` path, which is where a local
APIC interrupt goes. The APIC timer is vector `0xF3`. So an application
processor set `need_resched` a hundred times a second and nothing ever read
it, and every task ran on the boot processor: the exact bug the per-CPU run
queue was meant to fix, hiding one layer further down. Exceptions still never
preempt — a fault is not a scheduling opportunity.

**SMEP, SMAP and CR0.WP are per-processor bits** and only the boot processor
ever set them. Harmless while the other processors idled. The moment they ran
tasks, the hardening suite — which reads CR4 on whichever processor it happens
to be running on — started failing on three cores out of four. `harden_init_ap()`
now sets them on each processor and the kernel panics if one cannot match what
the boot processor reported: W^X enforced on one processor out of four is not
W^X enforced.

**Three tests had encoded "the suite runs on the boot processor."** One
asserted `self->is_bsp`, with a comment reading "the test suite runs on the
boot processor, because that is where the scheduler runs" — true when it was
written, and the whole defect. One asserted LINT0 was unmasked ExtINT, which
is correct for the boot processor and wrong for every other. One counted IPI
ping answers as `count - 1` while filtering out the boot processor rather than
the *sender*. All three now assert the per-processor truth, which is a better
test than the one that passed.

### What the evidence looks like

A summed switch count is satisfied by one busy processor, so the `sched` suite
starts six tasks — more than there are processors — has each record which
processor it is on, and logs the distribution:

```
ktest parallel: 240 slices over 3 processor(s): 120/80/0/40
  cpu 0: online 1 bsp 1 switches 49  ticks 70 resched 1  current autotest
  cpu 1: online 1 bsp 0 switches 28  ticks 69 resched 15 current idle
  cpu 2: online 1 bsp 0 switches 140 ticks 69 resched 14 current idle
  cpu 3: online 1 bsp 0 switches 127 ticks 69 resched 15 current idle
```

The assertion is on the switch counts, not on where those six tasks landed.
Which processor picks a task up is a race by design, and a later run put all
240 slices on one processor — which is legitimate, and is what turned this
from a check on the spread into a check on switch counts. A processor's switch
count is non-zero only if it has run something other than its own idle task,
and that is the claim worth making.

`cpus` prints the same columns from the shell.

### The reschedule IPI

A processor that makes a task runnable and is busy with something else has to
tell the others, because an idle processor is sitting in `HLT` and will
otherwise find out at its next tick. On a machine with three idle processors
and a 100 Hz tick, a task woken by an arriving network segment would wait up
to 10 ms to start running, which is absurd.

`APIC_VECTOR_IPI_RESCHED` is a broadcast sent by `task_create()`,
`task_fork()` and `task_unblock()`. The handler sets `need_resched` and
acknowledges; it does not schedule. The switch happens at the tail of the
interrupt path like every other preemption, which is the one place in the
kernel where the stack is known to be in a state a switch can survive.

The idle loop is `sti; hlt` with nothing between the two instructions, because
the processor guarantees that an interrupt pending at the `STI` is taken after
the `HLT` is entered. Written the other way round, a task made runnable in the
gap would wait for the next tick.

### What this did to user space

Two processes now genuinely run at the same instant, and the first thing that
noticed was the ring-3 `fork` demo, whose output came back spliced:

```
[child] fork() returned 0 here; my pid is   [ring3] fork() returned 77, my parent is  here
```

Not a kernel bug. The child built that line out of five `write()` calls and
the parent interleaved at a syscall boundary, which is what sharing a
descriptor means. A program that wants a whole line has to write a whole line,
so `user/syscall.h` grew `u_line_*`: build the line in your own memory, spend
one `write()` on it. The kernel has done the same thing inside `kprintf()`
since the console got a lock.

---

## What is missing

- **Affinity and a balancer.** The run queue is shared and there is no
  affinity: a task goes to whichever processor asks next, so it loses its
  cache every time it moves. The fix is a queue per processor and a balancer,
  and the reason it is not here is in "One queue, not one per processor"
  above — at 32 task slots and 8 processors the contention a balancer would
  relieve is not reachable. The number to watch is the `sched` lock's
  contention count, which `locks` prints.
- **Priorities.** Round robin with one quantum for everything. A task that
  wakes from a 1 ms sleep waits behind whatever was already runnable, and
  there is no way to say that one of two tasks matters more.
- **Interrupt-driven ATA.** The disk driver busy-polls, so a read holds the
  filesystem lock with interrupts masked for its duration. Correct, and not
  kind: on four processors it is the one lock that can be held for
  milliseconds.
- **The I/O APIC.** The MADT is parsed and its I/O APIC entries and interrupt
  source overrides are read — including that IRQ 0 arrives as GSI 2 on QEMU,
  which a kernel that assumed otherwise would lose its timer to. Nothing is
  programmed yet: device interrupts still go through the 8259s to the boot
  processor, so there is no interrupt distribution and no affinity.
- **x2APIC.** The id is read as 8 bits, which caps this at 255 processors —
  academic next to the `SMP_MAX_CPUS` of 8.
- **Processor hot-plug.** Processors ACPI marks online-capable-but-disabled are
  reported and left alone.
