#!/usr/bin/env python3
"""Which cell carries the inverted focus plate? Prints the answer per key press.

Reads the panel frames straight from the emulator, so the "band appeared on the
wrong cell" class of bug is caught by pixels, not by eyeballing a screenshot.
"""
import struct
import sys
import time
import zlib

sys.path.insert(0, "tools/qemu")
import x3emu  # noqa: E402

OUT = x3emu.ROOT / "build/qemu/band"
W, H = 528, 792          # upright frame size
CAPTION_Y = range(322, 372)   # title + author block of the first row


def gray_rows(path):
    """Frame -> rows of 8-bit gray pixels (the frame PNGs are 1-bit, so ffmpeg
    expands them; the harness already depends on ffmpeg for artwork previews)."""
    import subprocess
    proc = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-f", "rawvideo",
                           "-pix_fmt", "gray", "-"], capture_output=True)
    if proc.returncode != 0:
        raise SystemExit(f"ffmpeg failed: {proc.stderr[-300:]!r}")
    width, height = W, H
    data = proc.stdout
    return width, height, [data[y * width:(y + 1) * width] for y in range(height)]


def png_gray(path):
    return gray_rows(path)


def band_cell(path):
    width, height, rows = png_gray(path)
    # Cell columns of a 3-wide page, matching the theme paddings closely enough to
    # answer "which third of the screen is dark".
    thirds = [(0, width // 3), (width // 3, 2 * width // 3), (2 * width // 3, width)]
    scores = []
    for x0, x1 in thirds:
        dark = 0
        for y in CAPTION_Y:
            if y >= height:
                continue
            row = rows[y]
            dark += sum(1 for x in range(x0, x1) if row[x] < 128)
        scores.append(dark)
    total = sum(scores) or 1
    best = max(range(3), key=lambda i: scores[i])
    return best, [round(s / total, 2) for s in scores]


def main() -> int:
    buttons = (sys.argv[1:] or ["right", "right"])

    def fresh():
        if not (OUT / "sd.img").is_file():
            import shutil
            shutil.copy(x3emu.ROOT / "build/qemu/nav/sd.img", OUT / "sd.img")

    fresh()
    emu = x3emu.Emulator(firmware=x3emu.ROOT / "build/qemu/fw/firmware.bin", work_dir=OUT,
                         fresh_flash=True, fresh_sd=False)
    emu.start()
    try:
        emu.wait_frames(3, timeout=120)
        emu.wait_idle(1.0, timeout=60)
        emu.frame_png(emu.frames.frames[-1], OUT / "focus-0.png")
        cell, share = band_cell(OUT / "focus-0.png")
        print(f"start: plate on cell {cell} {share}", flush=True)
        for step, button in enumerate(buttons, start=1):
            before = len(emu.frames.frames)
            t0 = time.monotonic()
            emu.tap(button, 90)
            deadline = time.monotonic() + 20
            got = None
            while time.monotonic() < deadline and emu.running:
                emu.poll_frames()
                if len(emu.frames.frames) > before:
                    got = emu.frames.frames[-1]
                    break
                time.sleep(0.01)
            dt = (got.arrival - t0) * 1000 if got else None
            emu.wait_idle(0.4, timeout=10)
            path = OUT / f"focus-{step}.png"
            emu.frame_png(emu.frames.frames[-1], path)
            cell, share = band_cell(path)
            print(f"{button:6s} -> {'-' if dt is None else round(dt)} ms, plate cell {cell} {share}", flush=True)
        return 0
    finally:
        emu.stop()


if __name__ == "__main__":
    sys.exit(main())
