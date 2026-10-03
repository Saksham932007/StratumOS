# Testing

Five layers, ordered cheapest-first so a trivial mistake fails in a second
rather than after an emulator run.

| Layer | Needs | Runtime | Catches |
| --- | --- | --- | --- |
| Host unit tests | nothing | ~1 s | formatter, string, 64-bit division bugs |
| Pre-boot validation | the linker | ~0.1 s | unbootable images (20 failure conditions) |
| In-kernel suites | QEMU | ~5 s | allocator, paging, heap, scheduler, syscall, symbol and profiler bugs |
| Boot scenarios | QEMU + GRUB | ~6 min | regressions in either boot path, shell behaviour, measurement |
| Fuzzing | clang | ~2 min | what nobody thought to test: the parsers and the syscall boundary |

```bash
make test          # layers 1 to 4
make test-host     # layer 1 only, no emulator
make test-boot     # layers 3 and 4
make fuzz          # layer 5, bounded
make test-riscv64  # the second architecture, 58 checks
make portability   # how much of the kernel builds for riscv64
```

`make test` covers the x86 kernel. The riscv64 target has its own harness
(`tools/run-riscv64.py`) for the same reason it has its own build: the x86
one knows about disk images, two boot protocols and a serial console driven
command by command, and a kernel started directly from an ELF with no
firmware underneath has none of those. 17 expectations and an exit status,
unattended. See [PORTING.md](PORTING.md).

The first four layers test the kernel against inputs someone thought of. The
fifth tests it against inputs nobody thought of, which is a different claim
and has its own document: **`docs/FUZZING.md`**. It has found six real bugs so
far, every one of them in code the other four layers covered and passed.

---

## Layer 1: host unit tests

`tests/host/test_printf.c` compiles the kernel's **real** sources —
`kernel/core/printf.c`, `string.c`, `div64.c` — for the host as 32-bit
objects, stubs out `console_putc`, and diffs the output against the system
libc:

```c
#define CHECK_FMT(fmt, ...)                                                   \
    do {                                                                      \
        char mine[256], theirs[256];                                          \
        ksnprintf(mine, sizeof(mine), fmt, __VA_ARGS__);                      \
        snprintf(theirs, sizeof(theirs), fmt, __VA_ARGS__);                   \
        if (strcmp(mine, theirs) != 0)                                        \
            fail(fmt, theirs, mine);                                          \
    } while (0)
```

92 checks cover integer conversions, width and precision, the `-`/`0`/`+`/
`#`/space flags, `*` width and precision, 64-bit values (which exercise
`div64.c`'s shift-subtract division), truncation and return values, unknown
conversions, and the string routines including `memmove` overlap in both
directions and `str_to_u32` overflow rejection.

Compiling with `-m32` matters: it is the kernel's real ABI, so `size_t` and
pointer widths match what runs on the metal.

Printf bugs are miserable to find from inside a VM and there is no reason to
look for them there — the code is pure and testable.

### Adding a case

```c
static void test_my_thing(void)
{
    puts("formatter: my thing");
    CHECK_FMT("%+.3d", 7);              /* diffed against glibc */
    CHECK_EQ_STR("label", got, "want"); /* exact expectation */
    CHECK_TRUE("label", condition);
}
```

Then call it from `main()`.

---

## Layer 2: pre-boot validation

`tools/check-kernel.py` runs as part of the link step. A successful link says
almost nothing about whether the result can boot, so this checks the things
whose failure modes are otherwise silent:

- the Multiboot2 magic exists, is 8-byte aligned, is inside the 32 KiB window
  a loader searches, and its four header words really sum to zero;
- the ELF is 32-bit, little-endian, `ET_EXEC`, `EM_386` — a PIE would link
  fine and not boot;
- the entry point is inside a loadable segment;
- the lowest load address is 1 MiB, which both boot paths assume;
- `.bss` is 4-byte aligned at both ends, because `_start` clears it in bulk
  and would otherwise miss the tail;
- `.user` is page-aligned at both ends — its pages get remapped ring-3
  readable, so a partial page would expose adjacent kernel memory to
  userspace;
- no SSE or MMX instructions are present. The kernel never sets `CR4.OSFXSR`,
  so any the compiler emitted would raise `#UD`. `-mgeneral-regs-only` is
  supposed to prevent that; this verifies rather than trusts.

```bash
make sections     # prints everything it verified
```

```
entry point 0x00101000
6 PT_LOAD segment(s), 117 KiB resident
Multiboot2 header at offset 0x1000, 48 bytes, checksum valid
.bss is 45 KiB
.user is 4096 bytes at 0x114000
no SSE/MMX instructions (good: CR4.OSFXSR is never set)
```

Each of these has cost somebody an afternoon of QEMU bisection somewhere.
Checking them takes 30 milliseconds.

---

## Layer 3: in-kernel suites

`kernel/core/ktest.c` holds 21 suites and 633 assertions, run against
real hardware state — a bitmap with actual firmware-reported memory in it, real
page tables, a real heap, a real scheduler.

```
stratum> selftest
ktest: running 21 suites
ktest: string ... PASS (15 checks)
ktest: boot ... PASS (26 checks)
ktest: cpu ... PASS (8 checks)
ktest: pmm ... PASS (13 checks)
ktest: vmm ... PASS (32 checks)
ktest: heap ... PASS (57 checks)
ktest: irq ... PASS (9 checks)
ktest: sched ... PASS (10 checks)
ktest: syscall ... PASS (8 checks)
ktest: elf ... PASS (14 checks)
ktest: vmspace ... PASS (42 checks)
ktest: proc ... PASS (14 checks)
ktest: harden ... PASS (24 checks)
ktest: storage ... PASS (44 checks)
ktest: fs ... PASS (56 checks)
ktest: smp ... PASS (74 checks)
ktest: bootpd ... PASS (17 checks)
ktest: longmode ... PASS (51 checks)
ktest: net ... PASS (98 checks)
ktest: ksyms ... PASS (12 checks)
ktest: profile ... PASS (9 checks)
ktest: summary 21/21 suites passed
```

That transcript is from a run with four processors on a machine that has
x86-64, which is the configuration where every suite takes its widest path.
Three of them scale with the machine: `smp` checks each processor's APIC id,
stack, TSS and online transition, so one processor gives 49 rather than 74;
`longmode` gives 17 rather than 51 where the processor has no long mode to
enter; and `net` gives 63 rather than 98 where there is no Ethernet
controller. On one processor with no long mode the total is 539.

Each of those three logs which path it took, and the scenario that exercises
the wider path asserts the *check count* — see
[DESIGN-DECISIONS.md](DESIGN-DECISIONS.md) entry 34 for why that matters more
than it sounds.

| Suite | What it establishes |
| --- | --- |
| `string` | the formatter and string routines behave on the metal too |
| `boot` | the memory map was normalised, the linker symbols are sane and ordered |
| `cpu` | CPUID works, `CR0.PE`/`CR0.PG` are set, `CS` says ring 0 |
| `pmm` | allocation is page-aligned and never from the reserved first MiB; frees restore the count exactly; contiguous runs really are contiguous |
| `vmm` | the null page is unmapped and page 1 is not; mappings work when written through; offsets survive translation; `PTE_OWNED` governs whether unmap frees; `vmm_protect` keeps the frame |
| `heap` | payloads are 8-byte aligned; neighbours are not disturbed; coalescing returns merged space; realloc preserves contents; `kmalloc(0)`, `kfree(NULL)` and `krealloc(p, 0)` behave; 32 interleaved allocations freed in a different order leave the arena consistent |
| `irq` | the timer is advancing, no spurious interrupts accumulated, `int3` survives a full round trip through the stub and `iret`, `irq_save`/`irq_restore` nests |
| `sched` | a created task actually runs, switch counts rise, and a 60 ms sleep takes at least 50 ms |
| `syscall` | `user_range_ok` rejects kernel addresses, address-space wraps, unmapped pages, and ranges that *straddle* the kernel boundary |
| `elf` | a deliberately corrupted image is refused for each of eight reasons, each with a stated cause |
| `vmspace` | a fresh address space has the kernel's half and an empty user half; a clone marks the page read-only and COW in *both* copies; the reference count rises to 2; the first write allocates exactly one new frame and the second does not fault; destroying the clone drops the shared frame to zero references; and the frame count returns to where it started |
| `proc` | a kernel thread shares the kernel's page directory; `task_fork(NULL)` is refused rather than reading address zero; `wait()` returns a real pid and status for each child and -1 once there are none; `exec`'s program table contains `init` and terminates |
| `harden` | `CR0.WP` is set; no page of `.text` or `.rodata` is writable or user-accessible and `.data` still is; every kernel directory entry is present and none has the `USER` bit; this task's stack is in the guarded region with its guard page unmapped above and below; CR4 agrees with CPUID about SMEP and SMAP; a declared user access opens a window and an undeclared one is what SMAP exists to refuse |
| `storage` | IDENTIFY reports a model and a capacity; a two-sector read equals two one-sector reads *and* the two sectors differ, which is the per-sector DRQ handshake; five malformed reads are each refused *and* logged; zero sectors is a no-op that logs nothing; a read of a partition's block 0 equals a read of the disk at the partition's first LBA; a read past the partition's end is refused even though those sectors exist |
| `smp` | the MADT's local APIC address is page aligned and every processor it describes has a distinct APIC id; a table that is not there comes back NULL; the task priority register is zero; **LINT0 is ExtINT and unmasked, and the timer is advancing**; `smp_cpu_index()` agrees with the APIC id; every application processor's stack has an unmapped guard page below it and reached its idle loop; a lock taken with interrupts enabled gives them back; every other processor answers a ping and acknowledges a shootdown |
| `bootpd` | the AP bootstrap directory identity-maps the first 4 MiB, its recursive entry points at itself rather than the kernel's, its kernel half matches entry for entry, its user half is empty, and destroying it returns exactly the two frames it allocated — which is also the regression test for a compiler-reordering bug, see docs/SMP.md |
| `fs` | the BPB's four offsets are ordered and inside the device; the cluster count is in FAT16's range; lookup is case-insensitive and tolerates redundant slashes; a missing name, a relative path, an over-long component, a file used as a directory and a directory read as a file are all refused; a chunked read agrees with a whole-file read byte for byte across a cluster boundary; a read at the end is short and past the end is zero; a size bound the file exceeds refuses rather than truncates; the volume label is not reported as a file; and the sector cache's hit rate |
| `ksyms` | the table is sorted; every symbol resolves to itself; an exact address gives offset 0 and an address inside a function gives the right offset; addresses outside every executable section resolve to nothing |
| `profile` | synthetic frames are attributed correctly — ring 0 inside a known function counts, ring 3 counts separately, an address outside `.text` counts as unattributed, and a stopped profiler ignores ticks |

Tests keep going after a failure, so one run reports everything that is broken
rather than only the first thing.

### The failure format is deliberate

```
ktest: heap ... FAIL (1/57 checks failed) first: IS_ALIGNED((u32)a, 8)
```

`KT_ASSERT` stringifies the expression, so the report names the condition that
broke without needing a message argument at every call site. `run-tests.py`
asserts on these lines, so a regression fails CI rather than scrolling past.

### Adding a suite

```c
static void test_my_subsystem(struct ktest_result *r)
{
    KT_ASSERT(r, something_true());
    KT_EQ(r, computed, expected);
}

static const struct ktest tests[] = {
    ...
    { "mine", "what it checks", test_my_subsystem },
};
```

Nothing else: the count, the listing and the summary all derive from the
table.

---

## Layer 4: boot scenarios

`tools/run-tests.py` boots the kernel twelve ways and asserts on what it
says. Nine of them must come up clean; three of them must *panic*.

One of the twelve does not assert on what the kernel says at all: the
`network` scenario asserts on the bytes it put on the wire. See below.

Nine run under `qemu-system-i386`, which is the machine this kernel targets.
One - `long-mode` - runs under `qemu-system-x86_64`, because the i386 target
masks `CPUID.80000001H:EDX.LM` even with `-cpu max`, so long mode is
undetectable there. `Scenario.qemu` carries the choice, and a scenario whose
emulator is not installed is skipped with the binary's name printed, so a
skip cannot read as a pass.

### Unattended boots

The kernel accepts `autotest` on its command line, runs its suites, and then
asks QEMU to terminate through `isa-debug-exit`. No timeouts, no screen
scraping for a prompt.

```
kernel code 0x01  →  QEMU exit 3    tests passed
kernel code 0x02  →  QEMU exit 5    tests failed
kernel code 0x11  →  QEMU exit 35   panic
kernel code 0x12  →  QEMU exit 37   panic inside the panic handler
```

(QEMU exits with `(code << 1) | 1`.) Writing to port `0xF4` is inert on a
machine without the device, which is what makes it safe to leave in a normal
build — the same binary is both interactive and CI-testable.

Two separate images carry the command line, one per boot path:

```bash
build/stratum-test.img    # cmdline "autotest", via stage 2's patchable header
build/stratum-test.iso    # cmdline "autotest", via grub-test.cfg
```

Each scenario then checks 47 expected lines, 9 forbidden patterns, the
in-kernel summary, and the exit status. The forbidden list is the important
half:

```python
FORBIDDEN = [
    ("kernel panic", r"KERNEL PANIC"),
    ("unhandled exception", r"unhandled CPU exception"),
    ("page fault", r"page fault at"),
    ("test failure", r"ktest: \S+ \.\.\. FAIL"),
    ("error-level log line", r"\]\s+ERROR\s"),
    ("ring 3 escaped the pointer check",
     r"WARNING: kernel accepted a kernel pointer"),
    ("heap corruption", r"PROBLEMS FOUND"),
    ...
]
```

Each boot path also asserts on its *own* identity, so a regression that made
one silently fall back to the other is caught:

```python
("native protocol detected",     r"boot protocol\s+\[ok\] StratumOS native"),
("multiboot2 protocol detected", r"boot protocol\s+\[ok\] Multiboot2"),
```

### The same image, on four processors

Every other scenario runs on one processor, because that is the path that has
to keep working and is what almost every reader will build on. `four-processors`
boots the identical test image with `-smp 4`.

Nothing a uniprocessor boot touches is exercised by it. The trampoline, the
per-CPU GDT entries, the real spinlocks and the IPI paths only exist when
there is more than one processor, and the `smp` suite's second half — pings
answered, shootdowns acknowledged — can only run there:

```python
("the MADT was parsed",
 r"acpi: MADT: 4 processor\(s\), \d+ I/O APIC\(s\)"),
("every application processor started",
 r"smp: cpu 3 online \(APIC id 3\)"),
("all four are online", r"smp: 4 of 4 processor\(s\) online"),
```

### The same image, on a CPU that has SMEP and SMAP

QEMU's default i386 model does not implement CPUID leaf 7, so SMEP and SMAP
cannot be detected and the kernel takes its fallback path. That path is worth
testing — it is what runs on any pre-2012 machine — but it means the other
half of the code never executes.

So the `hardened-cpu` scenario boots the *same* image with `-cpu max` and
requires both features to come up:

```python
("SMEP and SMAP both enabled", r"harden: SMEP enabled, SMAP enabled"),
("the boot step says so",
 r"hardening\s+\[ok\] W\^X, guard pages, SMEP \+ SMAP"),
```

With SMAP on, every place the kernel touches user memory must have declared
the access with `stac`/`clac`, and a missing declaration is a page fault. So
this scenario is a test of that discipline rather than of a log line — and it
earned its keep on its first run, by faulting the `vmspace` suite, which had
never declared the user-page write it uses to prove copy-on-write works.

### The interactive scenario

41 shell commands, sent over the serial console and checked against regular
expressions:

```python
("meminfo", [r"Physical memory", r"Kernel heap",
             r"integrity : consistent", r"Firmware memory map"]),
("ps",      [r"PID\s+NAME\s+STATE", r"\bidle\b", r"\bshell\b"]),
("selftest heap", [r"ktest: heap \.\.\. PASS"]),
("ring3",   [r"\[ring3\] hello from user mode",
             r"\[ring3\] calling exit\(0\)"]),
```

**Piping a script into the serial port does not work**, and finding out why is
instructive. The 16550's receive FIFO is 16 bytes, and the kernel does not
start reading it until the shell is up — roughly 100 ms in. Everything typed
before then is lost to a receive overrun, so a piped script silently loses its
first few dozen bytes:

```
stratum> pagemap 0x100000      ← the first six commands simply vanished
```

So the harness drives the shell the way a person does: wait for the prompt,
send one command, read until the prompt returns, check, repeat. `SerialSession`
in `run-tests.py` is about 50 lines of `select()`-based expect.

### The syscall-fuzz scenario

A scenario of its own rather than a line in the shell script, because it runs
for ~25 seconds where every other command finishes in under one, and because
its pass condition is different in kind. Every other scenario asserts that the
kernel *did* something; this one asserts that 40,000 deliberately malformed
system calls, issued from ring 3, failed to make it do anything at all.

```python
("calls were refused", r"\[fuzz\] survived: \d+ accepted, [1-9]\d* refused"),
("calls were accepted", r"\[fuzz\] survived: [1-9]\d* accepted,"),
("the console rate limiter engaged",
 r"\(\d+ more like the next line in the last \d+ ms\)"),
("the shell survived and took another command",
 r"heap integrity: consistent"),
```

Four of the eleven expectations are worth pointing at. Both counts must be
non-zero: all-refused would mean the generator never produced a valid call,
all-accepted would mean the kernel never refused one, and either way the run
proved nothing. The rate limiter must visibly engage, because a limiter that
never fires is a limiter nobody has tested. And the scenario runs `stress 2 20`
*after* the fuzzer, because a kernel that survived the fuzzer and came out of
it with a corrupted heap has not survived the fuzzer.

The nine forbidden patterns do the rest of the work. See
[FUZZING.md](FUZZING.md).

### The long-mode scenario

The same 32-bit image, booted on a processor that has x86-64, so the
`longmode` suite takes its real path instead of its declining one. That is
the whole purpose of the scenario: the suite has two sets of assertions, and
without this one only the smaller set would ever run.

| Where | What `longmode` asserts | Checks |
| --- | --- | --- |
| `qemu-system-i386` | the kernel declines correctly - nothing attempted, nothing built, a reason given, the machine left alone | 17 |
| `qemu-system-x86_64` | the transition itself: six stage flags, every 64-bit probe, and that nothing was disturbed | 51 |

```python
("the round trip completed",
 r"lm: 64-bit long mode entered and left: CS 0x18, "
 r"EFER\.LMA set, \d+ MiB identity-mapped by a 4-level "
 r"table, round trip \d+ cycles"),
("the suite took its real path, not its declining one",
 r"ktest: longmode \.\.\. PASS \(5\d checks\)"),
```

The last pattern is the one that matters. Asserting the transition worked is
not enough on its own: a scenario where the suite quietly took its
17-check path would pass every other expectation while testing none of the
feature. Checking the *count* is what distinguishes "it ran" from "it was
skipped". See [LONGMODE.md](LONGMODE.md).

### The network scenario, and the pcap

The only scenario that does not assert on the kernel's own output. QEMU is
given `filter-dump`, which writes every frame in both directions to a
libpcap file, and the harness parses it.

That distinction is not stylistic. The driver's first working version
transmitted every frame with a source MAC of `00:00:00:00:00:00`, because
`struct net_device` was `const` and its `.mac` was never filled: the log
printed the driver's own copy, correctly, while the stack built frames from a
second copy that was empty. The peer replied to the zero address and the
controller's receive filter dropped the reply. Transmit worked, receive was
silent, and **every log line was right**. No assertion inside the kernel
could have found it.

So the harness has its own checksum:

```python
def inet_checksum(b: bytes) -> int:
    """RFC 1071, written independently of the kernel's version so that the
    two agreeing means something."""
```

and asserts, over every frame the kernel sent: a well-formed ARP request from
`10.0.2.15` to the broadcast address, a reply from the gateway, an echo
request out, an echo reply in, a correct IPv4 header checksum on every
datagram, a correct ICMP checksum, and — specifically, so it can never return
silently — that no frame had a zero source hardware address.

The `no-network` scenario is its counterpart. QEMU attaches a default e1000
unless told otherwise, which means every other scenario has a network whether
it asked for one or not, so the path where there is no controller would never
run. `-nic none` is what exercises it.

### The benchmark scenario

A fourth image boots with `autobench`, which runs the microbenchmarks and a
profile and then shuts down. Seventeen expectations cover it, including one
that asserts the profiler is *useful* rather than merely running:

```python
# The workload is allocator-dominated, so a profiler that discriminates must
# put kmalloc at the top. This asserts the profiler is useful, not merely that
# it runs.
("profile found the hot path", r"\d+\s+\d+\.\d%\s+kmalloc"),
```

See [PERFORMANCE.md](PERFORMANCE.md) for the methodology those numbers rest on.

### Running one scenario

```bash
python3 tools/run-tests.py --only interactive-shell
python3 tools/run-tests.py --only custom-bootloader --show-log
python3 tools/run-tests.py --keep-logs logs/
```

A failing scenario prints its whole serial transcript automatically, because
the one thing you always want after a CI failure is the log.

---

## Fault injection

The exception handlers need exercising on purpose, not just by accident. The
`fault` command does it, and each of these halts the kernel:

```
stratum> fault null        write to the unmapped null page   → #PF
stratum> fault unmapped    read a wild address               → #PF
stratum> fault readonly    ring-0 write to a read-only page  → #PF, CR0.WP
stratum> fault div0        divide by zero                    → #DE
stratum> fault ud          an invalid opcode                 → #UD
stratum> fault panic       call panic() directly
```

```
stratum> fault text        ring-0 write to the kernel's .text  → #PF, W^X
stratum> fault stackguard  write below a task's stack          → #PF, guard page
```

The first six are not part of CI — they are how the quality of the
diagnostics gets judged. The last two *are*, because a mitigation has two
halves and only one of them can be checked by a test that passes.

One detail worth recording. The fault addresses and the division operands all
live in file-scope `volatile` variables:

```c
/* Both operands live in volatile storage. With a literal numerator GCC proves
 * the division is undefined and emits `ud2` instead of `idiv`, so the #DE this
 * command exists to show never happens. */
static volatile int fault_dividend = 1;
static volatile int fault_divisor;  /* left zero */
```

At `-O2`, GCC replaces a provably-undefined operation with `ud2`. Both
`*(u32 *)0 = 1` and `1 / zero` compiled to an invalid opcode, so the commands
produced a perfectly-reported `#UD` instead of the page fault and divide error
they were supposed to demonstrate. The kernel was right; the test was wrong.

---

### Errors that are supposed to happen

Several checks in the `storage` and `elf` suites work by calling something
that must fail. Each of those logs an `ERROR`, correctly — and the forbidden
list above treats an unexpected `ERROR` line as a failure, also correctly.

Rather than soften the messages, a test opens a window in which they are
counted instead of printed:

```c
log_expect_errors(true);
KT_ASSERT(r, !ata_read(0, d->sectors, 1, buf));          /* past the end */
KT_ASSERT(r, !ata_read(0, d->sectors - 1, 2, buf));      /* straddles it */
KT_ASSERT(r, !ata_read(0, 0xFFFFFFFFFFFFFFFFull, 2, buf)); /* wraps      */
KT_ASSERT(r, !ata_read(ATA_MAX_DRIVES, 0, 1, buf));      /* no such disk */
KT_ASSERT(r, !ata_read(0, 0, 1, NULL));                  /* no buffer    */

u32 complaints = log_expected_errors();

log_expect_errors(false);
KT_EQ(r, complaints, 5u);
```

Counting is stronger than suppressing. Five refusals must produce five
complaints, so a driver that refused *silently* fails this test — and a
refusal nobody can see is nearly as bad as no refusal at all.

A lookup that fails to find a file is deliberately not in that category: it
logs at debug level, because a shell that probes for a file would be unusable
otherwise, and the `fs` suite checks those refusals without a window at all.

### Scenarios that must panic

The `harden` suite proves the kernel's `.text` has no write bit in its page
table entry, and that the page below every task stack is unmapped. It cannot
prove the CPU acts on either, because the correct outcome of trying is a dead
kernel.

`fault-text` and `fault-stackguard` each boot a kernel, type one command, and
require the panic to name the right address, the right reason and the right
region:

```python
(
    "fault-text",
    "fault text",
    [
        r"faulting address: 0xc01[0-9a-f]{5}",
        r"access\s+: write from ring 0",
        r"reason\s+: the page is mapped read-only \(CR0\.WP applies to ring 0 too\)",
        r"region\s+: the kernel's own code or constants, which are read-only",
        r"KERNEL PANIC",
        r"at\s+cmd_fault\+0x",     # the symbol table survives a fault in .text
    ],
),
```

and on QEMU exiting **35**, the panic code. A clean exit there would mean the
write succeeded, which is exactly the regression these exist to catch. The
session keeps draining the serial port for five seconds after the panic
banner, because the register dump, the resolved symbol and the call trace all
arrive after it and those are most of what is being asserted.

---

## CI

`.github/workflows/ci.yml` runs five jobs, cheapest first:

| Job | Contents |
| --- | --- |
| `host-tests` | layer 1, ~10 s |
| `build` | cross build, pre-boot validation, size report, uploads the images |
| `fuzz` | layer 5: four libFuzzer targets over the real kernel sources, 20,000 runs each |
| `boot-tests` | layers 3 and 4; uploads every serial transcript on success or failure |
| `analysis` | `clang-format --dry-run --Werror` (gating), `cppcheck` and GCC's `-fanalyzer` (advisory) |

`fuzz` runs before `boot-tests` on purpose: when both would fail, a sanitizer
report naming the instruction is a far better diagnosis than a triple fault.

The static analysers are advisory on purpose: a kernel legitimately does things
that look alarming out of context — volatile pointers to fixed addresses,
inline assembly with register constraints, structures laid out to match
hardware — and a gate that cries wolf gets ignored.

Serial transcripts are uploaded with `if: always()`, because the run you most
want the log from is the one that failed.
