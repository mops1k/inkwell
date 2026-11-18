#!/usr/bin/env python3
"""Grid navigation latency in the emulator: press Right/Down on the shelf and
time the first panel frame whose content differs from the one before the press.

Run after `pio run -e default`; uses whatever build/qemu/fw/firmware.bin holds.
Real books in build/qemu/sd-books make the first scan slow (minutes), so the
script waits for the index on the card instead of a fixed sleep.
"""
import hashlib
import os
import sys
import time

sys.path.insert(0, "tools/qemu")
import x3emu  # noqa: E402

OUT = x3emu.ROOT / "build/qemu/nav"
OUT.mkdir(parents=True, exist_ok=True)


def sig(frame):
    return hashlib.md5(frame.plane(1)).hexdigest()[:8]


def next_changed(emu, before_count, before_sig, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and emu.running:
        emu.poll_frames()
        for candidate in emu.frames.frames[before_count:]:
            if sig(candidate) != before_sig:
                return candidate
        time.sleep(0.01)
    return None


def main() -> int:
    # NAV_REUSE=1 keeps the card image (index + thumbnails already on it), which
    # skips the multi-minute scan of the real .fb2.zip books: handy when only the
    # repaint path is being measured.
    reuse_sd = os.environ.get("NAV_REUSE") == "1"
    emu = x3emu.Emulator(firmware=x3emu.ROOT / "build/qemu/fw/firmware.bin",
                         work_dir=OUT, fresh_flash=True, fresh_sd=not reuse_sd)
    emu.start()
    try:
        emu.wait_frames(3, timeout=120)
        # Real .fb2.zip books take minutes to scan in the emulator; poll the card.
        deadline = time.monotonic() + 300
        while time.monotonic() < deadline and emu.running:
            index = x3emu.read_card(emu.sd).get("books")
            if index and len(index.get("books", [])) >= 3:
                break
            emu.poll_frames()
            time.sleep(2)
        emu.wait_idle(1.0, timeout=60)
        print("books:", len((x3emu.read_card(emu.sd).get("books") or {}).get("books", [])))

        # Optional warm-up: the grid generates one cover per idle tick, and a
        # .fb2.zip decode is slow in the emulator. Waiting here separates "keys are
        # slow" from "a decode that started in an idle gap delayed the next key".
        warm = float(os.environ.get("NAV_WARM_S", "0"))
        if warm > 0:
            deadline = time.monotonic() + warm
            while time.monotonic() < deadline and emu.running:
                emu.poll_frames()
                time.sleep(0.5)
            emu.wait_idle(1.0, timeout=30)
            thumbs = [p for p in x3emu.read_card(emu.sd)["paths"] if "thumb_" in p]
            print(f"warmed {warm:.0f}s, thumbs: {len(thumbs)}")

        results = []
        # Rapid presses (< idle threshold) prove that warming is not what the key waits for.
        for button in ("right", "right", "down", "left", "up", "right", "down"):
            before_count = len(emu.frames.frames)
            before_sig = sig(emu.frames.frames[-1])
            t0 = time.monotonic()
            emu.tap(button, 90)
            frame = next_changed(emu, before_count, before_sig, timeout=20)
            dt = (frame.arrival - t0) * 1000 if frame else None
            results.append((button, None if dt is None else round(dt)))
            print(f"  {button:6s} -> {'' if dt is None else round(dt)} ms", flush=True)
            time.sleep(0.15)
        ok = all(dt is not None and dt < 400 for _, dt in results)
        print("RESULT:", "OK" if ok else "SLOW", results)
        return 0 if ok else 1
    finally:
        emu.stop()


if __name__ == "__main__":
    sys.exit(main())
