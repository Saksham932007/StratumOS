#!/usr/bin/env python3
"""Build a bootable StratumOS disk image.

Layout
------
    LBA 0          stage 1 (the MBR, exactly 512 bytes)
    LBA 1-24       stage 2 (exactly 24 sectors, padded by the assembler)
    LBA 25+        the kernel ELF, verbatim

Stage 2's header holds the kernel's starting LBA and its length in sectors.
Those two fields are patched here rather than assembled in, because the
assembler has no idea how big the kernel will turn out to be.

The image is then padded out to a whole number of 1 MiB units, which keeps
QEMU and real BIOSes from complaining about a disk whose size is not a
multiple of its reported geometry.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

SECTOR = 512
STAGE1_SECTORS = 1
STAGE2_SECTORS = 24
KERNEL_LBA = STAGE1_SECTORS + STAGE2_SECTORS

# Where stage 2 stages the kernel before entering protected mode. The ELF has
# to fit between there and the top of conventional memory, or the read runs
# into the EBDA.
KERNEL_STAGE_ADDR = 0x20000
CONVENTIONAL_TOP = 0x9FC00

STAGE2_MAGIC = b"S2OS"
# Offsets within the stage 2 header; see boot/stage2.asm.
OFF_MAGIC = 0
OFF_KERNEL_LBA = 4
OFF_KERNEL_SECTORS = 8
OFF_CMDLINE = 16
CMDLINE_MAX = 96


def fail(message: str) -> "NoReturn":  # type: ignore[valid-type]
    print(f"mkimage: error: {message}", file=sys.stderr)
    raise SystemExit(1)


def sectors_for(size: int) -> int:
    return (size + SECTOR - 1) // SECTOR


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stage1", required=True, type=Path)
    ap.add_argument("--stage2", required=True, type=Path)
    ap.add_argument("--kernel", required=True, type=Path,
                    help="kernel ELF, loaded and relocated by stage 2")
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--cmdline", default=None,
                    help="kernel command line to embed in the stage 2 header "
                         f"(max {CMDLINE_MAX - 1} characters)")
    ap.add_argument("--pad-to-mib", type=int, default=2,
                    help="pad the image to a multiple of this many MiB")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    for path in (args.stage1, args.stage2, args.kernel):
        if not path.is_file():
            fail(f"{path} does not exist")

    stage1 = args.stage1.read_bytes()
    stage2 = bytearray(args.stage2.read_bytes())
    kernel = args.kernel.read_bytes()

    # --- validate stage 1 -------------------------------------------------
    if len(stage1) != SECTOR:
        fail(f"stage1 is {len(stage1)} bytes; it must be exactly {SECTOR}")
    if stage1[510:512] != b"\x55\xaa":
        fail("stage1 does not end with the 0xAA55 boot signature")

    # --- validate stage 2 -------------------------------------------------
    expected = STAGE2_SECTORS * SECTOR
    if len(stage2) != expected:
        fail(f"stage2 is {len(stage2)} bytes; it must be exactly {expected} "
             f"({STAGE2_SECTORS} sectors). Check the padding in stage2.asm.")
    if bytes(stage2[OFF_MAGIC:OFF_MAGIC + 4]) != STAGE2_MAGIC:
        fail(f"stage2 does not start with {STAGE2_MAGIC!r}")

    # --- validate the kernel ---------------------------------------------
    if kernel[:4] != b"\x7fELF":
        fail("the kernel is not an ELF file")
    if kernel[4] != 1:
        fail("the kernel is not ELFCLASS32")
    if kernel[5] != 1:
        fail("the kernel is not little-endian")
    e_type, e_machine = struct.unpack_from("<HH", kernel, 16)
    if e_type != 2:
        fail(f"the kernel has e_type {e_type}; ET_EXEC (2) is required")
    if e_machine != 3:
        fail(f"the kernel has e_machine {e_machine}; EM_386 (3) is required")

    kernel_sectors = sectors_for(len(kernel))

    # Stage 2 reads the whole ELF into low memory before it can parse the
    # program headers, so the file itself must fit below the EBDA.
    staged_end = KERNEL_STAGE_ADDR + kernel_sectors * SECTOR
    if staged_end > CONVENTIONAL_TOP:
        fail(f"the kernel is {len(kernel) // 1024} KiB, which would stage to "
             f"{staged_end:#x} and overrun conventional memory at "
             f"{CONVENTIONAL_TOP:#x}. Shrink the kernel, or teach stage 2 to "
             f"stage it in several passes.")

    # --- patch stage 2's header ------------------------------------------
    struct.pack_into("<I", stage2, OFF_KERNEL_LBA, KERNEL_LBA)
    struct.pack_into("<I", stage2, OFF_KERNEL_SECTORS, kernel_sectors)

    if args.cmdline is not None:
        encoded = args.cmdline.encode("ascii", errors="strict")
        if len(encoded) >= CMDLINE_MAX:
            fail(f"the command line is {len(encoded)} bytes; the stage 2 "
                 f"header has room for {CMDLINE_MAX - 1} plus a terminator")
        stage2[OFF_CMDLINE:OFF_CMDLINE + CMDLINE_MAX] = \
            encoded.ljust(CMDLINE_MAX, b"\0")

    embedded = bytes(stage2[OFF_CMDLINE:OFF_CMDLINE + CMDLINE_MAX])
    embedded = embedded.split(b"\0", 1)[0].decode("ascii", errors="replace")

    # --- assemble ---------------------------------------------------------
    image = bytearray()
    image += stage1
    image += stage2
    image += kernel
    image += b"\x00" * (kernel_sectors * SECTOR - len(kernel))

    if args.pad_to_mib > 0:
        unit = args.pad_to_mib * 1024 * 1024
        if len(image) % unit:
            image += b"\x00" * (unit - (len(image) % unit))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)

    if not args.quiet:
        print(f"mkimage: {args.output}")
        print(f"  stage1  LBA 0          {len(stage1):>8} bytes")
        print(f"  stage2  LBA 1-{STAGE2_SECTORS:<10} {len(stage2):>8} bytes")
        print(f"  kernel  LBA {KERNEL_LBA}+        {len(kernel):>8} bytes "
              f"({kernel_sectors} sectors)")
        print(f"  staged at {KERNEL_STAGE_ADDR:#x}-{staged_end:#x}, "
              f"{(CONVENTIONAL_TOP - staged_end) // 1024} KiB of headroom")
        print(f"  cmdline \"{embedded}\"")
        print(f"  total   {len(image) // 1024} KiB")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
