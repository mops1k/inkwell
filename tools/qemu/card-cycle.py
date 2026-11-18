#!/usr/bin/env python3
"""Card round-trip check: grid -> card -> back -> card, with timings and frames.

Reproduces the "second visit to a card is empty and Back takes seconds" report so
the fix can be verified by pixels instead of by eye.
"""
import sys
import time

sys.path.insert(0, "tools/qemu")
import x3emu  # noqa: E402

OUT = x3emu.ROOT / "build/qemu/cardcycle"
OUT.mkdir(parents=True, exist_ok=True)


def wait_frame(emu, before, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and emu.running:
        emu.poll_frames()
        if len(emu.frames.frames) > before:
            return emu.frames.frames[-1]
        time.sleep(0.01)
    return None


def press(emu, button, hold_ms, label):
    before = len(emu.frames.frames)
    t0 = time.monotonic()
    emu.tap(button, hold_ms)
    frame = wait_frame(emu, before)
    dt = (frame.arrival - t0) * 1000 if frame else None
    print(f"{label:18s} {button:8s} -> {'-' if dt is None else round(dt)} ms", flush=True)
    time.sleep(0.3)
    emu.wait_idle(0.4, timeout=15)
    return frame


def main() -> int:
    import shutil
    if not (OUT / "sd.img").is_file():
        shutil.copy(x3emu.ROOT / "build/qemu/ui/sd.img", OUT / "sd.img")
    emu = x3emu.Emulator(firmware=x3emu.ROOT / "build/qemu/fw/firmware.bin", work_dir=OUT,
                         fresh_flash=True, fresh_sd=False)
    emu.start()
    try:
        emu.wait_frames(5, timeout=90)
        emu.wait_idle(1.0, timeout=60)
        emu.frame_png(emu.frames.frames[-1], OUT / "0-grid.png")
        press(emu, "right", 90, "focus fb2")
        press(emu, "confirm", 90, "open card")
        for _ in range(8):
            time.sleep(0.4)
            emu.poll_frames()
        emu.frame_png(emu.frames.frames[-1], OUT / "1b-card-settled.png")
        emu.frame_png(emu.frames.frames[-1], OUT / "1-card-first.png")
        press(emu, "back", 90, "back to grid")
        press(emu, "right", 90, "focus fb2 again")
        emu.frame_png(emu.frames.frames[-1], OUT / "2-grid-again.png")
        press(emu, "confirm", 90, "open card again")
        emu.frame_png(emu.frames.frames[-1], OUT / "3-card-second.png")
        press(emu, "back", 90, "back to grid 2")
        press(emu, "left", 90, "focus txt")
        press(emu, "left", 90, "focus txt 2")
        press(emu, "confirm", 90, "open txt card")
        emu.frame_png(emu.frames.frames[-1], OUT / "4-card-txt.png")
        print("frames:", OUT)
        return 0
    finally:
        emu.stop()


if __name__ == "__main__":
    sys.exit(main())
