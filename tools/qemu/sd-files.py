#!/usr/bin/env python3
"""Read the FAT16 microSD image of the QEMU run from the host (Q6).

``make-sd-image.py`` writes the card, the firmware then reads AND writes it
inside the emulator; this script is the independent check that the bytes really
landed in ``build/qemu/sd.img``: it parses MBR -> BPB -> FAT -> directories and
lists (or dumps) what is on the card, without mounting it and without mtools.

    tools/qemu/sd-files.py                                  # whole tree
    tools/qemu/sd-files.py --deleted                        # ... plus removed entries
    tools/qemu/sd-files.py --cat /.crosspoint/shelf/books.json
    tools/qemu/sd-files.py --extract /books/test.txt --out /tmp/test.txt
    tools/qemu/sd-files.py --json build/qemu/sd-files.json

Exit code: 0 on success, 1 if the image is not a FAT16 volume, 2 if a requested
path does not exist.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys

SECTOR = 512
ATTR_LFN = 0x0F
ATTR_DIR = 0x10
ATTR_VOLUME_ID = 0x08


class FatError(RuntimeError):
    pass


class Fat16:
    def __init__(self, image: bytes):
        self.img = image
        if len(image) < 2 * SECTOR:
            raise FatError("image is too small to hold a filesystem")

        self.part_lba, self.part_sectors = self._read_mbr()
        base = self.part_lba * SECTOR
        boot = image[base:base + SECTOR]
        if boot[510:512] != b"\x55\xaa":
            raise FatError("partition boot sector has no 0x55AA signature")

        (self.bytes_per_sector, self.spc, self.reserved, self.num_fats,
         self.root_entries, total16, self.media, fatsz16, _spt, _heads,
         _hidden, total32) = struct.unpack_from("<HBHBHHBHHHII", boot, 11)
        if self.bytes_per_sector != SECTOR:
            raise FatError(f"unexpected sector size {self.bytes_per_sector}")
        self.total_sectors = total16 or total32
        self.fat_sectors = fatsz16
        self.root_sectors = self.root_entries * 32 // SECTOR
        self.fat_lba = self.part_lba + self.reserved
        self.root_lba = self.fat_lba + self.num_fats * self.fat_sectors
        self.data_lba = self.root_lba + self.root_sectors
        data_sectors = self.total_sectors - (self.data_lba - self.part_lba)
        self.clusters = data_sectors // self.spc
        if not 4085 <= self.clusters <= 65524:
            raise FatError(f"{self.clusters} clusters: not FAT16 (FAT12/32 unsupported)")
        label = boot[43:54].decode("latin-1", "replace").strip()
        self.label = label
        self._fat = None

    # -- low level ---------------------------------------------------------- #
    def _read_mbr(self) -> tuple[int, int]:
        mbr = self.img[0:SECTOR]
        if mbr[510:512] != b"\x55\xaa":
            raise FatError("no MBR signature at 0x1FE (not an MBR-partitioned image)")
        for index in range(4):
            off = 446 + 16 * index
            ptype = mbr[off + 4]
            if ptype in (0x04, 0x06, 0x0E):          # FAT16 (any CHS/LBA flavour)
                lba = struct.unpack_from("<I", mbr, off + 8)[0]
                count = struct.unpack_from("<I", mbr, off + 12)[0]
                return lba, count
        raise FatError("no FAT16 partition found in the MBR")

    @property
    def fat(self) -> list[int]:
        if self._fat is None:
            raw = self.img[self.fat_lba * SECTOR:(self.fat_lba + self.fat_sectors) * SECTOR]
            self._fat = list(struct.unpack(f"<{self.clusters + 2}H", raw[:2 * (self.clusters + 2)]))
        return self._fat

    def cluster_offset(self, cluster: int) -> int:
        return (self.data_lba + (cluster - 2) * self.spc) * SECTOR

    def chain(self, cluster: int) -> list[int]:
        out: list[int] = []
        seen = set()
        while 2 <= cluster < 0xFFF8:
            if cluster in seen:
                raise FatError(f"cluster chain loops at {cluster}")
            seen.add(cluster)
            out.append(cluster)
            cluster = self.fat[cluster]
        return out

    def read_cluster_bytes(self, cluster: int) -> bytes:
        off = self.cluster_offset(cluster)
        return self.img[off:off + self.spc * SECTOR]

    def read_file(self, start_cluster: int, size: int) -> bytes:
        if size == 0:
            return b""
        data = b"".join(self.read_cluster_bytes(c) for c in self.chain(start_cluster))
        return data[:size]

    # -- directories -------------------------------------------------------- #
    def root_bytes(self) -> bytes:
        off = self.root_lba * SECTOR
        return self.img[off:off + self.root_sectors * SECTOR]

    @staticmethod
    def parse_entries(raw: bytes, include_deleted: bool = False) -> list[dict]:
        entries: list[dict] = []
        lfn: list[str] = []
        for index in range(0, len(raw) - 31, 32):
            entry = raw[index:index + 32]
            first = entry[0]
            if first == 0x00:
                break
            deleted = first == 0xE5
            if entry[11] == ATTR_LFN:
                chunk = entry[1:11] + entry[14:26] + entry[28:32]
                text = chunk.decode("utf-16-le", "replace")
                text = text.split("\x00", 1)[0]
                lfn.append(text)
                continue
            short = entry[0:11].decode("latin-1", "replace")
            attr = entry[11]
            long_name = "".join(reversed(lfn)) if lfn else ""
            lfn = []
            if attr == ATTR_VOLUME_ID:
                continue
            cluster = struct.unpack_from("<H", entry, 26)[0]
            size = struct.unpack_from("<I", entry, 28)[0]
            name = long_name or format_short(short, entry[12])
            if name in (".", ".."):
                continue
            if deleted and not include_deleted:
                continue
            entries.append({
                "name": name,
                "deleted": deleted,
                "dir": bool(attr & ATTR_DIR),
                "cluster": cluster,
                "size": size,
            })
        return entries

    def listdir(self, cluster: int, include_deleted: bool = False) -> list[dict]:
        if cluster == 0:
            raw = self.root_bytes()
        else:
            raw = b"".join(self.read_cluster_bytes(c) for c in self.chain(cluster))
        return self.parse_entries(raw, include_deleted)

    # -- walking ------------------------------------------------------------ #
    def walk(self, start: int = 0, prefix: str = "", include_deleted: bool = False,
             depth: int = 8) -> list[dict]:
        out: list[dict] = []
        for entry in self.listdir(start, include_deleted):
            path = f"{prefix}/{entry['name']}"
            item = dict(entry, path=path)
            out.append(item)
            if entry["dir"] and not entry["deleted"] and depth > 0 and entry["cluster"]:
                out.extend(self.walk(entry["cluster"], path, include_deleted, depth - 1))
        return out

    def lookup(self, path: str) -> dict:
        parts = [p for p in path.split("/") if p]
        cluster = 0
        entry = None
        for part in parts:
            entry = next((e for e in self.listdir(cluster) if e["name"].lower() == part.lower()),
                         None)
            if entry is None:
                raise FatError(f"not found: {path}")
            cluster = entry["cluster"]
        assert entry is not None
        return entry


def format_short(short: str, nt_res: int) -> str:
    base, ext = short[:8].rstrip(), short[8:11].rstrip()
    if nt_res & 0x08:
        base = base.lower()
    if nt_res & 0x10:
        ext = ext.lower()
    return f"{base}.{ext}" if ext else base


def human(size: int) -> str:
    for unit in ("B", "KiB", "MiB"):
        if size < 1024 or unit == "MiB":
            return f"{size} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1024
    return f"{size} B"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", default="build/qemu/sd.img")
    ap.add_argument("--deleted", action="store_true", help="also show entries marked 0xE5")
    ap.add_argument("--cat", metavar="PATH", help="dump one file to stdout")
    ap.add_argument("--extract", metavar="PATH", help="write one file to --out")
    ap.add_argument("--out", default="", help="destination for --extract")
    ap.add_argument("--json", default="", help="write the whole tree as JSON")
    args = ap.parse_args(argv)

    try:
        with open(args.image, "rb") as fh:
            image = fh.read()
    except OSError as exc:
        print(f"sd-files: {exc}", file=sys.stderr)
        return 1

    try:
        vol = Fat16(image)
    except FatError as exc:
        print(f"sd-files: {exc}", file=sys.stderr)
        return 1

    if args.cat or args.extract:
        path = args.cat or args.extract
        try:
            entry = vol.lookup(path)
        except FatError as exc:
            print(f"sd-files: {exc}", file=sys.stderr)
            return 2
        if entry["dir"]:
            print(f"sd-files: {path} is a directory", file=sys.stderr)
            return 2
        data = vol.read_file(entry["cluster"], entry["size"])
        if args.cat:
            sys.stdout.buffer.write(data)
            if data and not data.endswith(b"\n"):
                sys.stdout.buffer.write(b"\n")
        else:
            if not args.out:
                print("sd-files: --extract wants --out", file=sys.stderr)
                return 2
            with open(args.out, "wb") as fh:
                fh.write(data)
            print(f"{path} -> {args.out} ({len(data)} bytes)")
        return 0

    tree = vol.walk(include_deleted=args.deleted)
    print(f"{args.image}: FAT16 '{vol.label}', partition @{vol.part_lba}, "
          f"{vol.spc * SECTOR} B/cluster, {vol.clusters} clusters")
    total = 0
    for entry in tree:
        total += entry["size"]
        mark = "/" if entry["dir"] else ""
        flag = " (deleted)" if entry["deleted"] else ""
        print(f"  {entry['path']}{mark}{flag}"
              + ("" if entry["dir"] else f"  {human(entry['size'])}"))
    files = sum(1 for e in tree if not e["dir"])
    print(f"  {files} file(s), {total} bytes")
    if args.json:
        with open(args.json, "w") as fh:
            json.dump({"image": args.image, "label": vol.label, "entries": tree}, fh, indent=2)
            fh.write("\n")
        print(f"  json: {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
