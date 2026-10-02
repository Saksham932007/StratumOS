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
  * the higher-half split is real: the boot segment has VMA == LMA so that it
    can run with paging off, and every other segment is linked exactly
    KERNEL_VIRT_BASE above where it loads;
  * the embedded user program is a valid ELF whose segments all lie below
    KERNEL_VIRT_BASE - a user image that could map into kernel space would be
    a request to overwrite the kernel;
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
KERNEL_VIRT_BASE = 0xC0000000
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

        # .multiboot is an input section that the link script folds into
        # .boot, so it is the latter that must exist.
        for required in (".boot", ".text", ".rodata", ".bss"):
            if required not in secs:
                self.fail(f"section {required} is missing")

        if ".boot" in secs:
            boot = secs[".boot"]
            if boot["off"] >= MB2_SEARCH_LIMIT:
                self.fail(f".boot is at file offset {boot['off']:#x}, past "
                          f"the {MB2_SEARCH_LIMIT} byte window a Multiboot2 "
                          f"loader searches for the header")
            if boot["addr"] != EXPECTED_LOAD_ADDR:
                self.fail(f".boot is at {boot['addr']:#x}, expected "
                          f"{EXPECTED_LOAD_ADDR:#x}. It must run with paging "
                          f"off, so its address cannot be in the higher half.")
            self.note(f".boot is {boot['size']} bytes at {boot['addr']:#x}")

        if ".text" in secs:
            text = secs[".text"]
            if text["addr"] < KERNEL_VIRT_BASE:
                self.fail(f".text is at {text['addr']:#x}, below "
                          f"KERNEL_VIRT_BASE ({KERNEL_VIRT_BASE:#x}). The "
                          f"kernel is supposed to be linked in the higher "
                          f"half.")

        if ".bss" in secs:
            bss = secs[".bss"]
            if bss["addr"] % 4 or bss["size"] % 4:
                self.fail(f".bss spans {bss['addr']:#x}+{bss['size']:#x}, "
                          f"which is not 4-byte aligned; _start clears it in "
                          f"bulk and would miss the tail")
            self.note(f".bss is {bss['size'] // 1024} KiB")

    def check_higher_half_split(self) -> None:
        """The premise of the design: .boot runs where it is loaded, and
        everything else is linked exactly KERNEL_VIRT_BASE higher."""
        loads = [p for p in self.program_headers() if p["type"] == 1]

        if not loads:
            return

        loads.sort(key=lambda p: p["paddr"])
        boot = loads[0]

        if boot["vaddr"] != boot["paddr"]:
            self.fail(f"the first load segment has vaddr {boot['vaddr']:#x} "
                      f"and paddr {boot['paddr']:#x}; they must be equal, "
                      f"because that code runs before paging exists")
        else:
            self.note(f"boot segment runs where it loads ({boot['paddr']:#x})")

        for p in loads[1:]:
            delta = p["vaddr"] - p["paddr"]
            if delta != KERNEL_VIRT_BASE:
                self.fail(f"segment at paddr {p['paddr']:#x} is linked "
                          f"{delta:#x} higher, expected exactly "
                          f"{KERNEL_VIRT_BASE:#x}. Check the AT() expressions "
                          f"in linker/kernel.ld.")

        if len(loads) > 1:
            self.note(f"{len(loads) - 1} higher-half segment(s), all at "
                      f"+{KERNEL_VIRT_BASE:#x}")

    def vaddr_to_offset(self, vaddr: int) -> int | None:
        for p in self.program_headers():
            if p["type"] != 1:
                continue
            if p["vaddr"] <= vaddr < p["vaddr"] + p["filesz"]:
                return p["offset"] + (vaddr - p["vaddr"])
        return None

    def check_embedded_user_program(self) -> None:
        """The ring-3 program is a separate ELF embedded as a blob. Validate
        it here, because a user image whose segments reach into kernel space
        would be asking the kernel to overwrite itself on its behalf."""
        try:
            out = subprocess.run(["nm", str(self.path)], capture_output=True,
                                 text=True, check=True).stdout
        except (subprocess.CalledProcessError, FileNotFoundError):
            self.note("nm unavailable; skipped the embedded-program check")
            return

        syms: dict[str, int] = {}
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[2].startswith("_binary_init_elf_"):
                syms[parts[2]] = int(parts[0], 16)

        start = syms.get("_binary_init_elf_start")
        end = syms.get("_binary_init_elf_end")

        if start is None or end is None:
            self.fail("the embedded user program is missing "
                      "(_binary_init_elf_start/_end not found). Ring 3 would "
                      "have nothing to run.")
            return

        off = self.vaddr_to_offset(start)
        if off is None:
            self.fail(f"the embedded user program's address {start:#x} is not "
                      f"inside any loadable segment")
            return

        blob = self.data[off:off + (end - start)]

        if len(blob) < 52 or blob[:4] != b"\x7fELF":
            self.fail("the embedded user program is not an ELF file")
            return

        e_type, e_machine = struct.unpack_from("<HH", blob, 16)
        if e_type != 2 or e_machine != 3:
            self.fail(f"the embedded user program has e_type {e_type} / "
                      f"e_machine {e_machine}; ET_EXEC/EM_386 required")
            return

        u_entry, u_phoff = struct.unpack_from("<II", blob, 24)
        u_phentsize, u_phnum = struct.unpack_from("<HH", blob, 42)

        if u_entry >= KERNEL_VIRT_BASE:
            self.fail(f"the embedded user program's entry point {u_entry:#x} "
                      f"is in kernel space")

        user_loads = 0
        for i in range(u_phnum):
            base = u_phoff + i * u_phentsize
            if base + 32 > len(blob):
                self.fail("the embedded user program's program headers run "
                          "past the end of the blob")
                return
            (p_type, _p_off, p_vaddr, _p_paddr, _p_filesz, p_memsz,
             _p_flags, _p_align) = struct.unpack_from("<8I", blob, base)
            if p_type != 1:
                continue
            user_loads += 1
            if p_vaddr < PAGE:
                self.fail(f"a user segment at {p_vaddr:#x} overlaps the null "
                          f"page, which must stay unmapped")
            if p_vaddr >= KERNEL_VIRT_BASE or \
               p_vaddr + p_memsz > KERNEL_VIRT_BASE:
                self.fail(f"a user segment spans {p_vaddr:#x}+{p_memsz:#x}, "
                          f"reaching into kernel space")

        if user_loads == 0:
            self.fail("the embedded user program has no loadable segments")

        self.note(f"embedded user program: {len(blob)} bytes, entry "
                  f"{u_entry:#x}, {user_loads} segment(s), all below "
                  f"{KERNEL_VIRT_BASE:#x}")

    # ---- multiboot2 header ----    # ---- multiboot2 header ----------------------------------------------

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
        self.check_higher_half_split()
        self.check_embedded_user_program()
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


def loadable_image(path: Path) -> list[tuple[int, int, bytes]]:
    """Every PT_LOAD segment as (paddr, memsz, file contents)."""
    data = path.read_bytes()
    phoff, = struct.unpack_from("<I", data, 28)
    phentsize, phnum = struct.unpack_from("<HH", data, 42)

    out = []
    for i in range(phnum):
        base = phoff + i * phentsize
        (p_type, p_offset, _p_vaddr, p_paddr, p_filesz, p_memsz,
         _p_flags, _p_align) = struct.unpack_from("<8I", data, base)
        if p_type != 1:
            continue
        out.append((p_paddr, p_memsz, data[p_offset:p_offset + p_filesz]))

    out.sort(key=lambda s: s[0])
    return out


def compare_loadable(a: Path, b: Path, verbose: bool) -> int:
    """Confirm that stripping changed nothing a loader will see.

    The validator's symbol-dependent checks need the unstripped ELF, but it is
    the stripped one that boots. Rather than trust that `objcopy --strip-debug`
    only removes non-loadable sections, check it."""
    try:
        seg_a = loadable_image(a)
        seg_b = loadable_image(b)
    except (struct.error, IndexError) as e:
        print(f"check-kernel: FAIL: cannot compare loadable images: {e}",
              file=sys.stderr)
        return 1

    if len(seg_a) != len(seg_b):
        print(f"check-kernel: FAIL: {a.name} has {len(seg_a)} load segments "
              f"but {b.name} has {len(seg_b)}", file=sys.stderr)
        return 1

    for (pa, ma, da), (pb, mb, db) in zip(seg_a, seg_b):
        if pa != pb or ma != mb or da != db:
            print(f"check-kernel: FAIL: the loadable image differs at paddr "
                  f"{pa:#x} - stripping changed what gets booted",
                  file=sys.stderr)
            return 1

    if verbose:
        total = sum(m for _, m, _ in seg_a)
        print(f"    stripped image is identical: {len(seg_a)} segments, "
              f"{total // 1024} KiB")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("kernel", type=Path)
    ap.add_argument("--verbose", "-v", action="store_true")
    ap.add_argument("--matches", type=Path,
                    help="a second ELF (typically the stripped, bootable one) "
                         "whose loadable image must be byte-for-byte "
                         "identical to this one's")
    args = ap.parse_args()

    if not args.kernel.is_file():
        print(f"check-kernel: {args.kernel} does not exist", file=sys.stderr)
        return 1

    checker = Checker(args.kernel, args.verbose)
    rc = checker.run()

    if args.matches:
        rc |= compare_loadable(args.kernel, args.matches, args.verbose)

    return rc


if __name__ == "__main__":
    raise SystemExit(main())
