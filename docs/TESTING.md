# Testing

Four layers, ordered cheapest-first so a trivial mistake fails in a second
rather than after an emulator run.

| Layer | Needs | Runtime | Catches |
| --- | --- | --- | --- |
| Host unit tests | nothing | ~1 s | formatter, string, 64-bit division bugs |
| Pre-boot validation | the linker | ~0.1 s | unbootable images (20 failure conditions) |
| In-kernel suites | QEMU | ~5 s | allocator, paging, heap, scheduler, syscall, symbol and profiler bugs |
| Boot scenarios | QEMU + GRUB | ~2 min | regressions in either boot path, shell behaviour, measurement |

```bash
make test        # all of it
make test-host   # layer 1 only, no emulator
make test-boot   # layers 3 and 4
```

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

`kernel/core/ktest.c` holds 14 suites and 269 assertions, run against
real hardware state — a bitmap with actual firmware-reported memory in it, real
page tables, a real heap, a real scheduler.

```
stratum> selftest
ktest: running 14 suites
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
ktest: ksyms ... PASS (12 checks)
ktest: profile ... PASS (9 checks)
ktest: summary 14/14 suites passed
```

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

`tools/run-tests.py` boots the kernel three ways and asserts on what it says.

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

Each scenario then checks 25 expected lines, 9 forbidden patterns, the
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

### The interactive scenario

21 shell commands, sent over the serial console and checked against regular
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

These are not part of CI — they deliberately crash the machine — but they are
how the quality of the diagnostics gets judged.

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

## CI

`.github/workflows/ci.yml` runs four jobs, cheapest first:

| Job | Contents |
| --- | --- |
| `host-tests` | layer 1, ~10 s |
| `build` | cross build, pre-boot validation, size report, uploads the images |
| `boot-tests` | layers 3 and 4; uploads every serial transcript on success or failure |
| `analysis` | `clang-format --dry-run --Werror` (gating), `cppcheck` and GCC's `-fanalyzer` (advisory) |

The static analysers are advisory on purpose: a kernel legitimately does things
that look alarming out of context — volatile pointers to fixed addresses,
inline assembly with register constraints, structures laid out to match
hardware — and a gate that cries wolf gets ignored.

Serial transcripts are uploaded with `if: always()`, because the run you most
want the log from is the one that failed.
