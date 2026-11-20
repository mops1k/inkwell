#!/usr/bin/env python3
"""Build the microSD image the X3 QEMU runs mount (FAT16 + a minimal tree).

The firmware's Storage/SdFat path needs a real filesystem, not just a card that
answers CMD0/CMD58: on a blank image SdFat finds no volume, Storage.begin()
fails and the library index never starts. This script writes a standard
MBR-partitioned FAT16 volume with 2 KiB clusters (32768 clusters, safely inside
the FAT16 range) and fills it with a minimal tree:

    /books/test.txt
    /.crosspoint/            (the firmware's state/cache directory)

Pure Python (no mkfs.vfat/mtools on this box) so the image is reproducible:

  tools/qemu/make-sd-image.py --out build/qemu/sd.img                  # 64 MiB
  tools/qemu/make-sd-image.py --out build/qemu/sd.img --add book.epub:/books/book.epub
"""

from __future__ import annotations

import argparse
import math
import struct
import sys

SECTOR = 512
PART_LBA = 2048
ATTR_VOLUME_ID = 0x08
ATTR_DIR = 0x10
ATTR_ARCHIVE = 0x20
ATTR_LFN = 0x0F
SHORT_ALLOWED = set("$%'-_@~`!(){}^#&")


def short_name(name: str, is_dir: bool) -> tuple[str, bool]:
    """Return the 11-byte 8.3 name and whether an LFN entry is needed."""
    if not is_dir and "." in name:
        stem, ext = name.rsplit(".", 1)
    else:
        stem, ext = name, ""

    def clean(text: str, limit: int) -> str:
        return "".join(c for c in text.upper() if c.isalnum() or c in SHORT_ALLOWED)[:limit]

    stem_short = clean(stem, 8) or "X"
    ext_short = clean(ext, 3)
    # Anything that does not survive the 8.3 folding unchanged needs a long name.
    need_lfn = (stem_short != stem.upper() or ext_short != ext.upper()
                or any(c.islower() for c in name) or len(stem) > 8 or len(ext) > 3)
    if need_lfn and len(stem_short) > 6:
        stem_short = stem_short[:6] + "~1"
    return stem_short.ljust(8) + ext_short.ljust(3), need_lfn


def lfn_checksum(short: str) -> int:
    ck = 0
    for ch in short.encode("ascii"):
        ck = (((ck & 1) << 7) | (ck >> 1)) + ch
        ck &= 0xFF
    return ck


def lfn_entries(name: str, short: str) -> bytes:
    """The long-name entries of one file: last logical chunk first, as on disk."""
    units = [name[i:i + 1].encode("utf-16-le") for i in range(len(name))]
    units.append(b"\x00\x00")                  # terminator
    while len(units) % 13:
        units.append(b"\xff\xff")              # filler
    count = len(units) // 13
    ck = lfn_checksum(short)
    entries = []
    for index in range(count):
        chunk = units[index * 13:(index + 1) * 13]
        order = index + 1
        if index == count - 1:
            order |= 0x40
        entries.append(struct.pack("<B10sBBB12sH4s", order,
                                   b"".join(chunk[0:5]), ATTR_LFN, 0, ck,
                                   b"".join(chunk[5:11]), 0,
                                   b"".join(chunk[11:13])))
    entries.reverse()
    return b"".join(entries)


def direntry(short: str, attr: int, cluster: int = 0, size: int = 0) -> bytes:
    """One 32-byte directory entry (timestamps fixed at 1980-01-01 00:00)."""
    return struct.pack("<11sBBBHHHHHHHI",
                       short.encode("ascii"), attr, 0,     # attr, NTRes
                       0,                                  # CrtTimeTenth
                       0, 0,                               # CrtTime, CrtDate
                       0,                                  # LstAccDate
                       0,                                  # FstClusHI
                       0, 0x21,                            # WrtTime, WrtDate
                       cluster & 0xFFFF, size)


class Fat16Volume:
    def __init__(self, total_sectors: int, spc: int = 4, root_entries: int = 512,
                 label: str = "XTEINKX3"):
        self.total = total_sectors
        self.part_lba = PART_LBA
        self.part_sectors = total_sectors - PART_LBA
        self.spc = spc
        self.root_entries = root_entries
        self.label = label
        self.root_sectors = root_entries * 32 // SECTOR
        fat = 1
        while True:
            data_sectors = self.part_sectors - 1 - 2 * fat - self.root_sectors
            clusters = data_sectors // spc
            need = math.ceil((clusters + 2) * 2 / SECTOR)
            if need <= fat:
                self.fat_sectors = fat
                break
            fat = need
        self.data_sectors = self.part_sectors - 1 - 2 * self.fat_sectors - self.root_sectors
        self.clusters = self.data_sectors // spc
        if not 4085 <= self.clusters <= 65524:
            raise SystemExit(f"{self.clusters} clusters is not a FAT16 volume")
        self.data_lba = self.part_lba + 1 + 2 * self.fat_sectors + self.root_sectors
        self.img = bytearray(total_sectors * SECTOR)
        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0xFFF8
        self.fat[1] = 0xFFFF
        self.next_cluster = 2

    # -- cluster allocation ----------------------------------------------
    def alloc(self, count: int) -> int:
        if count == 0:
            return 0
        first = self.next_cluster
        for i in range(count):
            cluster = self.next_cluster + i
            self.fat[cluster] = 0xFFFF if i == count - 1 else cluster + 1
        self.next_cluster += count
        if self.next_cluster > self.clusters + 1:
            raise SystemExit("image is full")
        return first

    def cluster_offset(self, cluster: int) -> int:
        return (self.data_lba + (cluster - 2) * self.spc) * SECTOR

    def store(self, data: bytes) -> int:
        """Store ``data`` in freshly allocated clusters and return the first one."""
        first = self.alloc(max(1, math.ceil(len(data) / (self.spc * SECTOR))))
        self.img[self.cluster_offset(first):self.cluster_offset(first) + len(data)] = data
        return first

    # -- on-disk structures ----------------------------------------------
    def write_structures(self, root_body: bytes) -> None:
        # MBR with one active FAT16 partition
        mbr = bytearray(SECTOR)
        struct.pack_into("<B3sB3sII", mbr, 446, 0x00, b"\x00\x00\x00", 0x06,
                         b"\xfe\xff\xff", self.part_lba, self.part_sectors)
        mbr[510:512] = b"\x55\xaa"
        self.img[0:SECTOR] = mbr

        # boot sector / BPB
        boot = bytearray(SECTOR)
        boot[0:3] = b"\xeb\x3c\x90"
        boot[3:11] = b"MSWIN4.1"
        tot16 = self.part_sectors if self.part_sectors <= 0xFFFF else 0
        tot32 = 0 if tot16 else self.part_sectors
        struct.pack_into("<HBHBHHBHHHII", boot, 11, SECTOR, self.spc, 1, 2,
                         self.root_entries, tot16, 0xF8, self.fat_sectors, 32, 64,
                         self.part_lba, tot32)
        boot[36] = 0x80                      # drive number
        boot[38] = 0x29                      # extended boot signature
        struct.pack_into("<I", boot, 39, 0x58334B51)
        boot[43:54] = self.label.ljust(11)[:11].encode("ascii")
        boot[54:62] = b"FAT16   "
        boot[510:512] = b"\x55\xaa"
        self.img[self.part_lba * SECTOR:(self.part_lba + 1) * SECTOR] = boot

        # both FAT copies
        fat_bytes = bytearray(self.fat_sectors * SECTOR)
        for index, value in enumerate(self.fat):
            struct.pack_into("<H", fat_bytes, index * 2, value)
        for copy in range(2):
            off = (self.part_lba + 1 + copy * self.fat_sectors) * SECTOR
            self.img[off:off + len(fat_bytes)] = fat_bytes

        # root directory
        root_off = (self.part_lba + 1 + 2 * self.fat_sectors) * SECTOR
        root = direntry(self.label.ljust(11)[:11], ATTR_VOLUME_ID) + root_body
        if len(root) > self.root_sectors * SECTOR:
            raise SystemExit("root directory overflow")
        self.img[root_off:root_off + len(root)] = root

    def save(self, path: str) -> None:
        with open(path, "wb") as fh:
            fh.write(self.img)


def dir_body(items: list[tuple[str, int, int, int]]) -> bytes:
    """Serialize (name, attr, cluster, size) items into a directory body."""
    out = bytearray()
    for name, attr, cluster, size in items:
        if name in (".", ".."):
            out += direntry(name.ljust(11), ATTR_DIR, cluster)
            continue
        short, need_lfn = short_name(name, attr == ATTR_DIR)
        if need_lfn:
            out += lfn_entries(name, short)
        out += direntry(short, attr, cluster, size)
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="build/qemu/sd.img")
    ap.add_argument("--size", default="64M", help="image size, e.g. 64M (default)")
    ap.add_argument("--add", action="append", default=[], metavar="HOST:GUEST",
                    help="copy a host file to GUEST (/books/<name> or /.crosspoint/<name>);"
                         " the second form seeds settings, the shelf state or a locale")
    args = ap.parse_args()

    size = args.size.strip().upper()
    if size.endswith("M"):
        total_bytes = int(float(size[:-1]) * 1024 * 1024)
    elif size.endswith("K"):
        total_bytes = int(float(size[:-1]) * 1024)
    else:
        total_bytes = int(size)

    volume = Fat16Volume(total_bytes // SECTOR)

    test_data = (b"Xteink X3 QEMU SD image (Q4).\r\n"
                 b"This file exists so the firmware's library scan finds at least one "
                 b"document on the card.\r\n")
    test_cluster = volume.store(test_data)

    extra_files = {"/books": [], "/.crosspoint": []}
    for spec in args.add:
        if ":" not in spec:
            raise SystemExit(f"--add wants HOST:GUEST, got {spec!r}")
        host, guest = spec.split(":", 1)
        with open(host, "rb") as fh:
            data = fh.read()
        parent, _, name = guest.rpartition("/")
        if parent not in extra_files or not name:
            raise SystemExit(
                f"--add: guest must be /books/<name> or /.crosspoint/<name>, got {guest!r}")
        extra_files[parent].append((name, volume.store(data), len(data)))

    # Allocate the directories first, so "." can name its own cluster and ".."
    # can name the parent, then write the bodies in place.
    books_cluster = volume.alloc(1)
    cross_cluster = volume.alloc(1)

    books_items = [(".", ATTR_DIR, books_cluster, 0), ("..", ATTR_DIR, 0, 0),
                   ("test.txt", ATTR_ARCHIVE, test_cluster, len(test_data))]
    books_items += [(name, ATTR_ARCHIVE, cluster, size_)
                    for name, cluster, size_ in extra_files["/books"]]
    body = dir_body(books_items)
    volume.img[volume.cluster_offset(books_cluster):
               volume.cluster_offset(books_cluster) + len(body)] = body

    cross_items = [(".", ATTR_DIR, cross_cluster, 0), ("..", ATTR_DIR, 0, 0)]
    cross_items += [(name, ATTR_ARCHIVE, cluster, size_)
                    for name, cluster, size_ in extra_files["/.crosspoint"]]
    cross_body = dir_body(cross_items)
    volume.img[volume.cluster_offset(cross_cluster):
               volume.cluster_offset(cross_cluster) + len(cross_body)] = cross_body

    root_items = [(".crosspoint", ATTR_DIR, cross_cluster, 0),
                  ("books", ATTR_DIR, books_cluster, 0)]
    volume.write_structures(dir_body(root_items))
    volume.save(args.out)

    print(f"{args.out}: {total_bytes} bytes, FAT16, {volume.spc * SECTOR} B/cluster, "
          f"{volume.clusters} clusters, {volume.fat_sectors} FAT sectors, "
          f"partition @{PART_LBA}, data @{volume.data_lba}")
    print("  /books/test.txt")
    print("  /.crosspoint/")
    for parent, items in extra_files.items():
        for name, _, size_ in items:
            print(f"  {parent}/{name} ({size_} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
