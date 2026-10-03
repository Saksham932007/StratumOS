#!/usr/bin/env python3
"""StratumOS - how much of the kernel compiles for the second architecture.

The port's value is not that RISC-V works; QEMU's RISC-V works. It is the
measurement of how much of this kernel was genuinely architecture-independent
and how much only looked it, and that number belongs in a tool rather than in
a sentence in a document, because a quoted number goes stale the first time
somebody adds a file.

Method: compile every kernel source for riscv64 with -Werror and count what
comes out clean. No linking, no running - this measures *source* portability,
which is the question. A file that compiles cleanly is not thereby correct on
the target (kernel/mm/vmm.c would compile for riscv64 the moment its casts
were widened, and would still be two-level x86 paging), so the output
separates "compiles" from "is used by the riscv64 kernel".
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

CC = "clang"
FLAGS = [
    "--target=riscv64-unknown-elf",
    "-march=rv64imac", "-mabi=lp64", "-mcmodel=medany",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    "-Wall", "-Wextra", "-Werror", "-std=gnu11",
    "-I", "kernel/include",
    "-fsyntax-only",
]

# The files the riscv64 kernel actually links. Kept in step with RV_SHARED in
# the Makefile; a mismatch here would overstate the result.
LINKED = {
    "kernel/core/printf.c",
    "kernel/core/string.c",
    "kernel/core/div64.c",
    "kernel/core/log.c",
}

# Files that are x86 by definition rather than by accident. Counting them as
# portability failures would be dishonest in the other direction: they are
# the architecture layer, and a port supplies a sibling rather than fixing
# them.
ARCH_BY_DEFINITION = re.compile(r"kernel/arch/|kernel/drivers/(ata|keyboard|"
                                r"pci|rtc|serial|timer|vga|e1000)\.c")


def classify(path: Path) -> tuple[str, int, list[str]]:
    r = subprocess.run([CC, *FLAGS, str(path)], capture_output=True,
                       text=True)
    if r.returncode == 0:
        return ("clean", 0, [])

    errors = re.findall(r"error: ([^[\n]+)", r.stderr)
    return ("errors", len(errors), sorted(set(e.strip() for e in errors)))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true",
                    help="list each file's distinct error messages")
    args = ap.parse_args()

    if not shutil.which(CC):
        print(f"portability: {CC} is not installed; skipping",
              file=sys.stderr)
        return 0

    roots = ["kernel/core", "kernel/mm", "kernel/fs", "kernel/net",
             "kernel/drivers", "kernel/shell"]
    files = sorted(p for root in roots for p in Path(root).glob("*.c"))

    clean: list[Path] = []
    broken: list[tuple[Path, int, list[str]]] = []
    arch: list[Path] = []

    for f in files:
        if ARCH_BY_DEFINITION.search(str(f)):
            arch.append(f)
            continue

        state, count, msgs = classify(f)
        if state == "clean":
            clean.append(f)
        else:
            broken.append((f, count, msgs))

    portable = len(clean)
    considered = portable + len(broken)
    total_errors = sum(c for _, c, _ in broken)

    print("portability: compiling every portable-by-intent kernel source "
          "for riscv64\n")
    print(f"  {'file':<32} {'riscv64':<10} linked")
    print(f"  {'-' * 32} {'-' * 10} {'-' * 6}")

    for f in clean:
        mark = "yes" if str(f) in LINKED else "-"
        print(f"  {str(f).replace('kernel/', ''):<32} {'clean':<10} {mark}")
    for f, count, _ in broken:
        print(f"  {str(f).replace('kernel/', ''):<32} "
              f"{str(count) + ' err':<10} -")

    print(f"\n  {portable}/{considered} files compile for riscv64 unmodified "
          f"({100 * portable // considered}%)")
    print(f"  {total_errors} error(s) remain, across {len(broken)} file(s)")
    print(f"  {len(arch)} file(s) excluded as the x86 architecture layer")
    print(f"  {len(LINKED)} file(s) are actually linked into the riscv64 "
          f"kernel and run there")

    if args.verbose and broken:
        print("\n  what is left, by file:")
        for f, count, msgs in broken:
            print(f"\n  {f} ({count} error(s))")
            for m in msgs[:6]:
                print(f"    - {m}")

    # Informational: a report, not a gate. A file failing to compile for a
    # second architecture is a finding to act on, not a build break.
    return 0


if __name__ == "__main__":
    sys.exit(main())
