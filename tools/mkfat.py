#!/usr/bin/env python3
"""Build a FAT16 filesystem image from a directory tree.

Why this is written here rather than driven through mtools
----------------------------------------------------------
Two reasons, and the second is the real one.

First, dependencies: `mformat`/`mcopy` are one more thing that has to be
installed before the project builds, and the kernel's own test suite asserts
on specific bytes in specific clusters. A formatter whose layout can drift
between versions is a poor foundation for that.

Second, writing it is the point. The kernel's FAT driver has to parse a BPB,
walk a FAT, follow a cluster chain and decode 8.3 names; writing the producer
means every one of those fields is chosen here on purpose rather than
accepted from a tool. When the driver and the formatter disagree, one of them
is wrong and both are readable.

Layout produced
---------------
    sector 0                boot sector: the BPB, plus 0xAA55 so that
                            anything probing the partition recognises it
    reserved               1 sector (just the boot sector)
    FAT 1                  SECTORS_PER_FAT sectors
    FAT 2                  SECTORS_PER_FAT sectors, identical
    root directory         ROOT_ENTRIES * 32 bytes, rounded to a sector
    data area              clusters 2..N, SECTORS_PER_CLUSTER each

FAT16 rather than FAT12 or FAT32: FAT12's 12-bit entries straddle byte
boundaries, which is a fiddly decoder for no benefit, and FAT32 moves the
root directory into the data area and adds an FSInfo sector. FAT16's cluster
count has to land between 4085 and 65524, which the image size below
guarantees.

Names are 8.3 and uppercase, because long filename support is a separate
feature (a chain of pseudo-entries with a checksum over the short name) and
not one this kernel needs in order to find /bin/init.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

SECTOR = 512
SECTORS_PER_CLUSTER = 4          # 2 KiB clusters
RESERVED_SECTORS = 1
NUM_FATS = 2
ROOT_ENTRIES = 512               # 32 sectors of root directory
MEDIA_BYTE = 0xF8                # "fixed disk", the conventional value

# Cluster values with a meaning of their own.
FAT_FREE = 0x0000
FAT_BAD = 0xFFF7
FAT_EOC = 0xFFFF                 # end of chain

# Directory entry attributes.
ATTR_READ_ONLY = 0x01
ATTR_HIDDEN = 0x02
ATTR_SYSTEM = 0x04
ATTR_VOLUME_ID = 0x08
ATTR_DIRECTORY = 0x10
ATTR_ARCHIVE = 0x20

DIR_ENTRY_SIZE = 32


def fail(message: str) -> "NoReturn":  # type: ignore[valid-type]
    print(f"mkfat: error: {message}", file=sys.stderr)
    raise SystemExit(1)


def short_name(name: str) -> bytes:
    """Encode one path component as an 8.3 name, padded with spaces.

    Rejects rather than mangles. A formatter that silently truncated
    `initialise` to `INITIAL` would produce an image whose contents do not
    match the tree it was built from, and the resulting bug would look like a
    driver bug.
    """
    name = name.upper()

    if name in (".", ".."):
        return name.ljust(11, " ").encode("ascii")

    stem, dot, ext = name.partition(".")

    if dot and "." in ext:
        fail(f"'{name}' has more than one dot; 8.3 names allow one")
    if len(stem) > 8:
        fail(f"'{name}': the stem '{stem}' is longer than 8 characters")
    if len(ext) > 3:
        fail(f"'{name}': the extension '{ext}' is longer than 3 characters")
    if not stem:
        fail(f"'{name}' has no stem")

    for ch in stem + ext:
        if ch in '"*+,./:;<=>?[\\]|' or ord(ch) < 0x20:
            fail(f"'{name}' contains {ch!r}, which is not legal in an 8.3 name")

    return (stem.ljust(8, " ") + ext.ljust(3, " ")).encode("ascii")


class Fat16Builder:
    def __init__(self, total_sectors: int, label: str) -> None:
        if total_sectors > 0xFFFF:
            # The BPB's 16-bit total_sectors field; the 32-bit one exists but
            # a filesystem this small has no use for it.
            fail(f"{total_sectors} sectors does not fit the 16-bit BPB field")

        self.total_sectors = total_sectors
        self.label = label

        # The number of FAT sectors depends on the cluster count, which
        # depends on the number of FAT sectors. Solve it by iterating: start
        # with none and grow until it stops changing. Three rounds is always
        # enough; the loop is bounded so a mistake here cannot hang a build.
        sectors_per_fat = 1
        for _ in range(8):
            overhead = (RESERVED_SECTORS + NUM_FATS * sectors_per_fat +
                        (ROOT_ENTRIES * DIR_ENTRY_SIZE + SECTOR - 1) // SECTOR)
            data_sectors = total_sectors - overhead
            if data_sectors <= 0:
                fail("the image is too small to hold its own metadata")
            clusters = data_sectors // SECTORS_PER_CLUSTER
            # +2 because clusters are numbered from 2, so entries 0 and 1 are
            # reserved and still take space in the table.
            need = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
            if need == sectors_per_fat:
                break
            sectors_per_fat = need
        else:
            fail("the FAT size did not converge")

        self.sectors_per_fat = sectors_per_fat
        self.root_sectors = (ROOT_ENTRIES * DIR_ENTRY_SIZE + SECTOR - 1) // SECTOR
        self.fat_start = RESERVED_SECTORS
        self.root_start = RESERVED_SECTORS + NUM_FATS * sectors_per_fat
        self.data_start = self.root_start + self.root_sectors
        self.cluster_count = (total_sectors - self.data_start) // SECTORS_PER_CLUSTER

        if not 4085 <= self.cluster_count <= 65524:
            fail(f"{self.cluster_count} clusters is outside FAT16's range "
                 f"(4085-65524); adjust the image size or cluster size")

        # Entry 0 holds the media byte, entry 1 is the end-of-chain marker.
        self.fat = [FAT_FREE] * (self.cluster_count + 2)
        self.fat[0] = 0xFF00 | MEDIA_BYTE
        self.fat[1] = FAT_EOC

        self.data = bytearray(self.cluster_count * SECTORS_PER_CLUSTER * SECTOR)
        self.next_free = 2
        self.files = 0
        self.dirs = 0

    # ---- allocation ----------------------------------------------------

    def alloc_chain(self, length: int) -> list[int]:
        """Allocate `length` clusters and link them into a chain."""
        if length == 0:
            return []

        if self.next_free + length > self.cluster_count + 2:
            fail(f"out of space: need {length} more clusters, "
                 f"{self.cluster_count + 2 - self.next_free} left")

        chain = list(range(self.next_free, self.next_free + length))
        self.next_free += length

        for a, b in zip(chain, chain[1:]):
            self.fat[a] = b
        self.fat[chain[-1]] = FAT_EOC

        return chain

    def write_clusters(self, chain: list[int], payload: bytes) -> None:
        size = SECTORS_PER_CLUSTER * SECTOR
        for i, cluster in enumerate(chain):
            offset = (cluster - 2) * size
            piece = payload[i * size:(i + 1) * size]
            self.data[offset:offset + len(piece)] = piece

    # ---- directories ---------------------------------------------------

    def dir_entry(self, name: bytes, attr: int, cluster: int,
                  size: int) -> bytes:
        """One 32-byte directory entry.

        The timestamps are fixed rather than taken from the host clock, so
        that the same tree always produces the same image. A build whose
        output changes when nothing changed makes it impossible to tell
        whether a test is asserting on content or on a date.
        """
        return struct.pack(
            "<11sBBBHHHHHHHI",
            name,
            attr,
            0,          # reserved / VFAT case flags
            0,          # creation time, tenths of a second
            0,          # creation time
            0x2821,     # creation date: 2000-01-01
            0x2821,     # last access date
            0,          # high 16 bits of the cluster (FAT32 only)
            0,          # write time
            0x2821,     # write date
            cluster,
            size,
        )

    def build_directory(self, tree: dict, self_cluster: int,
                        parent_cluster: int) -> bytes:
        """Serialise one directory. Subdirectories are built recursively,
        depth first, so that a child's starting cluster is known before its
        parent's entry is written."""
        entries = bytearray()

        if self_cluster == 0 and self.label:
            # The volume label is a root-directory entry as well as a BPB
            # field. The BPB copy is advisory - tools read this one - so an
            # image with only the BPB field reads back as unlabelled.
            entries += self.dir_entry(
                self.label.upper().ljust(11)[:11].encode("ascii"),
                ATTR_VOLUME_ID, 0, 0)

        if self_cluster != 0:  # not the root; the root has no . or ..
            entries += self.dir_entry(short_name("."), ATTR_DIRECTORY,
                                      self_cluster, 0)
            entries += self.dir_entry(short_name(".."), ATTR_DIRECTORY,
                                      parent_cluster, 0)

        for name in sorted(tree):
            value = tree[name]

            if isinstance(value, dict):
                # A directory needs at least one cluster even when empty,
                # because . and .. have to live somewhere.
                chain = self.alloc_chain(1)
                payload = self.build_directory(value, chain[0], self_cluster)
                while len(payload) > SECTORS_PER_CLUSTER * SECTOR * len(chain):
                    chain += self.alloc_chain(1)
                    self.fat[chain[-2]] = chain[-1]
                    self.fat[chain[-1]] = FAT_EOC
                self.write_clusters(chain, payload)
                entries += self.dir_entry(short_name(name), ATTR_DIRECTORY,
                                          chain[0], 0)
                self.dirs += 1
            else:
                data: bytes = value
                need = (len(data) + SECTORS_PER_CLUSTER * SECTOR - 1) // \
                    (SECTORS_PER_CLUSTER * SECTOR)
                chain = self.alloc_chain(need)
                self.write_clusters(chain, data)
                entries += self.dir_entry(short_name(name), ATTR_ARCHIVE,
                                          chain[0] if chain else 0, len(data))
                self.files += 1

        return bytes(entries)

    # ---- the image -----------------------------------------------------

    def boot_sector(self) -> bytes:
        bs = bytearray(SECTOR)

        # A jump instruction, because a real boot sector starts with one and
        # some tools refuse an image that does not. This one jumps to a `hlt`
        # loop: the partition is data, not something to boot.
        bs[0:3] = b"\xeb\x3c\x90"
        bs[3:11] = b"STRATUM "                       # OEM name, 8 bytes

        struct.pack_into("<H", bs, 11, SECTOR)                 # bytes/sector
        bs[13] = SECTORS_PER_CLUSTER
        struct.pack_into("<H", bs, 14, RESERVED_SECTORS)
        bs[16] = NUM_FATS
        struct.pack_into("<H", bs, 17, ROOT_ENTRIES)
        struct.pack_into("<H", bs, 19, self.total_sectors)
        bs[21] = MEDIA_BYTE
        struct.pack_into("<H", bs, 22, self.sectors_per_fat)
        struct.pack_into("<H", bs, 24, 32)                     # sectors/track
        struct.pack_into("<H", bs, 26, 2)                      # heads
        struct.pack_into("<I", bs, 28, 0)                      # hidden sectors
        struct.pack_into("<I", bs, 32, 0)                      # total_sectors_32

        bs[36] = 0x80                                          # drive number
        bs[37] = 0                                             # reserved
        bs[38] = 0x29                                          # extended sig
        struct.pack_into("<I", bs, 39, 0x53545241)             # volume id
        bs[43:54] = self.label.upper().ljust(11)[:11].encode("ascii")
        bs[54:62] = b"FAT16   "

        bs[62:64] = b"\xf4\xeb"                                # hlt; jmp -2
        bs[510:512] = b"\x55\xaa"

        return bytes(bs)

    def finish(self, tree: dict) -> bytes:
        root = self.build_directory(tree, 0, 0)

        root_capacity = ROOT_ENTRIES * DIR_ENTRY_SIZE
        if len(root) > root_capacity:
            fail(f"the root directory needs {len(root)} bytes; FAT16's "
                 f"fixed root holds {root_capacity}")

        fat_bytes = bytearray()
        for entry in self.fat:
            fat_bytes += struct.pack("<H", entry)
        fat_bytes += b"\x00" * (self.sectors_per_fat * SECTOR - len(fat_bytes))

        image = bytearray()
        image += self.boot_sector()
        for _ in range(NUM_FATS):
            image += fat_bytes
        image += root.ljust(self.root_sectors * SECTOR, b"\x00")
        image += self.data
        image += b"\x00" * (self.total_sectors * SECTOR - len(image))

        assert len(image) == self.total_sectors * SECTOR
        return bytes(image)


def collect(root: Path) -> dict:
    """Read a directory tree into nested dicts of bytes."""
    tree: dict = {}

    for child in sorted(root.iterdir()):
        if child.name.startswith("."):
            continue
        if child.is_dir():
            tree[child.name] = collect(child)
        elif child.is_file():
            tree[child.name] = child.read_bytes()

    return tree


def describe(tree: dict, prefix: str = "") -> list[str]:
    out = []
    for name in sorted(tree):
        value = tree[name]
        if isinstance(value, dict):
            out.append(f"  {prefix}{name}/")
            out += describe(value, prefix + name + "/")
        else:
            out.append(f"  {prefix}{name:<24} {len(value):>8} bytes")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", required=True, type=Path,
                    help="directory whose contents become the filesystem")
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--size-kib", type=int, default=16384,
                    help="image size in KiB (default 16384, i.e. 16 MiB)")
    ap.add_argument("--label", default="STRATUM")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if not args.root.is_dir():
        fail(f"{args.root} is not a directory")

    total_sectors = args.size_kib * 1024 // SECTOR
    builder = Fat16Builder(total_sectors, args.label)
    tree = collect(args.root)
    image = builder.finish(tree)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)

    if not args.quiet:
        print(f"mkfat: {args.output}")
        print(f"  {args.size_kib} KiB, {total_sectors} sectors, FAT16, "
              f"label {args.label.upper()}")
        print(f"  {SECTORS_PER_CLUSTER * SECTOR // 1024} KiB clusters, "
              f"{builder.cluster_count} of them, "
              f"{builder.next_free - 2} used")
        print(f"  FAT {builder.fat_start}+{builder.sectors_per_fat} x "
              f"{NUM_FATS}, root {builder.root_start}+"
              f"{builder.root_sectors}, data {builder.data_start}+")
        print(f"  {builder.files} file(s), {builder.dirs} director(y|ies):")
        for line in describe(tree):
            print(line)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
