#!/usr/bin/env python3
"""Pack a raw 8-bit grayscale image into a 1-bit C header (MSB-first, 1 = white).

The format matches the existing icon headers in ``src/images`` and
``src/components/icons``: rows packed left to right, 8 pixels per byte, most
significant bit first, a set bit means white.

Usage:
    pack_logo.py INPUT.gray WIDTH HEIGHT OUTPUT.h [--symbol Logo120]

``INPUT.gray`` is produced from any PNG/SVG render, e.g.:
    rsvg-convert -w 120 -h 120 icon.svg -o icon.png
    ffmpeg -y -i icon.png -pix_fmt gray -f rawvideo icon.gray
"""

import argparse
import sys


def pack(path: str, width: int, height: int, threshold: int = 128) -> bytes:
    data = open(path, "rb").read()
    expected = width * height
    if len(data) != expected:
        raise SystemExit(f"{path}: expected {expected} bytes of 8-bit gray, got {len(data)}")

    out = bytearray()
    row_bytes = (width + 7) // 8
    for y in range(height):
        row = bytearray()
        for x0 in range(0, width, 8):
            byte = 0
            for j in range(8):
                x = x0 + j
                pixel = 255 if x >= width else data[y * width + x]
                byte = (byte << 1) | (1 if pixel >= threshold else 0)
            row.append(byte)
        assert len(row) == row_bytes
        out.extend(row)
    return bytes(out)


def write_header(payload: bytes, width: int, height: int, symbol: str, path: str) -> None:
    lines = [
        "#pragma once",
        "#include <cstdint>",
        "",
        f"// Image dimensions: {width}x{height}",
        f"static const uint8_t {symbol}[] = {{",
    ]
    for i in range(0, len(payload), 16):
        chunk = ", ".join(f"0x{b:02X}" for b in payload[i : i + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")
    open(path, "w").write("\n".join(lines))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input")
    ap.add_argument("width", type=int)
    ap.add_argument("height", type=int)
    ap.add_argument("output")
    ap.add_argument("--symbol", default="Logo120")
    ap.add_argument("--threshold", type=int, default=128)
    args = ap.parse_args()

    payload = pack(args.input, args.width, args.height, args.threshold)
    write_header(payload, args.width, args.height, args.symbol, args.output)
    print(f"{args.output}: {args.width}x{args.height}, {len(payload)} bytes ({args.symbol})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
