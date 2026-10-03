#!/usr/bin/env python3
"""Seed the fuzz corpora from inputs the kernel itself produces.

A fuzzer starting from nothing spends its first minutes rediscovering what an
ELF header looks like, and its first hour finding a FAT boot sector that will
mount. Neither is where the bugs are. Seeding with real artefacts - the
project's own ring-3 binary, its own filesystem image - means the first
mutation is already inside the parser rather than at its front door.

Each seed is also a minimised, structurally valid example, which is what
libFuzzer's mutators work best from: flipping a length field in a valid ELF
reaches a bounds check, while flipping a byte in random noise reaches the
magic-number test it has already failed a thousand times.

The generated seeds are deliberately small and deliberately varied in *kind*
rather than in content: one of each shape the parser has a distinct path for.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


def write(path: Path, name: str, data: bytes) -> int:
    path.mkdir(parents=True, exist_ok=True)
    (path / name).write_bytes(data)
    return len(data)


# ---- ELF ------------------------------------------------------------------

def elf32(e_type=2, e_machine=3, phoff=52, phnum=1, entry=0x00400080,
          segments=((1, 0x1000, 0x00400000, 0x100, 0x100, 5),),
          phentsize=32, body=b"") -> bytes:
    """A minimal ELF32, with every field a parameter so the seeds can differ
    in exactly one thing each."""
    ident = b"\x7fELF" + bytes([1, 1, 1, 0, 0]) + b"\0" * 7
    header = ident + struct.pack(
        "<HHIIIIIHHHHHH",
        e_type, e_machine, 1, entry, phoff, 0, 0, 52, phentsize, phnum,
        40, 0, 0)

    table = b""
    for p_type, offset, vaddr, filesz, memsz, flags in segments:
        table += struct.pack("<8I", p_type, offset, vaddr, vaddr, filesz,
                             memsz, flags, 0x1000)

    out = bytearray(header + table)
    if len(out) < 0x1000:
        out += b"\0" * (0x1000 - len(out))
    out += body or (b"\x90" * 0x100)
    return bytes(out)


def seed_elf(corpus: Path, user_elf: Path | None) -> list[str]:
    out = []

    if user_elf and user_elf.is_file():
        n = write(corpus, "real-init.elf", user_elf.read_bytes())
        out.append(f"real-init.elf ({n} bytes, the kernel's own ring-3 program)")

    # One valid image, so the mutator has something whose every field means
    # something.
    out.append(f"minimal.elf ({write(corpus, 'minimal.elf', elf32())} bytes)")

    # Two segments sharing a page, which is the case the loader's
    # second-pass permission tightening exists for.
    out.append("shared-page.elf")
    write(corpus, "shared-page.elf", elf32(
        phnum=2,
        segments=((1, 0x1000, 0x00400000, 0x80, 0x80, 5),
                  (1, 0x1080, 0x00400080, 0x80, 0x200, 6))))

    # A .bss tail: memsz beyond filesz, which the loader has to zero.
    out.append("bss-tail.elf")
    write(corpus, "bss-tail.elf", elf32(
        segments=((1, 0x1000, 0x00400000, 0x10, 0x3000, 6),)))

    # Each of these is a rejection the loader has a distinct message for.
    # Seeding them means the mutator starts *at* each check rather than
    # having to find it.
    for name, kw in [
        ("bad-magic.elf", {}),
        ("wrong-type.elf", {"e_type": 3}),
        ("wrong-machine.elf", {"e_machine": 62}),
        ("huge-phnum.elf", {"phnum": 60000}),
        ("phoff-past-end.elf", {"phoff": 0xFFFF0000}),
        ("tiny-phentsize.elf", {"phentsize": 8}),
        ("kernel-vaddr.elf", {"segments": ((1, 0x1000, 0xC0100000, 0x100,
                                           0x100, 6),)}),
        ("null-page.elf", {"segments": ((1, 0x1000, 0x0, 0x100, 0x100, 6),)}),
        ("filesz-past-end.elf", {"segments": ((1, 0x1000, 0x00400000,
                                               0xFFFFFF, 0xFFFFFF, 6),)}),
        ("memsz-lt-filesz.elf", {"segments": ((1, 0x1000, 0x00400000, 0x200,
                                               0x10, 6),)}),
        ("entry-outside.elf", {"entry": 0xDEADBEEF}),
        ("wrapping-segment.elf", {"segments": ((1, 0x1000, 0xBFFFF000,
                                                0x100000, 0x100000, 6),)}),
    ]:
        data = bytearray(elf32(**kw))
        if name == "bad-magic.elf":
            data[1] = ord("X")
        write(corpus, name, bytes(data))
        out.append(name)

    return out


# ---- FAT16 ----------------------------------------------------------------

def seed_fat(corpus: Path, fs_image: Path | None) -> list[str]:
    out = []

    if fs_image and fs_image.is_file():
        data = fs_image.read_bytes()
        # The whole 16 MiB image is far larger than the target's bound and
        # slows every execution down. The first 512 KiB holds the boot
        # sector, both FATs, the root directory and the start of the data
        # area, which is everything the parser walks.
        trimmed = data[:512 * 1024]
        n = write(corpus, "real-fs.img", trimmed)
        out.append(f"real-fs.img ({n} bytes, the kernel's own filesystem, trimmed)")

    # A hand-built boot sector that mounts, so a mutation of any BPB field
    # starts from a filesystem the driver accepts.
    sector = bytearray(512)
    sector[0:3] = b"\xeb\x3c\x90"
    sector[3:11] = b"STRATUM "
    struct.pack_into("<H", sector, 11, 512)     # bytes per sector
    sector[13] = 4                              # sectors per cluster
    struct.pack_into("<H", sector, 14, 1)       # reserved
    sector[16] = 2                               # FATs
    struct.pack_into("<H", sector, 17, 512)     # root entries
    struct.pack_into("<H", sector, 19, 32768)   # total sectors
    sector[21] = 0xF8
    struct.pack_into("<H", sector, 22, 32)      # sectors per FAT
    sector[54:62] = b"FAT16   "
    sector[510:512] = b"\x55\xaa"

    # 512 KiB is enough for the metadata the BPB describes.
    img = bytearray(512 * 1024)
    img[0:512] = sector
    # Entry 0 is the media byte, entry 1 the end-of-chain marker.
    struct.pack_into("<HH", img, 512, 0xFFF8, 0xFFFF)
    out.append(f"minimal.img ({write(corpus, 'minimal.img', bytes(img))} bytes)")

    # Each of these is a mount rejection with its own message.
    for name, patch in [
        ("sector-size.img", (11, struct.pack("<H", 1024))),
        ("zero-spc.img", (13, b"\x00")),
        ("odd-spc.img", (13, b"\x03")),
        ("zero-reserved.img", (14, struct.pack("<H", 0))),
        ("zero-fats.img", (16, b"\x00")),
        ("fat32-root.img", (17, struct.pack("<H", 0))),
        ("zero-spf.img", (22, struct.pack("<H", 0))),
        ("huge-total.img", (19, struct.pack("<H", 0xFFFF))),
        ("fat12-clusters.img", (19, struct.pack("<H", 200))),
        ("short-fat.img", (22, struct.pack("<H", 1))),
    ]:
        variant = bytearray(img)
        off, value = patch
        variant[off:off + len(value)] = value
        write(corpus, name, bytes(variant))
        out.append(name)

    return out


# ---- the heap's opcode stream ---------------------------------------------

def seed_heap(corpus: Path) -> list[str]:
    out = []

    # The sequences a hand-written stress test would use, as opcode pairs.
    # They are the starting points; the fuzzer finds the awkward ones.
    programs = {
        "alloc-free.bin": bytes([0x00, 0x08, 0x02, 0x00]),
        "fill-then-free.bin": bytes(
            b for i in range(32) for b in (i << 3, 0x10)) + bytes(
            b for i in range(32) for b in ((i << 3) | 2, 0x00)),
        "coalesce.bin": bytes([0x00, 0x20, 0x08, 0x20, 0x10, 0x20,
                               0x0a, 0x00, 0x02, 0x00, 0x12, 0x00]),
        "realloc-grow.bin": bytes([0x00, 0x04, 0x03, 0x40, 0x03, 0x02]),
        "aligned.bin": bytes([0x04, 0x07, 0x0c, 0x0f, 0x14, 0x17]),
        "degenerate.bin": bytes([0x05, 0x00, 0x05, 0x01, 0x07, 0x00]),
        "check-often.bin": bytes([0x00, 0x30, 0x07, 0x00, 0x06, 0x00,
                                  0x02, 0x00, 0x07, 0x00]),
    }

    for name, data in programs.items():
        write(corpus, name, data)
        out.append(name)

    return out


# ---- ACPI -----------------------------------------------------------------

def sdt(signature: bytes, payload: bytes, revision: int = 1) -> bytes:
    """A table with a correct checksum, because the checksum is arithmetic and
    there is nothing behind it worth making the fuzzer find."""
    length = 36 + len(payload)
    header = bytearray(
        signature[:4].ljust(4, b" ") + struct.pack("<I", length) +
        bytes([revision, 0]) + b"FUZZ  " + b"FUZZTBL " +
        struct.pack("<III", 1, 0x5A5A5A5A, 1))
    table = bytes(header) + payload
    checksum = (-sum(table)) & 0xFF
    out = bytearray(table)
    out[9] = checksum
    return bytes(out)


def seed_acpi(corpus: Path) -> list[str]:
    out = []

    # The target plants an RSDP and takes the RSDT pointer from the first
    # bytes of the input, so a seed is: four control bytes, then the tables at
    # the offset the target copies them to (0x10000).
    def build(madt_entries: bytes, extra_tables: int = 0) -> bytes:
        rsdt_at = 0x10000
        madt_at = rsdt_at + 0x200

        madt = sdt(b"APIC",
                   struct.pack("<II", 0xFEE00000, 1) + madt_entries)

        pointers = [madt_at] + [madt_at] * extra_tables
        rsdt = sdt(b"RSDT", b"".join(struct.pack("<I", p) for p in pointers))

        blob = bytearray(0x400)
        # The control bytes the target reads: revision, then the RSDT and
        # XSDT addresses.
        blob[0] = 0
        struct.pack_into("<I", blob, 1, rsdt_at)
        struct.pack_into("<I", blob, 5, 0)
        struct.pack_into("<Q", blob, 9, 0)

        # The target copies the input to 0x10000, so offsets inside the input
        # are offsets from there.
        body = bytearray(0x400)
        body[0:len(rsdt)] = rsdt
        pad = 0x200 - len(rsdt)
        if pad > 0:
            body += b"\0" * 0
        full = bytearray(blob[:0x20])
        full += b"\0" * (0x200 - len(full))
        # Lay the RSDT at the start of the copied region and the MADT 0x200
        # further on, matching the pointers above.
        region = bytearray(0x400)
        region[0:len(rsdt)] = rsdt
        region[0x200:0x200 + len(madt)] = madt

        seed = bytearray(0x20)
        seed[0] = 0
        struct.pack_into("<I", seed, 1, rsdt_at)
        struct.pack_into("<I", seed, 5, 0)
        struct.pack_into("<Q", seed, 9, 0)
        # Everything after the control bytes is the region, and the target
        # copies the whole input to 0x10000 - so the control bytes are part of
        # the region too. Place the tables past them.
        final = bytearray(region)
        final[0:0x20] = seed
        # The RSDT has to survive having its first 0x20 bytes overwritten, so
        # move it clear of them.
        final = bytearray(0x400)
        final[0:0x20] = seed
        struct.pack_into("<I", final, 1, rsdt_at + 0x100)
        final[0x100:0x100 + len(rsdt)] = rsdt
        final[0x200:0x200 + len(madt)] = madt
        return bytes(final)

    # One local APIC entry per processor: the shape a four-core machine has.
    four_cpus = b"".join(
        struct.pack("<BBBBI", 0, 8, i, i, 1) for i in range(4))
    out.append(f"four-cpus.bin ({write(corpus, 'four-cpus.bin', build(four_cpus))} bytes)")

    # An I/O APIC and an interrupt source override, which is what QEMU
    # publishes and what the parser's other two branches handle.
    io_and_iso = (four_cpus +
                  struct.pack("<BBBBII", 1, 12, 0, 0, 0xFEC00000, 0) +
                  struct.pack("<BBBBIH", 2, 10, 0, 0, 2, 0))
    write(corpus, "io-apic.bin", build(io_and_iso))
    out.append("io-apic.bin")

    # A zero-length MADT entry, which would loop forever if the parser did
    # not bound it. Seeding the case means the fuzzer starts from a proof the
    # bound exists rather than hunting for it.
    write(corpus, "zero-length-entry.bin", build(b"\x00\x00\x00\x00"))
    out.append("zero-length-entry.bin")

    # An entry whose length runs past the end of the table.
    write(corpus, "overlong-entry.bin", build(b"\x00\xff\x00\x00"))
    out.append("overlong-entry.bin")

    # A local APIC address override, the one entry type that carries a 64-bit
    # address a 32-bit kernel cannot use.
    write(corpus, "lapic-override.bin",
          build(four_cpus + struct.pack("<BBHQ", 5, 12, 0,
                                        0x1_0000_0000)))
    out.append("lapic-override.bin")

    return out


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", required=True, type=Path)
    ap.add_argument("--user-elf", type=Path)
    ap.add_argument("--fs-image", type=Path)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    written = {
        "elf": seed_elf(args.corpus / "elf", args.user_elf),
        "fat": seed_fat(args.corpus / "fat", args.fs_image),
        "heap": seed_heap(args.corpus / "heap"),
        "acpi": seed_acpi(args.corpus / "acpi"),
    }

    total = sum(len(v) for v in written.values())

    if not args.quiet:
        print(f"  SEED    {total} corpus input(s) under {args.corpus}")
        for target, names in written.items():
            print(f"            {target:<5} {len(names):>3}: "
                  f"{', '.join(n.split(' ')[0] for n in names[:4])}"
                  f"{', ...' if len(names) > 4 else ''}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
