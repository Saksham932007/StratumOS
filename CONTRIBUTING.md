# Contributing

## Building

```bash
sudo apt install build-essential nasm gcc-multilib libc6-dev-i386 \
                 qemu-system-x86 grub-pc-bin grub-common xorriso mtools
make
make test
```

`make toolchain` prints exactly which tools were found and which are missing.
An `i686-elf-gcc` cross-compiler is used automatically if one is on `PATH`;
otherwise the host GCC is driven with `-m32`.

## Before opening a pull request

```bash
make format        # clang-format, in place
make test          # host unit tests + three QEMU boot scenarios
```

Both must pass. `make format-check` is a gating CI job, so an unformatted
patch will fail before anything else runs.

## Style

- 4-space indent, 80 columns, Linux brace style. `.clang-format` is
  authoritative; run `make format` rather than arguing with it.
- C11 (`-std=gnu11`), compiled `-Wall -Wextra -Werror` plus `-Wshadow`,
  `-Wcast-align`, `-Wstrict-prototypes` and `-Wmissing-prototypes`. Warnings
  are errors, including in tests.
- Headers are self-contained and include what they use.
- A file that logs defines `LOG_TAG` **before** its includes:
  ```c
  #define LOG_TAG "pmm"

  #include <kernel/log.h>
  ```
- No SSE, MMX or floating point. The kernel never enables them in `CR4`, and
  `check-kernel.py` fails the build if any appear.
- Fixed-width types (`u32`, `paddr_t`, `vaddr_t`) over `int` and `long` for
  anything that touches hardware or an address.

## Comments

Comments should say **why**, not what. The code already says what.

```c
/* bad */
/* set the present bit */
pte |= PTE_PRESENT;

/* good */
/* A directory entry's USER bit gates the whole 4 MiB range, so it has to be
 * widened if any page inside it becomes user-accessible. */
if (flags & PTE_USER)
    pd[pdi] |= PTE_USER;
```

Every non-obvious hardware interaction should name the constraint it is
satisfying — which register, which specification, which failure mode if it is
done differently. If a line exists because of a bug, say so; that comment is
the only thing standing between the next reader and reintroducing it.

## Tests are not optional

Any change to allocator, paging, heap, scheduler or syscall behaviour needs a
matching assertion.

- Pure logic (formatting, string handling, arithmetic) →
  `tests/host/test_printf.c`. Runs in a second, no emulator.
- Anything needing real hardware state → a suite in `kernel/core/ktest.c`.
  Add an entry to the `tests[]` table; the count, listing and summary all
  derive from it.
- New shell commands or boot behaviour → `SHELL_SCRIPT` or `COMMON_EXPECTED`
  in `tools/run-tests.py`.

See [docs/TESTING.md](docs/TESTING.md) for each layer and how to extend it.

## Commit messages

Imperative subject under ~72 characters, then a body explaining *why* if the
change is not self-evident. If a commit fixes a real defect, describe the
failure mode — that is the part worth reading in six months.

## Where things live

| Putting | Goes in |
| --- | --- |
| something x86-specific | `kernel/arch/x86/` |
| a hardware driver | `kernel/drivers/` |
| an allocator or anything paging-related | `kernel/mm/` |
| a kernel service, portable in spirit | `kernel/core/` |
| a shell command | `kernel/shell/shell.c`, plus its `commands[]` entry |
| a build-time check | `tools/check-kernel.py` |
| a boot-time assertion | `tools/run-tests.py` |

New public interfaces get a header in `kernel/include/<area>/`, with a comment
block at the top explaining what the subsystem is for and which design choices
are deliberate.
