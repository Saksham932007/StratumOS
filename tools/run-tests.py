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
import struct
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
    ("paging really enabled", r"vmm: paging: kernel at 0xc0000000"),
    # Hardening. The kernel half being fully backed is what stops a kernel
    # mapping made after a fork from existing in only one address space.
    ("kernel half fully backed",
     r"kernel half fully backed: 255 page tables"),
    ("kernel text is read-only",
     r"kernel \.text and \.rodata mapped read-only \(\d+ pages\)"),
    ("hardening reported at boot", r"hardening\s+\[ok\]"),
    # Processors. One or four, the step has to report cleanly - and the
    # local-APIC line has to be there either way, because enabling it is what
    # moved the 8259s behind LINT0.
    ("processor count reported",
     r"boot: processors\s+\[ok\] \d+ of \d+ processor\(s\) online"),
    # Storage. The ATA driver and the block layer run on both boot paths; the
    # filesystem exists only on the raw disk image, so the shared expectation
    # is only that the step reported cleanly either way.
    ("ATA probe ran", r"boot: ATA disks\s+\[ok\]"),
    ("filesystem step reported", r"boot: filesystem\s+\[ok\]"),
    ("exec of a bogus path is refused",
     r"\[child\] exec\(\"/bin/NOTHERE\"\) failed cleanly too"),
    ("identity map dropped", r"identity map dropped"),
    ("linear map established", r"linear map \d+ MiB"),
    ("user program loaded as an ELF", r"elf: loaded a \d+-segment program"),
    ("heap really created", r"heap: kernel heap at 0xd0000000"),
    ("ring 3 reached", r"\[ring3\] hello from user mode"),
    ("ring 3 is a separate program", r"\[ring3\] I am a separate ELF"),
    ("ring 3 stack is writable", r"\[ring3\] my own stack is writable"),
    ("unmapped user pointers rejected",
     r"\[ring3\] unmapped user pointer also refused"),
    ("ring 3 syscalls work", r"\[ring3\] getpid\(\) returned \d+"),
    ("ring 3 can sleep", r"\[ring3\] slept 50 ms via syscall"),
    ("kernel pointers rejected", r"\[ring3\] kernel refused it \(EFAULT\)"),
    ("bad syscall rejected", r"\[ring3\] unknown syscall correctly rejected"),
    ("ring 3 exited cleanly", r"\[ring3\] calling exit\(0\)"),
    # Per-process address spaces. init builds its own before loading its
    # image; if it did not, fork would be cloning the kernel's user half.
    ("init has its own address space",
     r"entering ring 3 at 0x[0-9a-f]+ in its own address space"),
    # fork, copy-on-write and wait, observed from ring 3. The child's write
    # landing in the child and *not* in the parent is the only externally
    # visible difference between copy-on-write and a shared page, so it is
    # the assertion that matters here.
    ("fork returned twice", r"\[ring3\] fork\(\) returned \d+ here"),
    ("the child saw zero", r"\[child\] fork\(\) returned 0 here"),
    ("the child has the right parent", r"\[child\] .*my parent is \d+"),
    ("the child inherited the page", r"\[child\] I inherited 0x5a5a5a5a"),
    ("the child's write took", r"\[child\] my copy now reads 0x1234abcd"),
    ("wait collected the child",
     r"\[ring3\] wait\(\) collected pid \d+ with exit code 7"),
    ("copy-on-write kept the parent's page intact",
     r"my own copy still reads 0x5a5a5a5a - copy-on-write"),
    ("wait with no children returns -1",
     r"wait\(\) with no children returned -1"),
    # exec: a bogus name must fail without tearing the image down, and a
    # real one must replace the image while keeping the pid.
    ("exec of an unknown name fails cleanly",
     r"\[child\] exec\(\"nonexistent\"\) failed cleanly"),
    ("exec replaced the image", r"user: pid \d+ now running \"hello\" at 0x"),
    ("the exec'd image ran", r"\[exec\] hello: a different image"),
    ("the pid survived exec", r"\[exec\] getpid\(\) returned \d+ - the pid "
                              r"survived exec"),
    ("the rebuilt address space is still sealed",
     r"\[exec\] the rebuilt address space still refuses"),
    ("the exec'd child exited cleanly",
     r"the exec'd child \(pid \d+\) exited with 0"),
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
    # Which emulator binary to boot under. Everything defaults to
    # qemu-system-i386, because that is the machine this kernel targets. The
    # long-mode scenario needs qemu-system-x86_64: the i386 target masks
    # CPUID.80000001H:EDX.LM even with -cpu max, so long mode is
    # undetectable there - which is itself worth knowing, and is why the
    # longmode suite has two sets of assertions.
    qemu: str = QEMU


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
            sc.qemu,
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
    # Deliberately version-agnostic: pinning the number here means every
    # release bump looks like a test failure.
    ("version", [r"StratumOS \d+\.\d+\.\d+", r"boot via",
                 r"StratumOS native|Multiboot2"]),
    ("uptime", [r"up \d+:\d\d:\d\d", r"timer ticks"]),
    ("cpuinfo", [r"Vendor\s+:", r"Mode\s+: 32-bit protected mode"]),
    ("meminfo", [r"Physical memory", r"Kernel heap",
                 r"integrity : consistent", r"Firmware memory map",
                 r"kernel at : 0xc0000000", r"linear map",
                 r"address spaces created", r"COW\s+: \d+ faults",
                 r"frames held by more than one address space"]),
    # The VMSPACE column is the readable proof that a kernel thread shares the
    # kernel's page directory and a process does not.
    ("ps", [r"PID\s+PPID\s+NAME\s+STATE\s+RING\s+VMSPACE",
            r"idle\s+.*\bring0\b\s+kernel", r"\bshell\b",
            r"context switches total"]),
    ("irq", [r"IRQ\s+HANDLER\s+COUNT", r"\bpit\b",
             r"spurious: 0"]),
    ("pci", [r"ADDRESS\s+ID\s+VENDOR", r"host bridge"]),
    ("date", [r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d UTC", r"unix \d+"]),
    # The kernel lives in the higher half now, so its own text is at
    # 0xC0100000 and low addresses are user space - unmapped at boot.
    ("pagemap 0xc0101000", [r"physical  : 0x0010", r"flags     :.*present"]),
    ("pagemap 0xd0000000", [r"physical  : 0x", r"present"]),
    ("pagemap 0x1000", [r"mapping   : not present"]),
    ("hexdump 0xc0100000 16", [r"c0100000 ", r"\|"]),
    ("echo interactive shell works", [r"interactive shell works"]),
    ("help", [r"selftest", r"meminfo", r"pagemap"]),
    ("help pagemap", [r"usage: pagemap <address>"]),
    ("selftest list", [r"SUITE\s+DESCRIPTION", r"heap", r"sched"]),
    ("selftest heap", [r"ktest: heap \.\.\. PASS"]),
    ("selftest vmm", [r"ktest: vmm \.\.\. PASS"]),
    ("selftest vmspace", [r"ktest: vmspace \.\.\. PASS"]),
    ("selftest proc", [r"ktest: proc \.\.\. PASS"]),
    ("selftest harden", [r"ktest: harden \.\.\. PASS"]),
    ("harden", [r"null page\s+: unmapped",
                r"CR0\.WP\s+: set",
                r"kernel \.text\s+: read-only",
                r"kernel \.rodata\s+: read-only",
                r"SMEP \(CR4\.20\)\s+:",
                r"SMAP \(CR4\.21\)\s+:",
                r"guard pages\s+: one unmapped page below every stack",
                r"canaries\s+: checked on every context switch \(0 "
                r"failures\)",
                r"guard page at 0xe[0-9a-f]+ is unmapped, as it should be",
                r"all 255 directory slots pre-backed"]),
    ("programs", [r"exec's namespace", r"init\s+\(started at boot\)",
                  r"\bhello\b", r"/bin/INIT\s+\d+ bytes",
                  r"image\(s\) loaded from disk"]),
    ("selftest storage", [r"ktest: storage \.\.\. PASS"]),
    ("selftest fs", [r"ktest: fs \.\.\. PASS"]),
    ("selftest smp", [r"ktest: smp \.\.\. PASS"]),
    ("selftest bootpd", [r"ktest: bootpd \.\.\. PASS"]),
    # The suite passes either way: on this emulator it asserts the kernel
    # declines long mode correctly, and the `long-mode` scenario asserts the
    # transition itself under qemu-system-x86_64.
    ("selftest longmode", [r"ktest: longmode \.\.\. PASS"]),
    ("longmode", [r"Long mode \(x86-64\)", r"CPUID\s+:"]),
    # QEMU attaches a default e1000, so the shell scenario has a network
    # whether it asked for one or not - which makes these real assertions
    # rather than hopeful ones. The `network` scenario checks the wire; these
    # check that the commands report it.
    ("selftest net", [r"ktest: net \.\.\. PASS"]),
    ("net", [r"Controller", r"MAC       : [0-9a-f:]{17}",
             r"link      : up", r"receive   : 32 descriptors",
             r"address   : 10\.0\.2\.15", r"tcp echo  : port 7",
             r"udp echo  : port 7"]),
    # Either outcome is correct, and which one happens depends on whether
    # `selftest net` ran first and already resolved it. Asserting only the
    # cache-miss form made this test depend on the order of the script.
    ("arp 10.0.2.2",
     [r"(10\.0\.2\.2 is at|already cached:) [0-9a-f:]{17}"]),
    ("arp", [r"ADDRESS\s+MAC\s+AGE", r"10\.0\.2\.2"]),
    ("ping 10.0.2.2 2", [r"seq 1: reply from 10\.0\.2\.2 in \d+ ms",
                         r"seq 2: reply from 10\.0\.2\.2",
                         r"2 sent, 2 received, 0 lost"]),
    ("udpsend 10.0.2.2 7 stratum udp probe",
     [r"sent \d+ byte\(s\) to 10\.0\.2\.2:7"]),
    ("cpus", [r"Local APIC", r"CPU\s+APIC\s+ROLE\s+STATE",
              r"\s+0\s+0\s+bsp online", r"<- this one"]),
    ("disk", [r"DEV\s+MODEL\s+SECTORS\s+ADDR", r"hd0\s+\S",
              r"0 error\(s\), 0 timeout\(s\)",
              r"hd0p1\s+0e\s+\d+\s+\d+"]),
    ("mount", [r"FAT16 \"STRATUM\" on hd0p1, mounted at /",
               r"512-byte sectors, \d+ per cluster",
               r"FAT at \+\d+ \(2 x \d+ sectors\)",
               r"cache hit\(s\)"]),
    ("ls", [r"README\.TXT\s+\d+", r"BIN\s+0\s+d", r"ETC\s+0\s+d"]),
    ("ls /bin", [r"INIT\s+\d+", r"HELLO\s+\d+"]),
    ("cat /README.TXT", [r"StratumOS root filesystem",
                         r"FAT16, read-only, mounted at boot"]),
    ("cat /etc/MOTD.TXT", [r"init=/bin/INIT"]),
    ("cat /nosuchfile", [r"no such file or directory"]),
    ("cat /bin", [r"is a directory"]),
    ("stress 2 40", [r"heap integrity: consistent"]),
    ("ring3", [r"\[ring3\] hello from user mode",
               r"\[ring3\] calling exit\(0\)"]),
    ("nosuchcommand", [r"command not found"]),
    ("log debug", [r"log level set to DEBUG"]),
]


# The ring-3 syscall fuzzer gets a scenario of its own rather than a line in
# SHELL_SCRIPT, for two reasons: it runs for ~25 seconds where every other
# command finishes in under one, and its pass condition is different in kind.
# Every other scenario asserts that the kernel did something; this one asserts
# that 40,000 deliberately malformed system calls failed to make it do
# anything at all.
#
# The FORBIDDEN list is what actually tests the kernel here. The patterns
# below only confirm the run happened and completed.
SYSCALL_FUZZ_EXPECTED = [
    ("fuzzer started", r"syscall fuzzer started as pid \d+"),
    # The fuzzer's own banner is the proof that it reached ring 3: it has no
    # way to print except through the write() syscall, which is a privilege
    # transition from user mode. (The shell image boots quietly, so the
    # kernel's own "entering ring 3" line is not in this transcript.)
    ("fuzzer reached ring 3 and can only have printed from there",
     r"\[fuzz\] ring-3 syscall fuzzer"),
    ("iteration count announced",
     r"\[fuzz\] ring-3 syscall fuzzer: \d+ calls, deterministic seed"),
    ("the run advanced", r"\[fuzz\] \d+ calls, uptime \d+ s"),
    # Both counts must be non-zero: all-refused would mean the generator never
    # produced a valid call, and all-accepted would mean the kernel never
    # refused one. Either way the run proved nothing.
    ("calls were refused", r"\[fuzz\] survived: \d+ accepted, [1-9]\d* refused"),
    ("calls were accepted", r"\[fuzz\] survived: [1-9]\d* accepted,"),
    ("no panics", r"refused, 0 panics"),
    ("the kernel outlived the fuzzer",
     r"\[fuzz\] the kernel is still running"),
    ("the fuzzer exited cleanly", r"pid \d+ exited with 0"),
    # The console rate limiter has to engage, or the fuzzer would not have
    # found the flood it was built to find.
    ("the console rate limiter engaged",
     r"\(\d+ more like the next line in the last \d+ ms\)"),
    ("the shell survived and took another command",
     r"heap integrity: consistent"),
]


def run_syscall_fuzz(build_dir: Path, keep_logs: Path | None) -> Outcome:
    """Run user/fuzz.c from the shell and require the kernel to survive it."""
    image = build_dir / "stratum-shell.img"
    sc = Scenario(
        name="syscall-fuzz",
        description="40,000 malformed system calls issued from ring 3",
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

    session = SerialSession(cmd, timeout=240.0)

    if not session.read_until(PROMPT, timeout=60.0):
        failures.append("the shell prompt never appeared")
        session.close()
        return Outcome(sc, False, None, session.transcript, failures, None)

    # 180 s of headroom over the ~25 s the run takes under TCG, because a
    # machine running this in CI alongside other jobs is a slower machine.
    session.run_command("fuzz", settle=180.0)

    # Then one more command, which is the real proof: a kernel that survived
    # the fuzzer but came out of it with a corrupted heap is not a kernel that
    # survived the fuzzer.
    session.run_command("stress 2 20", settle=30.0)

    session.send_line("halt")
    session.read_until("halting", timeout=15.0)
    exit_code = session.close()

    log = session.transcript
    if keep_logs:
        keep_logs.mkdir(parents=True, exist_ok=True)
        (keep_logs / "syscall-fuzz.log").write_text(
            log, errors="backslashreplace")

    for label, pattern in SYSCALL_FUZZ_EXPECTED:
        if not re.search(pattern, log):
            failures.append(f"missing: {label}  (no match for /{pattern}/)")

    for label, pattern in FORBIDDEN:
        match = re.search(pattern, log)
        if match:
            failures.append(f"forbidden: {label} -> {match.group(0)}")

    if exit_code is None:
        failures.append("QEMU did not exit after 'halt'")
    elif exit_code != EXIT_PASS:
        failures.append(f"unexpected QEMU exit status {exit_code} after halt")

    return Outcome(sc, not failures, exit_code, log, failures, None)


# ---------------------------------------------------------------------------
# The network scenario, and the pcap it asserts on.
#
# Every other scenario checks what the kernel *says*. This one checks what it
# *put on the wire*, by having QEMU dump every frame to a pcap and parsing it
# here - independently of the kernel, with the harness recomputing the
# checksums itself.
#
# That distinction earned its keep immediately. The driver's first working
# version transmitted frames with a source MAC of 00:00:00:00:00:00: the boot
# log printed the correct address, because it printed the driver's copy, while
# the stack sent frames built from a second copy that was never filled in. The
# peer replied to the zero address and the controller's own receive filter
# dropped the reply. Transmit worked, receive was silent, and every log line
# was correct. Only the capture showed it.
# ---------------------------------------------------------------------------

def parse_pcap(path: Path) -> list[bytes]:
    """Return the frames in a libpcap file. Little-endian microsecond format,
    which is what QEMU's filter-dump writes."""
    data = path.read_bytes()

    if len(data) < 24:
        return []

    magic = data[:4]
    if magic == b"\xd4\xc3\xb2\xa1":
        endian = "<"
    elif magic == b"\xa1\xb2\xc3\xd4":
        endian = ">"
    else:
        return []

    frames = []
    off = 24

    while off + 16 <= len(data):
        _, _, caplen, _ = struct.unpack(endian + "IIII", data[off:off + 16])
        if caplen > 65535 or off + 16 + caplen > len(data):
            break
        frames.append(data[off + 16:off + 16 + caplen])
        off += 16 + caplen

    return frames


def inet_checksum(b: bytes) -> int:
    """RFC 1071, written independently of the kernel's version so that the two
    agreeing means something."""
    total = 0

    for i in range(0, len(b) - 1, 2):
        total += (b[i] << 8) | b[i + 1]
    if len(b) % 2:
        total += b[-1] << 8
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)

    return (~total) & 0xFFFF


GUEST_IP = bytes([10, 0, 2, 15])
GATEWAY_IP = bytes([10, 0, 2, 2])


def check_pcap(path: Path) -> list[str]:
    """Assert on the frames the kernel actually transmitted."""
    problems: list[str] = []

    if not path.exists():
        return [f"no packet capture at {path}"]

    frames = parse_pcap(path)

    if not frames:
        return [f"the capture at {path} holds no frames"]

    our_mac: bytes | None = None
    arp_requests = 0
    arp_replies_in = 0
    icmp_requests_out = 0
    icmp_replies_in = 0
    ip_from_us = 0
    bad_eth_src = 0
    bad_ip_checksum = 0
    bad_icmp_checksum = 0

    for f in frames:
        if len(f) < 14:
            continue

        dst, src = f[0:6], f[6:12]
        ethertype = struct.unpack(">H", f[12:14])[0]
        body = f[14:]

        # ARP, which is also where our MAC is learned - from a frame the
        # kernel sent, not from anything it claimed in a log line.
        if ethertype == 0x0806 and len(body) >= 28:
            op = struct.unpack(">H", body[6:8])[0]
            sender_mac, sender_ip = body[8:14], body[14:18]

            if op == 1 and sender_ip == GUEST_IP:
                arp_requests += 1
                if our_mac is None:
                    our_mac = sender_mac
                # A frame whose sender hardware address is all zeros is the
                # bug described above. Checked explicitly so that it can
                # never come back silently.
                if sender_mac == b"\x00" * 6:
                    bad_eth_src += 1
                if src == b"\x00" * 6:
                    bad_eth_src += 1
                if dst != b"\xff" * 6:
                    problems.append(
                        "an ARP request was not sent to the broadcast address")
            elif op == 2 and sender_ip == GATEWAY_IP:
                arp_replies_in += 1
            continue

        if ethertype != 0x0800 or len(body) < 20:
            continue

        ihl = (body[0] & 0x0F) * 4
        if ihl < 20 or ihl > len(body):
            continue

        # Every IPv4 header the kernel sent, checksummed here rather than
        # trusted. This is the assertion that the kernel's checksum code is
        # right, made by code that shares none of it.
        if body[12:16] == GUEST_IP:
            ip_from_us += 1
            if inet_checksum(body[:ihl]) != 0:
                bad_ip_checksum += 1
            if src == b"\x00" * 6:
                bad_eth_src += 1

        proto = body[9]
        total = struct.unpack(">H", body[2:4])[0]
        payload = body[ihl:total] if total <= len(body) else body[ihl:]

        if proto == 1 and len(payload) >= 8:
            icmp_type = payload[0]
            if icmp_type == 8 and body[12:16] == GUEST_IP:
                icmp_requests_out += 1
                if inet_checksum(payload) != 0:
                    bad_icmp_checksum += 1
            elif icmp_type == 0 and body[16:20] == GUEST_IP:
                icmp_replies_in += 1

    if our_mac is None:
        problems.append("no ARP request from 10.0.2.15 in the capture")
    elif our_mac == b"\x00" * 6:
        problems.append("the kernel sent frames with a zero source MAC")

    if bad_eth_src:
        problems.append(
            f"{bad_eth_src} frame(s) had a zero source hardware address")
    if not arp_requests:
        problems.append("the kernel never sent an ARP request")
    if not arp_replies_in:
        problems.append("no ARP reply from the gateway reached the capture")
    if not icmp_requests_out:
        problems.append("the kernel never sent an ICMP echo request")
    if not icmp_replies_in:
        problems.append("no ICMP echo reply came back")
    if bad_ip_checksum:
        problems.append(
            f"{bad_ip_checksum} IPv4 header(s) the kernel sent had a bad "
            f"checksum (verified independently of the kernel)")
    if bad_icmp_checksum:
        problems.append(
            f"{bad_icmp_checksum} ICMP message(s) the kernel sent had a bad "
            f"checksum")
    if ip_from_us == 0:
        problems.append("the kernel sent no IPv4 datagrams at all")

    return problems


NETWORK_EXPECTED = [
    ("the controller was found",
     r"e1000: 8254\dEM at \d\d:\d\d\.\d, MMIO 0x[0-9a-f]+ -> 0x[0-9a-f]+, "
     r"IRQ \d+, MAC (?!00:00:00:00:00:00)[0-9a-f:]{17}"),
    ("the link came up", r"e1000: link up"),
    ("the rings were set up", r"\d+ rx / \d+ tx descriptors of \d+ bytes"),
    ("an address was configured",
     r"net: address 10\.0\.2\.15 netmask 255\.255\.255\.0 "
     r"gateway 10\.0\.2\.2"),
    ("the echo ports are listening", r"tcp: listening on port 7"),
    ("the boot step reported the link",
     r"boot: network\s+\[ok\] [0-9a-f:]{17}, 10\.0\.2\.15, link up"),
    # The suite has two paths and this scenario must take the one that
    # reaches the wire. Asserting the path, not just the pass.
    ("the suite reached the hardware",
     r"ktest net: controller present, link up"),
    ("the net suite passed its wider path",
     r"ktest: net \.\.\. PASS \(9\d checks\)"),
]


def run_network(build_dir: Path, keep_logs: Path | None) -> Outcome:
    """Boot with an e1000, and assert on the frames it put on the wire."""
    image = build_dir / "stratum-test.img"
    sc = Scenario(
        name="network",
        description="an e1000, and the frames the kernel actually transmits",
        image=image,
        qemu_args=[],
        timeout=180,
    )

    failures: list[str] = []

    with tempfile.TemporaryDirectory() as tmp:
        log_path = Path(tmp) / "serial.log"
        pcap_path = Path(tmp) / "network.pcap"

        cmd = [
            QEMU, "-m", "128M", "-no-reboot", "-display", "none",
            "-serial", f"file:{log_path}",
            "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
            # An explicit controller rather than QEMU's default, so the
            # device model is pinned, and a filter that writes every frame in
            # both directions to a file.
            "-netdev", "user,id=n0",
            "-device", "e1000,netdev=n0",
            "-object", f"filter-dump,id=d0,netdev=n0,file={pcap_path}",
            "-drive", f"format=raw,file={image},index=0,media=disk",
        ]

        qemu_exit: int | None
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True,
                                  timeout=sc.timeout)
            qemu_exit = proc.returncode
        except subprocess.TimeoutExpired:
            qemu_exit = None
            failures.append(
                f"QEMU did not exit within {sc.timeout}s")

        log = log_path.read_text(errors="replace") if log_path.exists() else ""
        frames = len(parse_pcap(pcap_path)) if pcap_path.exists() else 0
        failures += check_pcap(pcap_path)

        if keep_logs:
            keep_logs.mkdir(parents=True, exist_ok=True)
            (keep_logs / "network.log").write_text(log, errors="replace")
            if pcap_path.exists():
                (keep_logs / "network.pcap").write_bytes(
                    pcap_path.read_bytes())

    for label, pattern in NETWORK_EXPECTED:
        if not re.search(pattern, log):
            failures.append(f"missing: {label}  (no match for /{pattern}/)")

    for label, pattern in FORBIDDEN:
        match = re.search(pattern, log)
        if match:
            failures.append(f"forbidden: {label} -> {match.group(0)}")

    if qemu_exit is not None and qemu_exit != EXIT_PASS:
        failures.append(f"unexpected QEMU exit status {qemu_exit}")

    summary = re.search(r"ktest: summary (\d+)/(\d+) suites passed", log)
    suites = (int(summary.group(1)), int(summary.group(2))) if summary else None

    if not summary:
        failures.append("no 'ktest: summary' line - the suites never finished")
    elif summary.group(1) != summary.group(2):
        failures.append(f"only {summary.group(1)} of {summary.group(2)} "
                        f"suites passed")

    return Outcome(sc, not failures, qemu_exit, log, failures, suites), frames


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
    # The workload is allocator traffic plus integer formatting, so a
    # profiler that discriminates has to find exactly those three functions.
    # This asserts the profiler is useful, not merely that it runs.
    ("profile found kmalloc", r"\d+\s+\d+\.\d%\s+kmalloc"),
    ("profile found kfree", r"\d+\s+\d+\.\d%\s+kfree"),
    ("profile separated the formatter's digit loop",
     r"\d+\s+\d+\.\d%\s+emit_number"),
    # The leader is whichever of the three the scheduler happened to sample
    # most, and it moves between runs - 29/23/18 one time, 32/24/14 the next.
    # So the positional check is that the top entry is one of the three
    # rather than some unrelated function: that is the part that would break
    # if attribution regressed, and naming a specific winner would be
    # asserting host scheduling noise.
    ("nothing unrelated is on top",
     r"SHARE\s+FUNCTION\s*\n\s*\d+\s+\d+\.\d%\s+"
     r"(kmalloc|kfree|emit_number)\b"),
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


# ---------------------------------------------------------------------------
# Deliberate faults
# ---------------------------------------------------------------------------
#
# Every other scenario asserts that nothing panicked. These assert that
# something *did*, because a mitigation has two halves and only one of them
# can be checked by a test that passes.
#
# The `harden` suite proves the kernel's .text has no write bit in its page
# table entry and that the page below each stack is unmapped. It cannot prove
# the CPU acts on either, because the correct outcome of trying is a dead
# kernel. So each of these boots a kernel, types one `fault` command, and
# requires the panic to name the right address, the right reason and the
# right region - which together are the evidence that the mitigation is doing
# something rather than merely being configured.

FAULT_CASES: list[tuple[str, str, list[str]]] = [
    (
        "fault-text",
        "fault text",
        [
            r"writing to the kernel's own \.text from ring 0",
            r"faulting address: 0xc01[0-9a-f]{5}",
            r"access\s+: write from ring 0",
            r"reason\s+: the page is mapped read-only \(CR0\.WP applies to "
            r"ring 0 too\)",
            r"region\s+: the kernel's own code or constants, which are "
            r"read-only",
            r"KERNEL PANIC",
            # The symbol table has to survive a fault in the kernel's own
            # text, which is where it is least convenient to need it.
            r"at\s+cmd_fault\+0x",
        ],
    ),
    (
        "fault-stackguard",
        "fault stackguard",
        [
            r"writing below this task's kernel stack",
            r"faulting address: 0xe000[0-9a-f]{4}",
            r"reason\s+: nothing is mapped at that address",
            r"region\s+: a kernel-stack guard page - a task overran its "
            r"stack",
            r"KERNEL PANIC",
        ],
    ),
]


def run_fault_case(build_dir: Path, name: str, command: str,
                   patterns: list[str], keep_logs: Path | None) -> Outcome:
    image = build_dir / "stratum-shell.img"
    sc = Scenario(
        name=name,
        description=f"`{command}` must panic, and say why",
        image=image,
        qemu_args=[],
    )

    failures: list[str] = []

    cmd = [
        QEMU, "-m", "128M", "-cpu", "max", "-no-reboot", "-display", "none",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
        "-serial", "stdio",
        "-drive", f"format=raw,file={image},index=0,media=disk",
    ]

    session = SerialSession(cmd, timeout=40.0)

    if not session.read_until(PROMPT, timeout=40.0):
        session.close()
        return Outcome(sc, False, None, session.transcript,
                       ["the shell prompt never appeared"], None)

    session.send_line(command)

    # The kernel is about to die, so there is no prompt to wait for. Read
    # until the panic banner, then keep draining: the register dump, the
    # resolved symbol and the call trace all come *after* it, and those are
    # most of what this scenario asserts on.
    session.read_until("KERNEL PANIC", timeout=20.0)
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        session.read_until("\x00", timeout=0.5)

    qemu_exit = session.close()
    log = session.transcript

    if keep_logs:
        keep_logs.mkdir(parents=True, exist_ok=True)
        (keep_logs / f"{name}.log").write_text(log)

    for pattern in patterns:
        if not re.search(pattern, log):
            failures.append(f"missing: /{pattern}/")

    # A panic exits with 35. Anything else - a clean exit above all - means
    # the fault did not happen, which is the failure this scenario exists to
    # catch.
    if qemu_exit != EXIT_PANIC:
        failures.append(
            f"QEMU exited {qemu_exit}, expected {EXIT_PANIC} (a panic). "
            f"The write was supposed to fault and did not."
        )

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
                # Only this path has a disk, so only this path can assert on
                # the whole storage stack: IDENTIFY, the partition table the
                # MBR carries alongside stage 1, the mount, and init being
                # read out of the filesystem rather than out of the kernel.
                ("the disk identified itself",
                 r"ata: hd0: \S.*, \d+ sectors"),
                ("the MBR partition was parsed",
                 r"blk: hd0p1: type 0e \(FAT\), LBA \d+ \+ \d+ sectors"),
                ("the filesystem mounted",
                 r"fat: mounted hd0p1: FAT16 \"STRATUM\", \d+ KiB, "
                 r"\d+ clusters"),
                ("init came off the disk",
                 r"entering ring 3 at 0x[0-9a-f]+ in its own address space "
                 r"\(\d+ user pages mapped, \"init\" from the filesystem\)"),
                ("exec read its image from the disk",
                 r"exec\(\"hello\"\) from the filesystem"),
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
            name="no-network",
            description="the same kernel on a machine with no Ethernet at all",
            image=build_dir / "stratum-test.img",
            qemu_args=[
                # QEMU adds a default e1000 unless told not to, which means
                # every other scenario has a network whether it asked for one
                # or not - and the path where there is no controller would
                # never run. This scenario is the one that exercises it.
                "-nic", "none",
                "-drive",
                f"format=raw,file={build_dir / 'stratum-test.img'},"
                f"index=0,media=disk",
            ],
            extra_expected=[
                ("the absence was reported, not failed",
                 r"boot: network\s+\[ok\] no supported controller"),
                ("the driver said so plainly",
                 r"e1000: no supported Ethernet controller on the PCI bus"),
                # The suite's narrower path. Asserting the count is what
                # distinguishes "took the right branch" from "happened to
                # pass".
                ("the net suite took its narrower path",
                 r"ktest net: controller absent, link down"),
                ("and still tested everything that does not need hardware",
                 r"ktest: net \.\.\. PASS \(6\d checks\)"),
            ],
        ),
        Scenario(
            name="long-mode",
            description="32-bit protected mode to 64-bit long mode, and back",
            image=build_dir / "stratum-test.img",
            qemu="qemu-system-x86_64",
            qemu_args=[
                # The same 32-bit image, on a processor that has x86-64.
                # qemu-system-i386 masks CPUID.80000001H:EDX.LM even with
                # -cpu max, so on every other scenario the longmode suite
                # takes its declining path and asserts the kernel leaves the
                # machine alone. Here it takes the real one.
                #
                # This is also the realistic configuration: a 32-bit kernel
                # on a 64-bit-capable machine is what actual hardware looks
                # like, and the one the transition would run on.
                "-drive",
                f"format=raw,file={build_dir / 'stratum-test.img'},"
                f"index=0,media=disk",
            ],
            extra_expected=[
                ("long mode was detected",
                 r"CPUID\.80000001H:EDX\.LM is set|"
                 r"64-bit long mode entered and left"),
                # The kernel's own line, which only prints after the round
                # trip validated: every stage reached, EFER.LMA seen set from
                # inside 64-bit mode, and the kernel back in 32-bit paging.
                ("the round trip completed",
                 r"lm: 64-bit long mode entered and left: CS 0x18, "
                 r"EFER\.LMA set, \d+ MiB identity-mapped by a 4-level "
                 r"table, round trip \d+ cycles"),
                # Twice: the suite runs it again to prove the tables are
                # reused rather than rebuilt, which is where a cleanup bug
                # would show.
                ("the transition is repeatable",
                 r"(?s)64-bit long mode entered and left.*"
                 r"64-bit long mode entered and left"),
                ("the suite took its real path, not its declining one",
                 r"ktest: longmode \.\.\. PASS \(5\d checks\)"),
            ],
        ),
        Scenario(
            name="hardened-cpu",
            description="the same kernel on a CPU that has SMEP and SMAP",
            image=build_dir / "stratum-test.img",
            qemu_args=[
                # QEMU's default i386 model does not implement CPUID leaf 7,
                # so SMEP and SMAP cannot be detected and the kernel takes
                # its fallback path - which the other scenarios test. This
                # one runs the same image on a CPU that has both, so the
                # stac/clac discipline around every kernel access to user
                # memory is actually exercised. A missing window shows up
                # here as a page fault, which is how the first version of
                # the vmspace suite was caught.
                "-cpu", "max",
                "-drive",
                f"format=raw,file={build_dir / 'stratum-test.img'},"
                f"index=0,media=disk",
            ],
            extra_expected=[
                ("SMEP and SMAP both enabled",
                 r"harden: SMEP enabled, SMAP enabled"),
                ("the boot step says so",
                 r"hardening\s+\[ok\] W\^X, guard pages, SMEP \+ SMAP"),
            ],
        ),
        Scenario(
            name="four-processors",
            description="the same kernel on four processors",
            image=build_dir / "stratum-test.img",
            qemu_args=[
                # The other scenarios run on one processor, which is the path
                # that has to keep working and is what almost every reader
                # will build on. This one brings up four, so the trampoline,
                # the per-CPU GDT entries, the real spinlocks and the IPI
                # paths are all exercised - none of which a uniprocessor boot
                # touches at all.
                "-smp", "4",
                "-cpu", "max",
                "-drive",
                f"format=raw,file={build_dir / 'stratum-test.img'},"
                f"index=0,media=disk",
            ],
            extra_expected=[
                ("the MADT was parsed",
                 r"acpi: MADT: 4 processor\(s\), \d+ I/O APIC\(s\)"),
                ("the local APIC came up",
                 r"apic: local APIC at 0xfee00000 -> 0x[0-9a-f]+, id 0"),
                ("every application processor started",
                 r"smp: cpu 3 online \(APIC id 3\)"),
                ("all four are online",
                 r"smp: 4 of 4 processor\(s\) online"),
                ("the boot step agrees",
                 r"processors\s+\[ok\] 4 of 4 processor\(s\) online"),
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
                # The other half of the storage story. The only drive here is
                # an ATAPI CD-ROM, which IDENTIFY refuses, so there is no
                # block device and no filesystem - and the kernel has to fall
                # back to the programs embedded in its own image. That
                # fallback is the reason they still exist, and this is what
                # tests it.
                ("no ATA drive found", r"ata: no ATA drives found"),
                ("no block devices", r"blk: no block devices"),
                ("the fallback was used",
                 r"boot: filesystem\s+\[ok\] none present; using the "
                 r"embedded programs"),
                ("init came from the kernel image",
                 r"\"init\" from the kernel image\)"),
            ],
        ),
    ]

    if only in ("interactive-shell", "benchmarks", "syscall-fuzz",
                "network") or (only or "").startswith("fault-"):
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
    # build/x86, not build: the build is parameterised by ARCH and each
    # architecture owns a subtree, so that two can be built side by side and
    # neither can pick up the other's stale objects. This harness is the x86
    # one - riscv64 has tools/run-riscv64.py, for the reasons in
    # docs/PORTING.md.
    ap.add_argument("--build-dir", type=Path, default=Path("build/x86"))
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

    # A scenario that needs an emulator this machine does not have is skipped
    # loudly rather than failed. Only the long-mode scenario is in that
    # position today, and only because it needs qemu-system-x86_64 to see a
    # processor with x86-64 on it. Printing the name keeps a skip from
    # reading as a pass.
    unavailable = [s for s in scenarios if not shutil.which(s.qemu)]
    for s in unavailable:
        print(f"  [{s.name}] SKIP ({s.qemu} is not installed)\n")
    scenarios = [s for s in scenarios if s not in unavailable]

    missing = [s for s in scenarios if not s.image.is_file()]
    if missing:
        for s in missing:
            print(f"run-tests: {s.image} does not exist - run 'make' first",
                  file=sys.stderr)
        return 2

    emulators = sorted({s.qemu for s in scenarios})
    if scenarios:
        print(f"run-tests: {len(scenarios)} boot scenario(s) under "
              f"{', '.join(emulators)}\n")

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

    if args.only in (None, "network"):
        test_image = args.build_dir / "stratum-test.img"
        if test_image.is_file():
            print("  [network] an e1000, and the frames the kernel actually "
                  "transmits")
            outcome, frames = run_network(args.build_dir, args.keep_logs)
            outcomes.append(outcome)
            print(f"    frames captured  : {frames}")
            print(f"    qemu exit        : {outcome.qemu_exit}")
            if outcome.suites:
                print(f"    in-kernel suites : {outcome.suites[0]}/"
                      f"{outcome.suites[1]} passed")
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
                print("    --- end ---\n")
        else:
            print(f"  [network] SKIP ({test_image} not built)\n")

    if args.only in (None, "syscall-fuzz"):
        shell_image = args.build_dir / "stratum-shell.img"
        if shell_image.is_file():
            print("  [syscall-fuzz] 40,000 malformed system calls issued "
                  "from ring 3")
            outcome = run_syscall_fuzz(args.build_dir, args.keep_logs)
            outcomes.append(outcome)
            print(f"    qemu exit        : {outcome.qemu_exit}")
            survived = re.search(r"\[fuzz\] survived: (\d+) accepted, "
                                 r"(\d+) refused", outcome.log)
            if survived:
                print(f"    calls accepted   : {survived.group(1)}")
                print(f"    calls refused    : {survived.group(2)}")
            if outcome.passed:
                print("    result           : PASS\n")
            else:
                print("    result           : FAIL")
                for f in outcome.failures:
                    print(f"      - {f}")
                print()
                print("    --- transcript ---")
                for line in outcome.log.splitlines():
                    print(f"    | {line}")
                print("    --- end ---\n")
        else:
            print(f"  [syscall-fuzz] SKIP ({shell_image} not built)\n")

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

    for name, command, patterns in FAULT_CASES:
        if args.only not in (None, name):
            continue
        shell_image = args.build_dir / "stratum-shell.img"
        if not shell_image.is_file():
            print(f"  [{name}] SKIP ({shell_image} not built)\n")
            continue

        print(f"  [{name}] `{command}` must panic, and say why")
        outcome = run_fault_case(args.build_dir, name, command, patterns,
                                 args.keep_logs)
        outcomes.append(outcome)
        print(f"    qemu exit        : {outcome.qemu_exit} "
              f"(expected {EXIT_PANIC}, a panic)")
        if outcome.passed:
            print("    result           : PASS\n")
        else:
            print("    result           : FAIL")
            for f in outcome.failures:
                print(f"      - {f}")
            print()
            print("    --- transcript ---")
            for line in outcome.log.splitlines():
                print(f"    | {line}")
            print("    --- end ---\n")

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
