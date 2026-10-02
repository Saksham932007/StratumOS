# Performance

The kernel measures itself. `make bench` boots an image that calibrates the
time-stamp counter against the PIT, runs eleven microbenchmarks, takes a
sampling profile, and shuts the machine down. CI runs the same image on every
push and asserts that every figure is produced.

```bash
make bench            # measurements and a profile, then exit
```
```
stratum> bench              # all of them
stratum> bench kmalloc      # one
stratum> bench list
stratum> profile run 2000   # profile a mixed workload for 2 s
```

---

## The one caveat that matters

Under emulation, a cycle count measures the emulator. The harness detects this
from the CPUID hypervisor bit and says so in its own output:

```
bench: tsc 2099.371 MHz   cpu "QEMU Virtual CPU version 2.5+"
bench: NOTE running under a hypervisor/emulator - these are emulated costs,
bench: NOTE not silicon timings. Use KVM or real hardware for absolute figures;
bench: NOTE the relative ordering is still informative.
```

Everything below was measured under QEMU's TCG interpreter. The *ratios* are
meaningful — a translation really is 20x cheaper than an allocation — but the
absolute numbers are not silicon timings. `qemu-system-i386 -enable-kvm` or a
real machine gives true figures from the same harness.

Being explicit about this is the difference between a benchmark and a
misleading number.

---

## Methodology

Three things keep the figures honest:

**The harness measures and subtracts its own cost.** An empty workload is
timed first; its cost is removed from every result. What is reported is the
operation, not the loop around it.

```
bench: harness overhead 2236 cycles per sample (subtracted)
```

**Every figure is a median of 24 samples.** A single timing on a machine with
interrupts enabled measures whatever happened to interrupt it. `min`, `median`
and `mean` are all reported, and a mean far above the median is itself the
signal that something is interfering.

**Reads are serialised.** `CPUID` is executed before and after each timed
region, because an out-of-order CPU is free to move `RDTSC` across the work
being measured otherwise.

Benchmarks run with interrupts masked except where the workload needs them —
a context switch to a partner task needs a live scheduler, so that one accepts
more noise and relies on the median to absorb it.

---

## Results

QEMU 8.2.2 TCG, calibrated TSC 2099 MHz, 128 MiB. Median of 24 samples.

| Benchmark | What it measures | Cycles | Time |
|---|---|---:|---:|
| `vmm-xlate` | virtual → physical translation | 60 | 28.6 ns |
| `heapcheck` | full heap integrity walk | 155 | 73.7 ns |
| `pmm` | physical frame alloc + free | 376 | 179.3 ns |
| `kmalloc-mx` | `kmalloc`/`kfree`, 8 mixed sizes | 413 | 196.5 ns |
| `kmalloc` | `kmalloc(64)` + `kfree` | 414 | 197.2 ns |
| `vmm-map` | map + unmap one 4 KiB page | 495 | 236.0 ns |
| `ctxsw` | one context switch | 610 | 290.4 ns |
| `ksnprintf` | format 4 conversions | 2216 | 1055.4 ns |
| `memset-4k` | `memset` of 4 KiB | 2671 | 1272.5 ns |
| `syscall` | `int 0x80` round trip | 1360 | 647.8 ns |
| `memcpy-4k` | `memcpy` of 4 KiB | 6060 | 2886.7 ns |

### What the numbers say

**`vmm-xlate` at 60 cycles** is the cheapest thing measured, and it should be:
a bounds check, then two loads — the page directory entry and the page table
entry — both reached through the recursive mapping window. No TLB involvement,
because this is a software walk.

**`pmm` at 376 cycles** for an allocate-and-free pair. The allocator scans the
bitmap a 32-bit word at a time and keeps a rotating hint, so the common case
touches one word. Most of the cost is the two function calls and the stats
bookkeeping rather than the search.

**`kmalloc` at 414 cycles**, and notably *the same* for mixed sizes (413) as
for a fixed 64 bytes. That is the signature of a first-fit allocator serving
everything from the head of the free list: the size barely matters because the
same block is reused. On a fragmented heap this would diverge sharply, which
is the argument for the slab allocator in [ROADMAP.md](ROADMAP.md).

Of those 414 cycles, a meaningful share is the guard magics — a header magic,
a footer magic, and validation on free. That is the cost of the heap catching
its own corruption, and it is worth paying.

**`ctxsw` at 610 cycles** per switch. The switch itself is nine instructions
(`pushfd`, four pushes, two stack moves, four pops, `popfd`, `ret`); the rest
is `pick_next()` walking the run queue, the state and accounting updates, and
writing the TSS's `esp0`. A per-CPU run queue with an O(1) pick — which SMP
will need anyway — is where this gets cheaper.

**`syscall` at 1360 cycles** is the most emulation-distorted figure here.
`int` and `iret` involve privilege checks, descriptor loads and stack
switching, all of which TCG emulates in software at a large multiple of their
hardware cost. On real silicon this path would be a few hundred cycles, and
`syscall`/`sysret` instead of `int 0x80` would make it far cheaper again.
Treat this one as "the shape of the path", not its price.

**`memcpy-4k` at 6060 cycles** is 1.48 cycles per byte, or ~5.9 cycles per
4-byte store. `memcpy` takes the aligned dword path here, so that is 1024
iterations of load/store/increment — about right for an interpreter. `memset`
at 2671 cycles is less than half of `memcpy`, which is exactly what you would
expect: it stores without loading.

**`ksnprintf` at 2216 cycles** for four conversions, ~550 cycles each. The
profile below shows where that goes.

---

## The sampling profiler

The timer interrupt already fires 100 times a second and already has the
interrupted `EIP` in its register frame. Attributing that address to a function
costs one binary search over the embedded symbol table, so a statistical
profiler is nearly free.

```
stratum> profile run 1500
```
```
Sampling profile
  sample rate   : 100 Hz (the timer tick)
  duration      : 70 ticks (~700 ms)
  kernel samples: 70 attributed, 0 unattributed
  ring 3        : 0
  idle          : 0

  SAMPLES   SHARE  FUNCTION
       40   57.1%  kmalloc
       16   22.8%  kfree
       13   18.5%  emit_number
        1    1.4%  ksnprintf
```

That is a workload of allocator traffic plus some integer formatting, and the
profile finds exactly that: `kmalloc` and `kfree` dominate, and `emit_number` —
the digit-conversion loop inside the formatter, which the caller never names —
accounts for almost all of the `ksnprintf` time. A profiler that could not
separate `emit_number` from `ksnprintf` would not be telling you anything you
did not already know.

CI asserts on that last property: the benchmark scenario requires `kmalloc` to
appear at the top of the profile, so a regression that broke attribution would
fail the build rather than quietly produce a flat profile.

### Honest limits

- **100 Hz is coarse.** A profile needs seconds of runtime to say anything,
  and anything that takes less than ~10 ms is invisible. Real profilers sample
  on a performance-counter overflow at kHz rates; raising the timer frequency
  while profiling would be the cheap improvement.
- **Samples are attributed to the leaf function only.** There is no call-graph
  attribution, so a helper called from several places aggregates all of its
  callers together. Walking the frame-pointer chain at sample time would fix
  that — the backtrace code already exists.
- **Ring 3 samples are counted but not attributed.** A user `EIP` means nothing
  in the kernel's symbol table, and guessing would blame whatever kernel
  function sits at that address.
- **The idle task is excluded** from the hot list and counted separately.
  Time spent idle is worth knowing but is not a hotspot.

---

## The symbol table this depends on

Both the profiler and panic backtraces need to turn an address into a name
without an external tool. The table is generated from the linked kernel and
embedded on a second link pass — which is circular, because embedding it
changes the addresses it describes.

Three passes converge:

```
A  link against an empty table   -> enumerate the symbol NAMES
B  link against the real table   -> addresses settle, because the table's
                                    size depends on names and count, not values
C  regenerate and relink         -> byte-identical layout to B
```

Pass C then **verifies** that the embedded table still describes the kernel it
is embedded in, rather than trusting that argument:

```
  LD      pass A (enumerate symbols)
  KSYMS   559 symbols -> build/ksyms_a.c
  LD      pass B (addresses settle)
  KSYMS   559 symbols -> build/ksyms_b.c
  LD      build/stratum.debug.elf (pass C, final)
  VERIFY  embedded symbol table describes this kernel
```

Only *function* symbols are emitted. That is the constraint that makes the
argument hold: data symbols would include the table's own, so the set of names
would differ between passes B and C and the size would change again.

The 559 symbols cost 11.3 KiB of the image - 4.4 KiB of address/pointer
pairs plus 6.9 KiB of names. In exchange, a panic is
readable with nothing but the serial log:

```
  EIP 0010cab4  CS  0008      EFLAGS 00000286
  at  cmd_fault+0x124
  ...
Call trace (return addresses; the faulting frame is EIP above):
  [0] 0x0010de26  shell_run_line+0xe6
  [1] 0x0010e3c7  shell_task+0x507
  [2] 0x00101b02  thread_trampoline+0xb
```

---

## Reproducing this

```bash
make bench                                  # under TCG, as above
qemu-system-i386 -enable-kvm -m 128M \
    -display none -serial stdio \
    -drive format=raw,file=build/stratum-bench.img,index=0,media=disk
```

With KVM the figures become real, and the harness drops the emulation note and
prints `bench: bare metal - figures are real silicon timings` instead.
