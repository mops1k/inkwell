#!/usr/bin/env python3
"""Render the QEMU UC8253 panel RAM dump into a PNG (and a PGM next to it).

The panel model (hw/display/xteink_uc8253.c) writes one record per
DISPLAY_REFRESH command to an optional chardev:

    -chardev file,id=xteink-panel,path=build/qemu/frame.bin
    -machine esp32c3,xteink-panel=xteink-panel

Record layout (little endian, no padding):

    u32 magic 'X3PN' | u32 version | u32 width | u32 height | u32 stride
    u32 planes | u32 flags | u32 index
    then `planes` planes of stride*height bytes. Since version 2 there are four:

      plane 0  DTM1 (0x10, the "old" RAM, or the LSB plane of a 4-level pass)
      plane 1  DTM2 (0x13, the "new" RAM, or the MSB plane of a 4-level pass)
      plane 2  screen MSB  } composed picture: level = 2*MSB + LSB,
      plane 3  screen LSB  } 0 = paper, 1 = light gray, 2 = dark gray, 3 = black

flags bit0 = rows are in gate order (bottom-up), bit1 = the refresh was a
4-level grayscale pass (the RAM planes are a code, not a picture).

Rows are stored the way the driver streams them (bottom gate line first), so
the renderer flips them back into logical top-down order. One bit per pixel,
MSB first: in the DTM planes bit 1 = white (BW coding); in the screen planes the
pair is the 2-bit level above. The panel's default 0xFF fill is white, which is
what a blank/splash frame must look like.

Why the screen planes exist (Q7): the reader paints its page, then runs a second
4-level pass that only writes the *anti-aliasing* planes and refreshes again.
Rendering that last record through the DTM planes shows the gray code, which
looks like a black page -- the page itself never moved. `--plane screen` (the
default) shows what the panel displays: the BW picture with the gray nudge on
top.

Usage:
  tools/qemu/panel-frame.py                        # last frame -> build/qemu/frame.png
  tools/qemu/panel-frame.py --plane dtm2 --out /tmp/new.png   # raw RAM plane, 1 bit
  tools/qemu/panel-frame.py --stats-only --all     # per-record statistics
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import zlib

MAGIC = 0x4E503358  # "X3PN"
HDR_SIZE = 32
DEFAULT_INPUT = "build/qemu/frame.bin"
DEFAULT_OUT = "build/qemu/frame.png"

FLAG_BOTTOM_UP = 0x1
FLAG_GRAY4 = 0x2

PLANE_SCREEN_MSB = 2
PLANE_SCREEN_LSB = 3

# Screen level (0..3) -> 8-bit intensity: paper, light gray, dark gray, black.
LEVEL_INTENSITY = (255, 170, 85, 0)
LEVEL_NAMES = ("paper", "light", "dark", "black")


class Frame:
    def __init__(self, header: tuple, payload: bytes):
        (magic, version, width, height, stride, planes, flags, index) = header
        self.magic = magic
        self.version = version
        self.width = width
        self.height = height
        self.stride = stride
        self.planes = planes
        self.flags = flags
        self.index = index
        self.payload = payload

    def plane(self, which: int) -> bytes:
        size = self.stride * self.height
        return self.payload[which * size:(which + 1) * size]

    @property
    def gray4(self) -> bool:
        """The refresh behind this record was a 4-level grayscale pass."""
        return bool(self.flags & FLAG_GRAY4)

    @property
    def has_screen(self) -> bool:
        return self.planes > PLANE_SCREEN_LSB

    def screen_planes(self) -> tuple[bytes, bytes] | None:
        if not self.has_screen:
            return None
        return self.plane(PLANE_SCREEN_MSB), self.plane(PLANE_SCREEN_LSB)

    def rows_logical(self, plane: bytes) -> list[bytes]:
        """Rows top-down, still 1 bpp MSB-first."""
        stride = self.stride
        rows = [plane[y * stride:(y + 1) * stride] for y in range(self.height)]
        if self.flags & FLAG_BOTTOM_UP:  # stored bottom-up
            rows.reverse()
        return rows


def read_frames(path: str) -> list[Frame]:
    with open(path, "rb") as fh:
        blob = fh.read()
    frames: list[Frame] = []
    off = 0
    while off + HDR_SIZE <= len(blob):
        header = struct.unpack_from("<8I", blob, off)
        if header[0] != MAGIC:
            raise SystemExit(
                f"{path}: record at offset {off} has magic 0x{header[0]:08x}, "
                f"expected 0x{MAGIC:08x} (is this a panel frame dump?)")
        width, height, stride, planes = header[2], header[3], header[4], header[5]
        size = stride * height
        if off + HDR_SIZE + size * planes > len(blob):
            # A partially written record (the run was killed mid-dump).
            break
        frames.append(Frame(header, blob[off + HDR_SIZE:off + HDR_SIZE + size * planes]))
        off += HDR_SIZE + size * planes
    return frames


def png_bytes(width: int, height: int, rows: list[bytes]) -> bytes:
    """1 bit per pixel greyscale PNG (0 = black, 1 = white)."""
    raw = b"".join(b"\x00" + row for row in rows)
    return _png(width, height, 1, raw)


def png_gray_bytes(width: int, height: int, levels: list[bytes]) -> bytes:
    """8 bit greyscale PNG from one level byte (0..3) per pixel."""
    table = bytes(LEVEL_INTENSITY)
    raw = b"".join(b"\x00" + bytes(map(table.__getitem__, row)) for row in levels)
    return _png(width, height, 8, raw)


def _png(width: int, height: int, depth: int, raw: bytes) -> bytes:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    # colour type 0 (greyscale), no interlace; bit depth 1 or 8
    ihdr = struct.pack(">IIBBBBB", width, height, depth, 0, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def pgm_bytes(width: int, height: int, rows: list[bytes]) -> bytes:
    out = bytearray(f"P5\n{width} {height}\n255\n".encode())
    for row in rows:
        for byte in row:
            for bit in range(7, -1, -1):
                out.append(255 if (byte >> bit) & 1 else 0)
    return bytes(out)


def pgm_gray_bytes(width: int, height: int, levels: list[bytes]) -> bytes:
    table = bytes(LEVEL_INTENSITY)
    out = bytearray(f"P5\n{width} {height}\n255\n".encode())
    for row in levels:
        out.extend(map(table.__getitem__, row))
    return bytes(out)


def stats(plane: bytes, width: int, height: int, label: str) -> dict:
    histogram: dict[int, int] = {}
    white = 0
    total = 0
    nonempty_rows = 0
    stride = width // 8
    for row in range(height):
        line = plane[row * stride:(row + 1) * stride]
        if any(b != line[0] for b in line) or (line and line[0] not in (0x00, 0xFF)):
            nonempty_rows += 1
        for byte in line:
            histogram[byte] = histogram.get(byte, 0) + 1
        # count white bits (bit = 1)
        white += sum(bin(byte).count("1") for byte in line)
    if line_bytes := len(plane):
        total = line_bytes * 8
    black = total - white
    top = sorted(histogram.items(), key=lambda kv: -kv[1])[:4]
    return {
        "label": label,
        "bytes": len(plane),
        "pixels": total,
        "white": white,
        "black": black,
        "black_pct": (100.0 * black / total) if total else 0.0,
        "distinct_bytes": len(histogram),
        "nonempty_rows": nonempty_rows,
        "top_bytes": ", ".join(f"0x{b:02x}:{n}" for b, n in top),
    }


_LEVEL_PAIRS: dict[tuple[int, int], bytes] = {}


def _pair_levels(msb: int, lsb: int) -> bytes:
    """One byte of MSB and LSB bits -> eight shade levels (0..3), MSB first."""
    key = (msb, lsb)
    got = _LEVEL_PAIRS.get(key)
    if got is None:
        got = bytes(((((msb >> bit) & 1) << 1) | ((lsb >> bit) & 1))
                    for bit in range(7, -1, -1))
        _LEVEL_PAIRS[key] = got
    return got


def levels_rows(frame: Frame) -> list[bytes] | None:
    """Screen levels (0..3 per pixel) in logical top-down order.

    ``None`` for records without the composed screen planes (version 1).
    """
    planes = frame.screen_planes()
    if planes is None:
        return None
    msb_plane, lsb_plane = planes
    stride = frame.stride
    rows: list[bytes] = []
    for y in range(frame.height):
        msb = msb_plane[y * stride:(y + 1) * stride]
        lsb = lsb_plane[y * stride:(y + 1) * stride]
        rows.append(b"".join(_pair_levels(m, l) for m, l in zip(msb, lsb)))
    if frame.flags & FLAG_BOTTOM_UP:
        rows.reverse()
    return rows


def level_stats(levels: list[bytes]) -> dict:
    """Share of each shade on a composed screen, plus the ink fraction."""
    counts = [0, 0, 0, 0]
    for row in levels:
        for value in row:
            counts[value] += 1
    total = sum(counts) or 1
    pct = [100.0 * n / total for n in counts]
    ink = (pct[1] + pct[2] + pct[3])
    return {
        "pixels": total,
        "paper_pct": pct[0],
        "light_pct": pct[1],
        "dark_pct": pct[2],
        "black_pct": pct[3],
        "ink_pct": ink,
        "gray_pct": pct[1] + pct[2],
        "counts": counts,
    }


def print_stats(st: dict, width: int, height: int) -> None:
    print(f"  {st['label']}: {st['bytes']} bytes for {width}x{height} "
          f"= {st['pixels']} pixels")
    print(f"    black {st['black']} ({st['black_pct']:.3f}%), white {st['white']}")
    print(f"    distinct byte values: {st['distinct_bytes']}, "
          f"rows with non-uniform content: {st['nonempty_rows']}")
    print(f"    most common bytes: {st['top_bytes']}")


def print_level_stats(st: dict, width: int, height: int) -> None:
    print(f"  SCREEN: {width}x{height} = {st['pixels']} pixels, "
          f"ink {st['ink_pct']:.3f}% (gray {st['gray_pct']:.3f}%)")
    print(f"    paper {st['paper_pct']:.3f}% | light gray {st['light_pct']:.3f}% | "
          f"dark gray {st['dark_pct']:.3f}% | black {st['black_pct']:.3f}%")


def ascii_preview(rows: list[bytes], width: int, height: int, cols: int = 79,
                  rows_out: int = 33) -> str:
    """Downsample the frame to text: '#' dark, '.' light. For headless checks."""
    shade = " .:-=+*#%@"
    out = []
    cell_w = max(1, width // cols)
    cell_h = max(1, height // rows_out)
    for row_block in range(rows_out):
        line = []
        for col_block in range(cols):
            dark = 0
            total = 0
            for y in range(row_block * cell_h, min(height, (row_block + 1) * cell_h), 2):
                row = rows[y]
                for x in range(col_block * cell_w, min(width, (col_block + 1) * cell_w), 2):
                    # bit set = white; count dark pixels
                    if not (row[x // 8] >> (7 - (x % 8))) & 1:
                        dark += 1
                    total += 1
            level = (dark / total) if total else 0.0
            line.append(shade[min(len(shade) - 1, int(level * (len(shade) - 1)))])
        out.append("".join(line))
    return "\n".join(out)


def ascii_preview_levels(levels: list[bytes], width: int, height: int,
                         cols: int = 79, rows_out: int = 33) -> str:
    """Same preview for screen levels: ink weight 0..1 per pixel."""
    shade = " .:-=+*#%@"
    out = []
    cell_w = max(1, width // cols)
    cell_h = max(1, height // rows_out)
    for row_block in range(rows_out):
        line = []
        for col_block in range(cols):
            total = 0
            weight = 0
            for y in range(row_block * cell_h, min(height, (row_block + 1) * cell_h), 2):
                row = levels[y]
                for x in range(col_block * cell_w, min(width, (col_block + 1) * cell_w), 2):
                    weight += row[x]
                    total += 3
            level = (weight / total) if total else 0.0
            line.append(shade[min(len(shade) - 1, int(level * (len(shade) - 1) * 3))])
        out.append("".join(line))
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--input", default=DEFAULT_INPUT)
    ap.add_argument("--out", default=DEFAULT_OUT, help="PNG output path")
    ap.add_argument("--pgm", default=None,
                    help="PGM output path (default: the PNG path with .pgm)")
    ap.add_argument("--plane", choices=("screen", "dtm1", "dtm2"), default=None,
                    help="screen (default) = what the panel shows, 4 shades; "
                         "dtm1/dtm2 = the raw RAM plane, 1 bit")
    ap.add_argument("--frame", type=int, default=-1,
                    help="record index to render, -1 = the last one")
    ap.add_argument("--all", action="store_true", help="print statistics for every record")
    ap.add_argument("--stats-only", action="store_true", help="no image output")
    ap.add_argument("--ascii", action="store_true",
                    help="also print a downsampled text preview of the frame")
    ap.add_argument("--no-pgm", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.input):
        raise SystemExit(f"{args.input}: missing. Run tools/qemu/boot.sh with the frame "
                         f"chardev enabled first (see tools/qemu/README.md, Q4).")
    frames = read_frames(args.input)
    if not frames:
        raise SystemExit(f"{args.input}: no complete frame record "
                         f"(the run may have been cut before the first DRF)")
    last = frames[-1]
    plane_arg = args.plane or ("screen" if last.has_screen else "dtm2")
    print(f"{args.input}: {len(frames)} frame record(s), v{last.version}, "
          f"last index {last.index}, {last.width}x{last.height} "
          f"stride {last.stride}, planes {last.planes}, "
          f"rows {'bottom-up' if last.flags & FLAG_BOTTOM_UP else 'top-down'}, "
          f"last refresh {'4-level gray' if last.gray4 else '1-bit BW'}")

    indices = range(len(frames)) if args.all else [args.frame % len(frames)]
    for idx in indices:
        frame = frames[idx]
        if plane_arg == "screen":
            levels = levels_rows(frame)
            if levels is None:
                raise SystemExit(f"{args.input}: record {idx} has no composed screen "
                                 f"(version {frame.version}, planes {frame.planes}); "
                                 f"use --plane dtm1|dtm2")
            print(f"[record {idx}] index={frame.index} refresh="
                  f"{'4-level gray' if frame.gray4 else '1-bit BW'} plane=SCREEN")
            print_level_stats(level_stats(levels), frame.width, frame.height)
            if args.all or args.stats_only:
                continue
            with open(args.out, "wb") as fh:
                fh.write(png_gray_bytes(frame.width, frame.height, levels))
            print(f"  wrote {args.out}")
            if not args.no_pgm:
                pgm = args.pgm or os.path.splitext(args.out)[0] + ".pgm"
                with open(pgm, "wb") as fh:
                    fh.write(pgm_gray_bytes(frame.width, frame.height, levels))
                print(f"  wrote {pgm}")
            if args.ascii:
                print(ascii_preview_levels(levels, frame.width, frame.height))
            continue

        which = 0 if plane_arg == "dtm1" else 1
        plane = frame.plane(which)
        print(f"[record {idx}] index={frame.index} refresh="
              f"{'4-level gray' if frame.gray4 else '1-bit BW'} plane={plane_arg.upper()}")
        print_stats(stats(plane, frame.width, frame.height, plane_arg.upper()),
                    frame.width, frame.height)
        if args.all or args.stats_only:
            continue
        rows = frame.rows_logical(plane)
        with open(args.out, "wb") as fh:
            fh.write(png_bytes(frame.width, frame.height, rows))
        print(f"  wrote {args.out}")
        if not args.no_pgm:
            pgm = args.pgm or os.path.splitext(args.out)[0] + ".pgm"
            with open(pgm, "wb") as fh:
                fh.write(pgm_bytes(frame.width, frame.height, rows))
            print(f"  wrote {pgm}")
        if args.ascii:
            print(ascii_preview(rows, frame.width, frame.height))
    return 0


if __name__ == "__main__":
    sys.exit(main())
