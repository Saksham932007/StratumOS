#!/usr/bin/env python3
"""StratumOS - boot the riscv64 kernel under QEMU and assert on what it says.

The counterpart of the `--only` scenarios in run-tests.py, kept separate for
the same reason the build is: run-tests.py knows about disk images, two boot
protocols and a serial console driven command by command, and none of that
applies to a kernel started directly from an ELF with no firmware underneath.

Unattended the same way the x86 side is. The SiFive test device at 0x100000
terminates the machine with an exit code when written to, which is the role
`-device isa-debug-exit` plays on x86 - so a pass or a failure is a process
exit status rather than something to screen-scrape.
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

QEMU = "qemu-system-riscv64"

# The SiFive test device's convention is *not* x86's.
#
# `isa-debug-exit` on x86 exits with ((code << 1) | 1), which is why the x86
# harness expects 3 for a pass. The RISC-V device instead treats
# FINISHER_PASS (0x5555) as a clean shutdown and exits 0, and FINISHER_FAIL
# (0x3333) as a failure whose exit status is the code in the high half-word.
#
# Assuming the x86 formula carried over made every expectation pass and the
# scenario fail, which is a small example of what porting a *harness* costs
# on top of porting a kernel.
EXIT_PASS = 0

EXPECTED = [
    # The boot arc, in order. Each line is a privilege transition or a piece
    # of hardware the port had to bring up itself.
    ("the banner printed through a 16550 at 0x10000000",
     r"StratumOS \d+\.\d+\.\d+\s+-\s+riscv64 \(rv64imac\) on QEMU virt"),
    ("the loader's hand-off was read",
     r"boot: hart 0, device tree at 0x[0-9a-f]+"),
    ("machine mode set up its own trap vector and timer",
     r"machine mode: mtvec installed, CLINT timer at 100 Hz, "
     r"\d+ exception\(s\) delegated to supervisor mode"),
    ("misa was read, so the ISA string is the processor's own",
     r"misa 0x[0-9a-f]+"),
    # The transition that is the whole reason the port boots in M-mode.
    ("mret reached supervisor mode", r"supervisor mode reached by mret"),
    ("the supervisor trap vector was installed", r"stvec installed"),
    ("Sv39 came up with a three-level table",
     r"Sv39 enabled: root at 0x[0-9a-f]+, 2 gigapages identity-mapped"),

    # The point of the port: the shared code, running unmodified.
    ("the shared sources were identified as such",
     r"running the \*shared\* code, compiled from the same sources the x86 "
     r"kernel links"),
    ("kernel/core/printf.c passed on riscv64",
     r"rvtest: core/printf\.c \.\.\. PASS"),
    ("kernel/core/string.c passed on riscv64",
     r"rvtest: core/string\.c \.\.\. PASS"),

    # The arch layer.
    ("traps dispatch, with the instruction length decoded",
     r"rvtest: traps\s+\.\.\. PASS"),
    ("the CLINT timer fires in M-mode while S-mode runs",
     r"rvtest: timer\s+\.\.\. PASS"),
    ("the Sv39 three-level walk resolves a 4 KiB page",
     r"rvtest: sv39\s+\.\.\. PASS"),

    # Counts, so that a run which skipped the body cannot pass. The illegal
    # instruction must be counted exactly once: twice means the handler
    # resumed mid-instruction, which is the bug the length decoder fixed.
    ("every trap kind was taken",
     r"traps taken: \d+ total, [1-9]\d* timer, 1 ecall, 1 illegal"),
    ("the timer actually ticked", r"timer: [1-9]\d* tick\(s\), mtime \d+"),
    ("enough checks ran that the body was not skipped",
     r"rvtest: summary ([5-9]\d|\d{3,}) check\(s\), 0 failure\(s\)"),
    ("and it said so", r"rvtest: ALL CHECKS PASSED"),
]

FORBIDDEN = [
    ("a failed check", r"rvtest: FAIL"),
    ("failures reported", r"rvtest: THERE WERE FAILURES"),
    # An unexpected ERROR line. The deliberate rejections in the Sv39 test
    # run inside log_expect_errors(), so a real one here is a real problem.
    ("an error-level log line", r"\]\s+ERROR\s"),
    ("a machine-mode exception", r"machine-mode exception"),
    ("an unhandled supervisor exception", r"supervisor exception"),
    ("an unexpected machine interrupt", r"unexpected machine interrupt"),
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", type=Path, required=True)
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--show-log", action="store_true")
    args = ap.parse_args()

    if not shutil.which(QEMU):
        print(f"run-riscv64: {QEMU} is not installed; skipping",
              file=sys.stderr)
        return 0

    if not args.elf.is_file():
        print(f"run-riscv64: {args.elf} does not exist - run "
              f"'make riscv64' first", file=sys.stderr)
        return 2

    cmd = [
        QEMU,
        "-machine", "virt",
        # No firmware: the hart starts in machine mode at the base of RAM and
        # the kernel owns the machine from reset.
        "-bios", "none",
        "-nographic",
        "-smp", "1",
        "-m", "128M",
        "-kernel", str(args.elf),
    ]

    print(f"run-riscv64: booting {args.elf.name} under {QEMU}\n")

    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=args.timeout, errors="replace")
        log = proc.stdout + proc.stderr
        status: int | None = proc.returncode
    except subprocess.TimeoutExpired as e:
        log = (e.stdout or b"").decode(errors="replace") if \
            isinstance(e.stdout, bytes) else (e.stdout or "")
        status = None

    failures: list[str] = []

    if status is None:
        failures.append(f"QEMU did not exit within {args.timeout}s - the "
                        f"kernel never reached its shutdown path")

    for label, pattern in EXPECTED:
        if not re.search(pattern, log):
            failures.append(f"missing: {label}  (no match for /{pattern}/)")

    for label, pattern in FORBIDDEN:
        m = re.search(pattern, log)
        if m:
            failures.append(f"forbidden: {label} -> {m.group(0)}")

    if status is not None and status != EXIT_PASS:
        failures.append(f"unexpected exit status {status} "
                        f"(expected {EXIT_PASS}, a clean shutdown via the "
                        f"SiFive test device)")

    summary = re.search(r"rvtest: summary (\d+) check\(s\), (\d+) failure",
                        log)
    if summary:
        print(f"    checks run       : {summary.group(1)}")
        print(f"    failures         : {summary.group(2)}")
    print(f"    qemu exit        : {status}")
    print(f"    expectations     : {len(EXPECTED)}")

    if not failures:
        print("    result           : PASS\n")
        if args.show_log:
            for line in log.splitlines():
                print(f"    | {line}")
        return 0

    print("    result           : FAIL")
    for f in failures:
        print(f"      - {f}")
    print("\n    --- serial log ---")
    for line in log.splitlines():
        print(f"    | {line}")
    print("    --- end ---\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
