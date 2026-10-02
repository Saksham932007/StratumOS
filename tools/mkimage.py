#!/usr/bin/env python3
"""Build a bootable StratumOS disk image.

Layout
------
    LBA 0          stage 1 (the MBR, exactly 512 bytes)
    LBA 1-24       stage 2 (exactly 24 sectors, padded by the assembler)
    LBA 25+        the kernel ELF, verbatim
    (aligned)      an optional FAT16 filesystem, described by a real MBR
                   partition entry in sector 0

Stage 2's header holds the kernel's starting LBA and its length in sectors.
Those two fields are patched here rather than assembled in, because the
assembler has no idea how big the kernel will turn out to be.

The partition table
-------------------
Sector 0 is both the boot sector and the partition table: the four 16-byte
entries live at offset 446, immediately after the boot code. Stage 1 uses 315
of those 446 bytes, so there is room for a real table rather than a
convention, and the kernel's block layer parses it the way it would parse any
disk's.

Only the LBA fields are filled in. The CHS fields exist for BIOSes older than
INT 13h extensions, cannot describe anything past 8 GiB, and are routinely
wrong on real disks - a table where CHS and LBA disagree is normal, and LBA
is the one anything modern believes. They are written as the "use LBA
instead" sentinel rather than left zero, which is what partitioning tools do.

The kernel and the filesystem share one disk on purpose. A second drive would
have been less work and would have tested less: this way the partition table
has to be right, and the filesystem's reads have to be offset by the
partition's start, which is the bug a separate disk would hide.

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

# Where the filesystem partition starts. Aligned to 1 MiB, which is what
# every partitioning tool has done since about 2010: it keeps a partition's
# clusters aligned to the erase blocks of anything flash-based, and it is a
# round number in a hexdump.
PARTITION_ALIGN_SECTORS = 2048

MBR_PARTITION_OFFSET = 446
MBR_PARTITION_SIZE = 16
# "Use LBA, the CHS fields are meaningless" - the conventional filler.
CHS_USE_LBA = b"\xfe\xff\xff"
PART_TYPE_FAT16_LBA = 0x0E

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
    ap.add_argument("--fs", type=Path, default=None,
                    help="FAT16 filesystem image to append as partition 1")
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

    stage1 = bytearray(args.stage1.read_bytes())
    stage2 = bytearray(args.stage2.read_bytes())
    kernel = args.kernel.read_bytes()

    fs = args.fs.read_bytes() if args.fs else b""

    if args.fs and not args.fs.is_file():
        fail(f"{args.fs} does not exist")

    # --- validate stage 1 -------------------------------------------------
    if len(stage1) != SECTOR:
        fail(f"stage1 is {len(stage1)} bytes; it must be exactly {SECTOR}")
    if stage1[510:512] != b"\x55\xaa":
        fail("stage1 does not end with the 0xAA55 boot signature")

    # The partition table overlaps stage 1's code if stage 1 is too long. The
    # assembler cannot catch this, because as far as it is concerned the whole
    # 510 bytes are available.
    code_end = len(bytes(stage1[:MBR_PARTITION_OFFSET]).rstrip(b"\0"))
    if code_end > MBR_PARTITION_OFFSET:
        fail(f"stage1's code reaches byte {code_end}; the partition table "
             f"starts at {MBR_PARTITION_OFFSET}")

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

    # --- the partition table ---------------------------------------------
    kernel_end_sector = KERNEL_LBA + kernel_sectors
    fs_lba = 0
    fs_sectors = 0

    if fs:
        if len(fs) % SECTOR:
            fail(f"the filesystem image is {len(fs)} bytes, not a whole "
                 f"number of {SECTOR}-byte sectors")

        fs_sectors = len(fs) // SECTOR
        fs_lba = ((kernel_end_sector + PARTITION_ALIGN_SECTORS - 1) //
                  PARTITION_ALIGN_SECTORS) * PARTITION_ALIGN_SECTORS

        if fs[510:512] != b"\x55\xaa":
            fail("the filesystem image has no 0xAA55 signature in its boot "
                 "sector; mkfat.py writes one")
        if fs[54:62] != b"FAT16   ":
            fail("the filesystem image does not say FAT16 at offset 54")

        entry = struct.pack(
            "<B3sB3sII",
            0x00,                   # not bootable; the MBR boots the kernel
            CHS_USE_LBA,
            PART_TYPE_FAT16_LBA,
            CHS_USE_LBA,
            fs_lba,
            fs_sectors,
        )
        assert len(entry) == MBR_PARTITION_SIZE

        stage1[MBR_PARTITION_OFFSET:
               MBR_PARTITION_OFFSET + MBR_PARTITION_SIZE] = entry

    # --- assemble ---------------------------------------------------------
    image = bytearray()
    image += stage1
    image += stage2
    image += kernel
    image += b"\x00" * (kernel_sectors * SECTOR - len(kernel))

    if fs:
        image += b"\x00" * (fs_lba * SECTOR - len(image))
        image += fs

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
        if fs:
            print(f"  fat16   LBA {fs_lba}+     {len(fs):>8} bytes "
                  f"({fs_sectors} sectors), partition 1, type "
                  f"{PART_TYPE_FAT16_LBA:#02x}")
        print(f"  cmdline \"{embedded}\"")
        print(f"  total   {len(image) // 1024} KiB")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
