#!/usr/bin/env python3
"""Rotate a PGM produced by tools/qemu/panel-frame.py so the X3 UI reads upright.

The panel RAM dump is 792x528 landscape; the device shows it in portrait, and the
firmware draws text rotated 90 degrees. This turns the dump back the way a human
sees the screen and writes a PNG (pure stdlib: zlib + struct, no PIL here).
"""
import argparse
import struct
import sys
import zlib


def read_pgm(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if not data.startswith(b"P5"):
        raise SystemExit(f"{path}: not a binary PGM")
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while data[pos:pos + 1] not in (b"\n", b""):
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1
    w, h, _maxval = fields
    return w, h, bytearray(data[pos:pos + w * h])


def write_png(path, w, h, pixels):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += pixels[y * w:(y + 1) * w]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", ihdr))
        fh.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        fh.write(chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--input", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--turn", choices=("cw", "ccw"), default="cw")
    args = ap.parse_args()

    w, h, pixels = read_pgm(args.input)
    if args.turn == "cw":
        nw, nh = h, w
        out = bytearray(nw * nh)
        for y in range(h):
            row = y * w
            for x in range(w):
                out[x * nw + (h - 1 - y)] = pixels[row + x]
    else:
        nw, nh = h, w
        out = bytearray(nw * nh)
        for y in range(h):
            row = y * w
            for x in range(w):
                out[(w - 1 - x) * nw + y] = pixels[row + x]
    write_png(args.out, nw, nh, out)
    print(f"{args.out}: {nw}x{nh} ({args.turn})")


if __name__ == "__main__":
    sys.exit(main())
