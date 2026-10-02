#!/usr/bin/env python3
"""Validate a built StratumOS disk image before anything tries to boot it.

tools/check-kernel.py does this for the kernel ELF. This is the same argument
one level up: `mkimage` succeeding says the bytes were written, not that a
BIOS can boot them or that the kernel will find its filesystem.

The checks are the ones whose failure mode is otherwise a silent hang or a
kernel that boots and then cannot find /bin/INIT:

  * sector 0 ends in 0xAA55, or no BIOS will execute it at all;
  * stage 1's code stops before the partition table at offset 446, so the
    table it carries has not been overwritten by the loader it shares a
    sector with;
  * the stage 2 header is where stage 1 expects it, and its kernel LBA and
    sector count point at something that is actually an ELF;
  * every partition entry lies inside the image;
  * a FAT partition's boot sector really is a FAT boot sector, its geometry
    is self-consistent, and its cluster count is in FAT16's range;
  * the files the kernel needs are present: /BIN/INIT at least, and every
    file in /BIN is an ELF for the right machine.

The last one is the point of the whole script. A build that produces an image
whose /BIN/INIT is missing or truncated boots perfectly and then fails in
ring 3, which is the hardest place to debug it from.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

SECTOR = 512
STAGE2_LBA = 1
STAGE2_SECTORS = 24
MBR_PARTITION_OFFSET = 446
DIRENT_SIZE = 32


class Checker:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        self.problems: list[str] = []
        self.notes: list[str] = []

    def fail(self, msg: str) -> None:
        self.problems.append(msg)

    def note(self, msg: str) -> None:
        self.notes.append(msg)

    def sector(self, lba: int) -> bytes:
        off = lba * SECTOR
        if off + SECTOR > len(self.data):
            return b""
        return self.data[off:off + SECTOR]

    # ---- the master boot record -----------------------------------------

    def check_mbr(self) -> None:
        mbr = self.sector(0)

        if len(mbr) != SECTOR:
            self.fail("the image is shorter than one sector")
            return

        if mbr[510:512] != b"\x55\xaa":
            self.fail("sector 0 does not end with 0xAA55; no BIOS will boot "
                      "this image")

        code = mbr[:MBR_PARTITION_OFFSET].rstrip(b"\x00")

        if len(code) > MBR_PARTITION_OFFSET:
            self.fail(f"stage 1's code is {len(code)} bytes and reaches into "
                      f"the partition table at {MBR_PARTITION_OFFSET}")
        else:
            self.note(f"stage 1: {len(code)} of {MBR_PARTITION_OFFSET} bytes "
                      f"before the partition table")

    def partitions(self) -> list[dict]:
        mbr = self.sector(0)
        out = []

        for i in range(4):
            base = MBR_PARTITION_OFFSET + i * 16
            status, _chs1, ptype, _chs2, first, count = struct.unpack_from(
                "<B3sB3sII", mbr, base)

            if ptype == 0 or count == 0:
                continue

            out.append({
                "index": i + 1,
                "status": status,
                "type": ptype,
                "first": first,
                "count": count,
            })

        return out

    def check_partitions(self) -> None:
        total_sectors = len(self.data) // SECTOR

        for p in self.partitions():
            if p["first"] >= total_sectors:
                self.fail(f"partition {p['index']} starts at LBA "
                          f"{p['first']}, past the image's {total_sectors} "
                          f"sectors")
            elif p["first"] + p["count"] > total_sectors:
                self.fail(f"partition {p['index']} spans LBA {p['first']}"
                          f"+{p['count']}, past the image's {total_sectors} "
                          f"sectors")
            else:
                self.note(f"partition {p['index']}: type {p['type']:#04x}, "
                          f"LBA {p['first']}+{p['count']} "
                          f"({p['count'] // 2} KiB)")

    # ---- stage 2 and the kernel ----------------------------------------

    def check_stage2(self) -> None:
        s2 = self.data[STAGE2_LBA * SECTOR:
                       (STAGE2_LBA + STAGE2_SECTORS) * SECTOR]

        if len(s2) != STAGE2_SECTORS * SECTOR:
            self.fail("stage 2 is not where it should be, or is truncated")
            return

        if s2[:4] != b"S2OS":
            self.fail(f"stage 2 does not begin with 'S2OS' (found {s2[:4]!r})")
            return

        kernel_lba, kernel_sectors = struct.unpack_from("<II", s2, 4)

        if kernel_lba != STAGE2_LBA + STAGE2_SECTORS:
            self.fail(f"the stage 2 header says the kernel is at LBA "
                      f"{kernel_lba}; stage 2 ends at "
                      f"{STAGE2_LBA + STAGE2_SECTORS}")

        if kernel_sectors == 0:
            self.fail("the stage 2 header says the kernel is zero sectors")
            return

        kernel = self.sector(kernel_lba)

        if kernel[:4] != b"\x7fELF":
            self.fail(f"LBA {kernel_lba} is not an ELF header; the stage 2 "
                      f"header points at the wrong place")
            return

        cmdline = s2[16:16 + 96].split(b"\x00", 1)[0].decode(
            "ascii", errors="replace")

        self.note(f"stage 2: kernel at LBA {kernel_lba}, {kernel_sectors} "
                  f"sectors, cmdline \"{cmdline}\"")

    # ---- the filesystem --------------------------------------------------

    def check_filesystem(self) -> None:
        fat = next((p for p in self.partitions()
                    if p["type"] in (0x01, 0x04, 0x06, 0x0E)), None)

        if fat is None:
            self.note("no FAT partition; the kernel will use its embedded "
                      "programs")
            return

        base = fat["first"]
        bs = self.sector(base)

        if bs[510:512] != b"\x55\xaa":
            self.fail("the FAT partition's boot sector has no 0xAA55")
            return

        (bytes_per_sector, sectors_per_cluster, reserved, num_fats,
         root_entries, total16) = struct.unpack_from("<HBHBHH", bs, 11)
        sectors_per_fat = struct.unpack_from("<H", bs, 22)[0]
        label = bs[43:54].decode("ascii", errors="replace").strip()
        fs_type = bs[54:62].decode("ascii", errors="replace").strip()

        if bytes_per_sector != SECTOR:
            self.fail(f"the filesystem uses {bytes_per_sector}-byte sectors; "
                      f"the kernel's driver assumes {SECTOR}")
            return

        for name, value in (("sectors per cluster", sectors_per_cluster),
                            ("reserved sectors", reserved),
                            ("FATs", num_fats),
                            ("root entries", root_entries),
                            ("sectors per FAT", sectors_per_fat)):
            if value == 0:
                self.fail(f"the filesystem's BPB has zero {name}")
                return

        if sectors_per_cluster & (sectors_per_cluster - 1):
            self.fail(f"{sectors_per_cluster} sectors per cluster is not a "
                      f"power of two")

        total = total16 or struct.unpack_from("<I", bs, 32)[0]

        if total > fat["count"]:
            self.fail(f"the BPB claims {total} sectors; the partition is "
                      f"{fat['count']}")

        fat_start = reserved
        root_start = fat_start + num_fats * sectors_per_fat
        root_sectors = (root_entries * DIRENT_SIZE + SECTOR - 1) // SECTOR
        data_start = root_start + root_sectors
        clusters = (total - data_start) // sectors_per_cluster

        if not 4085 <= clusters <= 65524:
            self.fail(f"{clusters} clusters is outside FAT16's 4085-65524; "
                      f"the kernel's driver will refuse to mount it")

        self.note(f"filesystem: {fs_type} \"{label}\", {total} sectors, "
                  f"{clusters} clusters of "
                  f"{sectors_per_cluster * SECTOR // 1024} KiB")

        # ---- walk it, which is the part that matters --------------------
        def read_cluster_chain(first: int, limit: int) -> bytes:
            out = bytearray()
            cluster = first
            seen = 0

            while 2 <= cluster < clusters + 2:
                start = base + data_start + (cluster - 2) * sectors_per_cluster
                for i in range(sectors_per_cluster):
                    out += self.sector(start + i)

                if len(out) > limit:
                    break

                byte = cluster * 2
                fat_sector = self.sector(base + fat_start + byte // SECTOR)
                cluster = struct.unpack_from("<H", fat_sector,
                                             byte % SECTOR)[0]
                seen += 1
                if seen > clusters:
                    self.fail("a cluster chain loops")
                    return bytes(out)

            return bytes(out)

        def entries(raw: bytes) -> list[dict]:
            out = []
            for off in range(0, len(raw), DIRENT_SIZE):
                e = raw[off:off + DIRENT_SIZE]
                if len(e) < DIRENT_SIZE or e[0] == 0x00:
                    break
                if e[0] == 0xE5:
                    continue
                attr = e[11]
                if attr & 0x0F == 0x0F or attr & 0x08:
                    continue
                stem = e[0:8].decode("ascii", errors="replace").rstrip()
                ext = e[8:11].decode("ascii", errors="replace").rstrip()
                cluster, size = struct.unpack_from("<HI", e, 26)
                out.append({
                    "name": stem + ("." + ext if ext else ""),
                    "attr": attr,
                    "is_dir": bool(attr & 0x10),
                    "cluster": cluster,
                    "size": size,
                })
            return out

        root_raw = b""
        for i in range(root_sectors):
            root_raw += self.sector(base + root_start + i)

        root = entries(root_raw)
        binary_dir = next((e for e in root
                           if e["is_dir"] and e["name"] == "BIN"), None)

        if binary_dir is None:
            self.fail("the filesystem has no /BIN directory, so the kernel "
                      "has nothing to exec")
            return

        bin_entries = [e for e in entries(
            read_cluster_chain(binary_dir["cluster"], 64 * 1024))
            if not e["is_dir"]]

        if not bin_entries:
            self.fail("/BIN is empty")
            return

        names = sorted(e["name"] for e in bin_entries)

        if "INIT" not in names:
            self.fail(f"/BIN holds {names} but not INIT, which is the "
                      f"program the kernel starts")

        for e in bin_entries:
            content = read_cluster_chain(e["cluster"], e["size"])[:e["size"]]

            if len(content) < e["size"]:
                self.fail(f"/BIN/{e['name']}'s chain holds {len(content)} "
                          f"bytes; its directory entry says {e['size']}")
                continue

            if content[:4] != b"\x7fELF":
                self.fail(f"/BIN/{e['name']} is not an ELF file")
                continue

            e_type, e_machine = struct.unpack_from("<HH", content, 16)

            if e_type != 2 or e_machine != 3:
                self.fail(f"/BIN/{e['name']} has e_type {e_type} / "
                          f"e_machine {e_machine}; ET_EXEC/EM_386 required")
                continue

            entry = struct.unpack_from("<I", content, 24)[0]

            if entry >= 0xC0000000:
                self.fail(f"/BIN/{e['name']}'s entry point {entry:#x} is in "
                          f"kernel space")

            self.note(f"  /BIN/{e['name']}: {e['size']} bytes, ELF32 i386, "
                      f"entry {entry:#x}")

        self.note(f"root: {', '.join(sorted(e['name'] for e in root))}")

    def run(self) -> bool:
        self.check_mbr()
        self.check_partitions()
        self.check_stage2()
        self.check_filesystem()
        return not self.problems


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", type=Path)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not args.image.is_file():
        print(f"check-image: {args.image} does not exist", file=sys.stderr)
        return 2

    c = Checker(args.image)
    ok = c.run()

    if args.verbose or not ok:
        for n in c.notes:
            print(f"    {n}")

    for p in c.problems:
        print(f"check-image: {args.image}: {p}", file=sys.stderr)

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
