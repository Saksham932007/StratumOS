#!/usr/bin/env python3
"""Validate a freshly linked StratumOS kernel.

The link step succeeding says almost nothing about whether the result can
boot. This runs the checks whose failure modes are otherwise silent:

  * the Multiboot2 header is present, within the first 32 KiB, 8-byte aligned,
    and its checksum actually sums to zero;
  * the ELF is 32-bit, little-endian, ET_EXEC, EM_386;
  * the entry point lands inside a loadable segment;
  * the load address is 1 MiB, as both boot paths assume;
  * .bss is 4-byte aligned at both ends, because _start zeroes it in bulk;
  * the .user section is page-aligned on both ends, because its pages get
    remapped as ring-3 accessible and a partial page would expose kernel data;
  * no SSE/MMX instructions crept in from the compiler.

Each of these has cost somebody an afternoon of QEMU bisection at some point.
Checking them takes 30 milliseconds.
"""

from __future__ import annotations

import argparse
import struct
import subprocess
import sys
from pathlib import Path

MB2_MAGIC = 0xE85250D6
MB2_SEARCH_LIMIT = 32768
EXPECTED_LOAD_ADDR = 0x100000
PAGE = 4096


class Checker:
    def __init__(self, path: Path, verbose: bool) -> None:
        self.path = path
        self.verbose = verbose
        self.data = path.read_bytes()
        self.problems: list[str] = []
        self.notes: list[str] = []

    def fail(self, message: str) -> None:
        self.problems.append(message)

    def note(self, message: str) -> None:
        self.notes.append(message)

    # ---- ELF parsing ----------------------------------------------------

    def check_elf_header(self) -> None:
        d = self.data

        if len(d) < 52:
            self.fail("file is too short to be an ELF32 image")
            return
        if d[:4] != b"\x7fELF":
            self.fail("not an ELF file")
            return
        if d[4] != 1:
            self.fail(f"EI_CLASS is {d[4]}, expected 1 (ELFCLASS32)")
        if d[5] != 1:
            self.fail(f"EI_DATA is {d[5]}, expected 1 (little-endian)")

        e_type, e_machine = struct.unpack_from("<HH", d, 16)
        if e_type != 2:
            self.fail(f"e_type is {e_type}, expected 2 (ET_EXEC). A PIE or "
                      f"shared object cannot be booted.")
        if e_machine != 3:
            self.fail(f"e_machine is {e_machine}, expected 3 (EM_386)")

        self.entry, self.phoff, _ = struct.unpack_from("<III", d, 24)
        self.phentsize, self.phnum = struct.unpack_from("<HH", d, 42)
        self.note(f"entry point {self.entry:#010x}")

    def program_headers(self) -> list[dict]:
        out = []
        for i in range(self.phnum):
            off = self.phoff + i * self.phentsize
            (p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
             p_flags, p_align) = struct.unpack_from("<8I", self.data, off)
            out.append(dict(type=p_type, offset=p_offset, vaddr=p_vaddr,
                            paddr=p_paddr, filesz=p_filesz, memsz=p_memsz,
                            flags=p_flags, align=p_align))
        return out

    def check_segments(self) -> None:
        loads = [p for p in self.program_headers() if p["type"] == 1]

        if not loads:
            self.fail("no PT_LOAD segments; there is nothing for a loader "
                      "to copy")
            return

        lowest = min(p["paddr"] for p in loads)
        if lowest != EXPECTED_LOAD_ADDR:
            self.fail(f"lowest load address is {lowest:#x}, expected "
                      f"{EXPECTED_LOAD_ADDR:#x}. Both boot paths assume the "
                      f"kernel sits at 1 MiB.")

        in_segment = any(p["paddr"] <= self.entry < p["paddr"] + p["memsz"]
                         for p in loads)
        if not in_segment:
            self.fail(f"entry point {self.entry:#x} is not inside any "
                      f"loadable segment")

        total = sum(p["memsz"] for p in loads)
        self.note(f"{len(loads)} PT_LOAD segment(s), {total // 1024} KiB "
                  f"resident")
        for p in loads:
            self.note(f"  paddr {p['paddr']:#010x} filesz {p['filesz']:>7} "
                      f"memsz {p['memsz']:>7} align {p['align']:>5}")

    # ---- sections -------------------------------------------------------

    def sections(self) -> dict[str, dict]:
        out = subprocess.run(
            ["readelf", "-S", "-W", str(self.path)],
            capture_output=True, text=True, check=True).stdout

        result: dict[str, dict] = {}
        for line in out.splitlines():
            line = line.strip()
            if not line.startswith("["):
                continue
            # [ N] name type addr off size ...
            try:
                rest = line.split("]", 1)[1].split()
                name, _stype, addr, off, size = rest[0], rest[1], rest[2], rest[3], rest[4]
                result[name] = dict(addr=int(addr, 16), off=int(off, 16),
                                    size=int(size, 16))
            except (IndexError, ValueError):
                continue
        return result

    def check_sections(self) -> None:
        secs = self.sections()

        for required in (".text", ".rodata", ".bss", ".multiboot"):
            if required not in secs:
                self.fail(f"section {required} is missing")

        if ".multiboot" in secs:
            mb = secs[".multiboot"]
            if mb["off"] >= MB2_SEARCH_LIMIT:
                self.fail(f".multiboot is at file offset {mb['off']:#x}, past "
                          f"the {MB2_SEARCH_LIMIT} byte window a Multiboot2 "
                          f"loader searches")
            if mb["addr"] % 8:
                self.fail(f".multiboot is at {mb['addr']:#x}, which is not "
                          f"8-byte aligned as the spec requires")

        if ".bss" in secs:
            bss = secs[".bss"]
            if bss["addr"] % 4 or bss["size"] % 4:
                self.fail(f".bss spans {bss['addr']:#x}+{bss['size']:#x}, "
                          f"which is not 4-byte aligned; _start clears it in "
                          f"bulk and would miss the tail")
            self.note(f".bss is {bss['size'] // 1024} KiB")

        if ".user" in secs:
            u = secs[".user"]
            if u["addr"] % PAGE or u["size"] % PAGE:
                self.fail(f".user spans {u['addr']:#x}+{u['size']:#x}, which "
                          f"is not page aligned. Its pages are remapped as "
                          f"ring-3 readable, so a partial page would expose "
                          f"adjacent kernel memory to userspace.")
            self.note(f".user is {u['size']} bytes at {u['addr']:#x}")
        else:
            self.fail("section .user is missing; the ring-3 demo has nowhere "
                      "to live")

    # ---- multiboot2 header ----------------------------------------------

    def check_multiboot(self) -> None:
        window = self.data[:MB2_SEARCH_LIMIT]
        needle = struct.pack("<I", MB2_MAGIC)

        pos = -1
        for candidate in range(0, len(window) - 16, 8):
            if window[candidate:candidate + 4] == needle:
                pos = candidate
                break

        if pos < 0:
            self.fail("no Multiboot2 magic found in the first 32 KiB; GRUB "
                      "would reject this kernel")
            return

        magic, arch, length, checksum = struct.unpack_from("<4I", self.data, pos)

        if arch != 0:
            self.fail(f"Multiboot2 architecture is {arch}, expected 0 (i386)")

        total = (magic + arch + length + checksum) & 0xFFFFFFFF
        if total != 0:
            self.fail(f"Multiboot2 checksum is wrong: the four header words "
                      f"sum to {total:#x}, not 0")

        if pos + length > len(self.data):
            self.fail(f"Multiboot2 header claims {length} bytes but runs past "
                      f"the end of the file")

        self.note(f"Multiboot2 header at offset {pos:#x}, {length} bytes, "
                  f"checksum valid")

    # ---- instruction set -------------------------------------------------

    def check_no_sse(self) -> None:
        """The kernel never enables SSE in CR4, so any SSE instruction the
        compiler emitted would raise #UD at boot. -mgeneral-regs-only is
        supposed to prevent this; verify rather than trust."""
        try:
            out = subprocess.run(
                ["objdump", "-d", "--no-show-raw-insn", str(self.path)],
                capture_output=True, text=True, check=True).stdout
        except (subprocess.CalledProcessError, FileNotFoundError):
            self.note("objdump unavailable; skipped the SSE scan")
            return

        banned = ("movaps", "movups", "movss", "movsd ", "addps", "mulps",
                  "xorps", "movdqa", "movdqu", "punpck", "pxor", "emms")
        hits: list[str] = []

        for line in out.splitlines():
            if "\t" not in line:
                continue
            text = line.split("\t", 1)[1].strip()
            for insn in banned:
                if text.startswith(insn):
                    hits.append(text)
                    break

        if hits:
            shown = ", ".join(sorted(set(hits))[:5])
            self.fail(f"{len(hits)} SSE/MMX instruction(s) present ({shown}). "
                      f"The kernel does not enable SSE in CR4, so these would "
                      f"fault. Check that -mgeneral-regs-only is in CFLAGS.")
        else:
            self.note("no SSE/MMX instructions (good: CR4.OSFXSR is never set)")

    # ---- driver ----------------------------------------------------------

    def run(self) -> int:
        self.check_elf_header()
        if self.problems:
            self.report()
            return 1

        self.check_segments()
        self.check_multiboot()
        self.check_sections()
        self.check_no_sse()
        self.report()
        return 1 if self.problems else 0

    def report(self) -> None:
        if self.verbose:
            for n in self.notes:
                print(f"    {n}")

        for p in self.problems:
            print(f"check-kernel: FAIL: {p}", file=sys.stderr)

        if self.problems:
            print(f"check-kernel: {len(self.problems)} problem(s) in "
                  f"{self.path}", file=sys.stderr)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kernel", type=Path)
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    if not args.kernel.is_file():
        print(f"check-kernel: {args.kernel} does not exist", file=sys.stderr)
        return 1

    return Checker(args.kernel, args.verbose).run()


if __name__ == "__main__":
    raise SystemExit(main())
