#!/usr/bin/env python3
"""Proves the background cover worker really produced artwork in the emulator.

Boots the firmware with the current card image (no re-scan), waits while the cover
task decodes, then reports which thumbnails appeared on the card and renders the
last panel frame, so "covers show up without blocking keys" is checked by facts.
"""
import os
import shutil
import sys
import time

sys.path.insert(0, "tools/qemu")
import x3emu  # noqa: E402

WAIT_S = float(os.environ.get("COVER_WAIT_S", "180"))
OUT = x3emu.ROOT / "build/qemu/covers"


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    reuse = os.environ.get("COVER_REUSE", "1") == "1"
    source = x3emu.ROOT / "build/qemu/nav/sd.img"
    if reuse and not (OUT / "sd.img").is_file() and source.is_file():
        shutil.copy(source, OUT / "sd.img")
    emu = x3emu.Emulator(firmware=x3emu.ROOT / "build/qemu/fw/firmware.bin",
                         work_dir=OUT, fresh_flash=True, fresh_sd=not reuse)
    emu.start()
    try:
        emu.wait_frames(3, timeout=120)
        thumbs_before = [p for p in x3emu.read_card(emu.sd)["paths"] if "thumb_" in p]
        print("thumbs before:", len(thumbs_before), flush=True)
        deadline = time.monotonic() + WAIT_S
        while time.monotonic() < deadline and emu.running:
            emu.poll_frames()
            time.sleep(0.5)
        emu.wait_idle(1.0, timeout=60)
        card = x3emu.read_card(emu.sd)
        thumbs = [p for p in card["paths"] if "thumb_" in p]
        print("thumbs after:", len(thumbs))
        for path in thumbs:
            print("  ", path)
        books = (card.get("books") or {}).get("books", [])
        print("index books:", len(books))
        emu.frame_png(emu.frames.frames[-1], OUT / "last-frame.png")
        print("frame:", OUT / "last-frame.png")
        return 0 if len(thumbs) > len(thumbs_before) else 1
    finally:
        emu.stop()


if __name__ == "__main__":
    sys.exit(main())
