#!/usr/bin/env python3
"""Boot StratumOS under QEMU and assert on what it says.

Why this exists
---------------
"It booted" is not a test. This harness boots the kernel through *both*
supported paths - the custom two-stage bootloader from a raw disk image, and
Multiboot2 via GRUB from an ISO - and then checks that:

  * each boot path reports itself correctly, so a regression that silently
    falls back to the other protocol is caught;
  * every subsystem announced that it came up;
  * every in-kernel test suite passed;
  * nothing panicked, faulted, or logged an error;
  * QEMU exited with the status the kernel asked for.

The kernel cooperates by emitting fixed, greppable lines rather than prose,
and by writing to `isa-debug-exit` so a run ends by itself instead of hanging
until a timeout.

Exit status mapping
-------------------
The kernel writes a code to port 0xF4; QEMU exits with (code << 1) | 1.

    kernel code 0x01  ->  QEMU 3   tests passed
    kernel code 0x02  ->  QEMU 5   tests failed
    kernel code 0x11  ->  QEMU 35  panic
    kernel code 0x12  ->  QEMU 37  panic inside the panic handler
"""

from __future__ import annotations

import argparse
import os
import re
import select
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

QEMU = "qemu-system-i386"

EXIT_PASS = 3
EXIT_FAIL = 5
EXIT_PANIC = 35
EXIT_DOUBLE_PANIC = 37

# Lines that must appear in any successful boot, whichever loader was used.
COMMON_EXPECTED = [
    ("serial driver", r"boot: serial COM1\s+\[ok\]"),
    ("VGA driver", r"boot: VGA text mode\s+\[ok\]"),
    ("CPU detection", r"boot: CPU detect\s+\[ok\]"),
    ("GDT and TSS", r"boot: GDT \+ TSS\s+\[ok\]"),
    ("IDT", r"boot: IDT\s+\[ok\]"),
    ("PIC remap", r"boot: PIC remap\s+\[ok\]"),
    ("timer", r"boot: PIT timer\s+\[ok\]"),
    ("keyboard", r"boot: PS/2 keyboard\s+\[ok\]"),
    ("interrupts enabled", r"boot: interrupts\s+\[ok\]"),
    ("physical allocator", r"boot: physical memory\s+\[ok\]"),
    ("paging", r"boot: paging\s+\[ok\]"),
    ("kernel heap", r"boot: kernel heap\s+\[ok\]"),
    ("RTC", r"boot: RTC\s+\[ok\]"),
    ("PCI", r"boot: PCI\s+\[ok\]"),
    ("scheduler", r"boot: scheduler\s+\[ok\]"),
    ("syscalls", r"boot: syscalls\s+\[ok\]"),
    ("paging really enabled", r"vmm: paging enabled"),
    ("heap really created", r"heap: kernel heap at 0xd0000000"),
    ("ring 3 reached", r"\[ring3\] hello from user mode"),
    ("ring 3 syscalls work", r"\[ring3\] getpid\(\) returned \d+"),
    ("ring 3 can sleep", r"\[ring3\] slept 50 ms via syscall"),
    ("kernel pointers rejected", r"\[ring3\] kernel refused it \(EFAULT\)"),
    ("bad syscall rejected", r"\[ring3\] unknown syscall correctly rejected"),
    ("ring 3 exited cleanly", r"\[ring3\] calling exit\(0\)"),
    ("autotest finished", r"stratum: autotest complete"),
]

# Lines that must NEVER appear.
FORBIDDEN = [
    ("kernel panic", r"KERNEL PANIC"),
    ("double panic", r"double panic"),
    ("unhandled exception", r"unhandled CPU exception"),
    ("page fault", r"page fault at"),
    ("test failure", r"ktest: \S+ \.\.\. FAIL"),
    ("test suite failures", r"THERE WERE FAILURES"),
    ("error-level log line", r"\]\s+ERROR\s"),
    ("ring 3 escaped the pointer check",
     r"WARNING: kernel accepted a kernel pointer"),
    ("heap corruption", r"PROBLEMS FOUND"),
    ("spurious interrupt storm", r"unhandled IRQ"),
]


@dataclass
class Scenario:
    name: str
    description: str
    image: Path
    qemu_args: list[str]
    extra_expected: list[tuple[str, str]] = field(default_factory=list)
    timeout: int = 90


@dataclass
class Outcome:
    scenario: Scenario
    passed: bool
    qemu_exit: int | None
    log: str
    failures: list[str]
    suites: tuple[int, int] | None


def run_scenario(sc: Scenario, keep_logs: Path | None) -> Outcome:
    failures: list[str] = []

    with tempfile.TemporaryDirectory() as tmp:
        log_path = Path(tmp) / "serial.log"

        cmd = [
            QEMU,
            "-m", "128M",
            "-no-reboot",
            "-display", "none",
            "-serial", f"file:{log_path}",
            "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
            *sc.qemu_args,
        ]

        try:
            proc = subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=sc.timeout)
            qemu_exit: int | None = proc.returncode
            stderr = proc.stderr
        except subprocess.TimeoutExpired:
            qemu_exit = None
            stderr = ""
            failures.append(
                f"QEMU did not exit within {sc.timeout}s. The kernel never "
                f"reached its shutdown path - look for a hang in the log."
            )

        log = log_path.read_text(errors="replace") if log_path.exists() else ""

        if keep_logs:
            keep_logs.mkdir(parents=True, exist_ok=True)
            (keep_logs / f"{sc.name}.log").write_text(log)

    if stderr.strip():
        interesting = [l for l in stderr.splitlines()
                       if l.strip() and "warning" not in l.lower()]
        if interesting:
            failures.append("QEMU reported: " + "; ".join(interesting[:3]))

    if not log.strip():
        failures.append("the serial log is empty - the kernel produced no "
                        "output at all")

    for label, pattern in COMMON_EXPECTED + sc.extra_expected:
        if not re.search(pattern, log):
            failures.append(f"missing: {label}  (no match for /{pattern}/)")

    for label, pattern in FORBIDDEN:
        match = re.search(pattern, log)
        if match:
            line = next((l for l in log.splitlines() if match.group(0) in l),
                        match.group(0))
            failures.append(f"forbidden: {label}  ->  {line.strip()[:110]}")

    suites: tuple[int, int] | None = None
    summary = re.search(r"ktest: summary (\d+)/(\d+) suites passed", log)
    if summary:
        passed, total = int(summary.group(1)), int(summary.group(2))
        suites = (passed, total)
        if passed != total:
            failures.append(f"only {passed} of {total} test suites passed")
    else:
        failures.append("no 'ktest: summary' line - the test suite never ran "
                        "to completion")

    if qemu_exit is not None and qemu_exit not in (EXIT_PASS,):
        if qemu_exit == EXIT_FAIL:
            failures.append("the kernel reported test failures via its exit "
                            "code")
        elif qemu_exit in (EXIT_PANIC, EXIT_DOUBLE_PANIC):
            failures.append(f"the kernel panicked (QEMU exit {qemu_exit})")
        else:
            failures.append(f"unexpected QEMU exit status {qemu_exit} "
                            f"(expected {EXIT_PASS})")

    return Outcome(sc, not failures, qemu_exit, log, failures, suites)


# ---------------------------------------------------------------------------
# Interactive shell scenario
# ---------------------------------------------------------------------------
#
# Feeding a script straight into QEMU's stdin does not work: the UART's FIFO
# is 16 bytes and the kernel does not start reading it until the shell is up,
# so everything typed before then is simply lost to a receive overrun. The
# commands therefore have to be sent the way a person would - one at a time,
# after the prompt has appeared.

PROMPT = "stratum> "


class SerialSession:
    """Drive the kernel's shell over QEMU's stdio serial port."""

    def __init__(self, cmd: list[str], timeout: float) -> None:
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, bufsize=0)
        self.timeout = timeout
        self.transcript = ""

    def read_until(self, needle: str, timeout: float | None = None) -> bool:
        """Accumulate output until `needle` appears. False on timeout."""
        deadline = time.monotonic() + (timeout or self.timeout)
        assert self.proc.stdout is not None
        fd = self.proc.stdout.fileno()

        while time.monotonic() < deadline:
            if needle in self.transcript:
                return True

            ready, _, _ = select.select([fd], [], [], 0.25)
            if not ready:
                if self.proc.poll() is not None:
                    return needle in self.transcript
                continue

            chunk = os.read(fd, 4096)
            if not chunk:
                return needle in self.transcript
            self.transcript += chunk.decode("utf-8", errors="replace")

        return needle in self.transcript

    def send_line(self, text: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write((text + "\r").encode())
        self.proc.stdin.flush()

    def run_command(self, command: str, settle: float = 6.0) -> str:
        """Send a command and return just the output it produced."""
        mark = len(self.transcript)
        self.send_line(command)
        # The prompt reappears when the command is done. Looking for it from
        # the current position avoids matching the prompt we already consumed.
        deadline = time.monotonic() + settle
        while time.monotonic() < deadline:
            if self.transcript.count(PROMPT, mark) >= 1 and \
               len(self.transcript) > mark + len(command):
                tail = self.transcript[mark:]
                if PROMPT in tail[len(command):]:
                    break
            if not self.read_until("\x00", timeout=0.3):
                pass
        return self.transcript[mark:]

    def close(self, grace: float = 10.0) -> int | None:
        try:
            if self.proc.stdin:
                self.proc.stdin.close()
        except OSError:
            pass
        try:
            return self.proc.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            return None


# Each entry is (command, [patterns the output must contain]).
SHELL_SCRIPT: list[tuple[str, list[str]]] = [
    ("syms", [r"\d+ symbols embedded"]),
    ("bench vmm-xlate", [r"bench: vmm-xlate\s+min=", r"tsc \d+\.\d+ MHz"]),
    ("version", [r"StratumOS 0\.3", r"boot via",
                 r"StratumOS native|Multiboot2"]),
    ("uptime", [r"up \d+:\d\d:\d\d", r"timer ticks"]),
    ("cpuinfo", [r"Vendor\s+:", r"Mode\s+: 32-bit protected mode"]),
    ("meminfo", [r"Physical memory", r"Kernel heap",
                 r"integrity : consistent", r"Firmware memory map"]),
    ("ps", [r"PID\s+NAME\s+STATE", r"\bidle\b", r"\bshell\b",
            r"context switches total"]),
    ("irq", [r"IRQ\s+HANDLER\s+COUNT", r"\bpit\b",
             r"spurious: 0"]),
    ("pci", [r"ADDRESS\s+ID\s+VENDOR", r"host bridge"]),
    ("date", [r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d UTC", r"unix \d+"]),
    ("pagemap 0x100000", [r"physical  : 0x00100000", r"flags     :.*present"]),
    ("pagemap 0xd0000000", [r"physical  : 0x", r"present"]),
    ("hexdump 0x100000 16", [r"00100000 ", r"\|"]),
    ("echo interactive shell works", [r"interactive shell works"]),
    ("help", [r"selftest", r"meminfo", r"pagemap"]),
    ("help pagemap", [r"usage: pagemap <address>"]),
    ("selftest list", [r"SUITE\s+DESCRIPTION", r"heap", r"sched"]),
    ("selftest heap", [r"ktest: heap \.\.\. PASS"]),
    ("selftest vmm", [r"ktest: vmm \.\.\. PASS"]),
    ("stress 2 40", [r"heap integrity: consistent"]),
    ("ring3", [r"\[ring3\] hello from user mode",
               r"\[ring3\] calling exit\(0\)"]),
    ("nosuchcommand", [r"command not found"]),
    ("log debug", [r"log level set to DEBUG"]),
]


def run_interactive(build_dir: Path, keep_logs: Path | None) -> Outcome:
    image = build_dir / "stratum-shell.img"
    sc = Scenario(
        name="interactive-shell",
        description="shell driven over the serial console, command by command",
        image=image,
        qemu_args=[],
    )

    failures: list[str] = []

    cmd = [
        QEMU, "-m", "128M", "-no-reboot", "-display", "none",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-serial", "stdio",
        "-drive", f"format=raw,file={image},index=0,media=disk",
    ]

    session = SerialSession(cmd, timeout=40.0)

    if not session.read_until(PROMPT, timeout=40.0):
        failures.append("the shell prompt never appeared")
        session.close()
        return Outcome(sc, False, None, session.transcript, failures, None)

    for command, patterns in SHELL_SCRIPT:
        output = session.run_command(command)
        for pattern in patterns:
            if not re.search(pattern, output):
                failures.append(
                    f"'{command}': no match for /{pattern}/ in its output")

    session.send_line("halt")
    session.read_until("halting", timeout=10.0)
    exit_code = session.close()

    log = session.transcript
    if keep_logs:
        keep_logs.mkdir(parents=True, exist_ok=True)
        (keep_logs / "interactive-shell.log").write_text(log)

    for label, pattern in FORBIDDEN:
        # A deliberate 'nosuchcommand' is expected; test-failure patterns are
        # not. Panics and faults are never acceptable.
        match = re.search(pattern, log)
        if match:
            failures.append(f"forbidden: {label} -> {match.group(0)}")

    if exit_code is None:
        failures.append("QEMU did not exit after 'halt'")
    elif exit_code != EXIT_PASS:
        failures.append(f"unexpected QEMU exit status {exit_code} after halt")

    return Outcome(sc, not failures, exit_code, log, failures, None)


# The benchmark image reports measurements and a profile rather than test
# results, so it gets its own expectation set instead of COMMON_EXPECTED.
BENCH_EXPECTED = [
    ("TSC calibrated", r"bench: tsc \d+\.\d+ MHz"),
    ("platform disclosed", r"bench: (NOTE running under|bare metal)"),
    ("harness overhead measured", r"bench: harness overhead \d+ cycles"),
    ("syscall measured", r"bench: syscall\s+min=\d+"),
    ("context switch measured", r"bench: ctxsw\s+min=\d+"),
    ("kmalloc measured", r"bench: kmalloc\s+min=\d+"),
    ("pmm measured", r"bench: pmm\s+min=\d+"),
    ("page mapping measured", r"bench: vmm-map\s+min=\d+"),
    ("translation measured", r"bench: vmm-xlate\s+min=\d+"),
    ("memcpy measured", r"bench: memcpy-4k\s+min=\d+"),
    ("formatter measured", r"bench: ksnprintf\s+min=\d+"),
    ("benchmarks completed", r"bench: complete"),
    ("nanoseconds derived", r"= \d+\.\d+ ns"),
    ("profile produced", r"Sampling profile"),
    ("profile attributed samples", r"kernel samples: [1-9]\d* attributed"),
    # The workload is allocator-dominated, so a profiler that discriminates
    # must put kmalloc at the top. This asserts the profiler is useful, not
    # merely that it runs.
    ("profile found the hot path", r"\d+\s+\d+\.\d%\s+kmalloc"),
    ("autobench finished", r"stratum: autobench complete"),
]


def run_benchmarks(build_dir: Path, keep_logs: Path | None) -> Outcome:
    image = build_dir / "stratum-bench.img"
    sc = Scenario(
        name="benchmarks",
        description="microbenchmarks and a sampling profile",
        image=image,
        qemu_args=[
            "-drive",
            f"format=raw,file={image},index=0,media=disk",
        ],
        timeout=180,
    )

    failures: list[str] = []

    with tempfile.TemporaryDirectory() as tmp:
        log_path = Path(tmp) / "serial.log"
        cmd = [
            QEMU, "-m", "128M", "-no-reboot", "-display", "none",
            "-serial", f"file:{log_path}",
            "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
            *sc.qemu_args,
        ]

        try:
            proc = subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=sc.timeout)
            qemu_exit: int | None = proc.returncode
        except subprocess.TimeoutExpired:
            qemu_exit = None
            failures.append(f"QEMU did not exit within {sc.timeout}s")

        log = log_path.read_text(errors="replace") if log_path.exists() else ""

    if keep_logs:
        keep_logs.mkdir(parents=True, exist_ok=True)
        (keep_logs / "benchmarks.log").write_text(log)

    for label, pattern in BENCH_EXPECTED:
        if not re.search(pattern, log):
            failures.append(f"missing: {label}  (no match for /{pattern}/)")

    for label, pattern in FORBIDDEN:
        if re.search(pattern, log):
            failures.append(f"forbidden: {label}")

    if qemu_exit is not None and qemu_exit != EXIT_PASS:
        failures.append(f"unexpected QEMU exit status {qemu_exit}")

    return Outcome(sc, not failures, qemu_exit, log, failures, None)


def build_scenarios(build_dir: Path, only: str | None) -> list[Scenario]:
    scenarios = [
        Scenario(
            name="custom-bootloader",
            description="two-stage BIOS bootloader from a raw disk image",
            image=build_dir / "stratum-test.img",
            qemu_args=[
                "-drive",
                f"format=raw,file={build_dir / 'stratum-test.img'},"
                f"index=0,media=disk",
            ],
            extra_expected=[
                ("stage 1 ran", r"StratumOS stage1"),
                ("stage 2 ran", r"StratumOS stage2"),
                ("A20 gate enabled", r"\[ok\] A20 gate"),
                ("BIOS memory map read", r"\[ok\] BIOS memory map"),
                ("kernel staged from disk", r"\[ok\] kernel image staged"),
                ("entered protected mode", r"entering protected mode"),
                ("native protocol detected",
                 r"boot protocol\s+\[ok\] StratumOS native"),
            ],
        ),
        Scenario(
            name="multiboot2-grub",
            description="Multiboot2 via GRUB from an ISO",
            image=build_dir / "stratum-test.iso",
            qemu_args=["-cdrom", str(build_dir / "stratum-test.iso")],
            extra_expected=[
                ("multiboot2 protocol detected",
                 r"boot protocol\s+\[ok\] Multiboot2"),
                ("GRUB identified itself", r"booted by GRUB"),
            ],
        ),
    ]

    if only in ("interactive-shell", "benchmarks"):
        return []

    if only:
        scenarios = [s for s in scenarios if s.name == only]
        if not scenarios:
            print(f"run-tests: no scenario named '{only}'", file=sys.stderr)
            raise SystemExit(2)

    return scenarios


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", type=Path, default=Path("build"))
    ap.add_argument("--only", help="run a single scenario by name")
    ap.add_argument("--keep-logs", type=Path,
                    help="write each scenario's serial log to this directory")
    ap.add_argument("--show-log", action="store_true",
                    help="print the full serial log for every scenario")
    args = ap.parse_args()

    if not shutil.which(QEMU):
        print(f"run-tests: {QEMU} is not installed; cannot run boot tests",
              file=sys.stderr)
        return 2

    scenarios = build_scenarios(args.build_dir, args.only)

    missing = [s for s in scenarios if not s.image.is_file()]
    if missing:
        for s in missing:
            print(f"run-tests: {s.image} does not exist - run 'make' first",
                  file=sys.stderr)
        return 2

    print(f"run-tests: {len(scenarios)} boot scenario(s) under {QEMU}\n")

    outcomes = []

    if args.only in (None, "interactive-shell"):
        shell_image = args.build_dir / "stratum-shell.img"
        if shell_image.is_file():
            print("  [interactive-shell] shell driven over the serial "
                  "console, command by command")
            outcome = run_interactive(args.build_dir, args.keep_logs)
            outcomes.append(outcome)
            print(f"    commands run     : {len(SHELL_SCRIPT)}")
            print(f"    qemu exit        : {outcome.qemu_exit}")
            if outcome.passed:
                print("    result           : PASS\n")
            else:
                print("    result           : FAIL")
                for f in outcome.failures:
                    print(f"      - {f}")
                print()
                if args.show_log or True:
                    print("    --- transcript ---")
                    for line in outcome.log.splitlines():
                        print(f"    | {line}")
                    print("    --- end ---\n")
        else:
            print(f"  [interactive-shell] SKIP ({shell_image} not built)\n")

    if args.only in (None, "benchmarks"):
        bench_image = args.build_dir / "stratum-bench.img"
        if bench_image.is_file():
            print("  [benchmarks] microbenchmarks and a sampling profile")
            outcome = run_benchmarks(args.build_dir, args.keep_logs)
            outcomes.append(outcome)
            print(f"    qemu exit        : {outcome.qemu_exit}")
            if outcome.passed:
                print("    result           : PASS\n")
            else:
                print("    result           : FAIL")
                for f in outcome.failures:
                    print(f"      - {f}")
                print()
                print("    --- serial log ---")
                for line in outcome.log.splitlines():
                    print(f"    | {line}")
                print("    --- end of log ---\n")
        else:
            print(f"  [benchmarks] SKIP ({bench_image} not built)\n")

    for sc in scenarios:
        print(f"  [{sc.name}] {sc.description}")
        outcome = run_scenario(sc, args.keep_logs)
        outcomes.append(outcome)

        if outcome.suites:
            passed, total = outcome.suites
            print(f"    in-kernel suites : {passed}/{total} passed")
        print(f"    qemu exit        : {outcome.qemu_exit}")
        print(f"    serial log       : {len(outcome.log.splitlines())} lines")

        if outcome.passed:
            print("    result           : PASS\n")
        else:
            print("    result           : FAIL")
            for f in outcome.failures:
                print(f"      - {f}")
            print()

        if args.show_log or not outcome.passed:
            print("    --- serial log ---")
            for line in outcome.log.splitlines():
                print(f"    | {line}")
            print("    --- end of log ---\n")

    failed = [o for o in outcomes if not o.passed]

    print("-" * 68)
    for o in outcomes:
        status = "PASS" if o.passed else "FAIL"
        print(f"  {status}  {o.scenario.name:<20} {o.scenario.description}")
    print("-" * 68)

    if failed:
        print(f"\nrun-tests: {len(failed)}/{len(outcomes)} scenario(s) failed")
        return 1

    print(f"\nrun-tests: all {len(outcomes)} scenario(s) passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
