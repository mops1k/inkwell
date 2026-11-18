#!/usr/bin/env python3
"""q5-scenarios.py — headless checks of the X3 button injection (Q5).

Boots the real firmware in QEMU with ``xteink-buttons`` (ADC ladder + power
GPIO), then drives it through the physical key sequence and measures, for every
press, how long the firmware takes to put a new frame on the panel:

    home          the shelf grid after boot
    short Back    the main menu (pop-up)
    Confirm       the book card
    Right/Down    the selection in the grid
    long Back     the "filter and sort" dialog (hold >= 1 s)
    hold probe    long Back held for 2.5 s: when does the dialog appear?

Every step gets a PNG under build/qemu/q5/ (upright, 528x792), the timings go to
build/qemu/q5/report.json and report.md, and the script exits non-zero if a step
that must change the screen does not.

Usage:
  tools/qemu/q5-scenarios.py                 # full run + boot probe (~40 s)
  tools/qemu/q5-scenarios.py --no-boot-probe
  tools/qemu/q5-scenarios.py --firmware path/to/firmware.bin --reuse-state

Every run starts from a clean flash and a freshly built card (see "Q6" in
tools/qemu/README.md): once SD writes work, a reused flash accumulates the
BootHealth failure counter of every killed run and, after a few of them, the
firmware correctly boots into "recovery firmware mode" (the SD firmware
picker) instead of the shelf — the scenario would then measure the picker.
--reuse-state brings the old behaviour back (whatever flash and card are
already in build/qemu/q5/run).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import x3emu  # noqa: E402

OUT_DIR = x3emu.ROOT / "build/qemu/q5"
BOOKS = {
    "alpha.txt": "Alpha\n\nChapter one.\nThe first test book on the emulated card.\n",
    "beta.txt": "Beta\n\nChapter one.\nA second book, so the grid has something to move through.\n",
    "gamma.txt": "Gamma\n\nChapter one.\nA third book for the filter dialog.\n",
}


def sig(frame) -> str:
    return hashlib.md5(frame.plane(1)).hexdigest()[:8]


class Runner:
    def __init__(self, args):
        self.args = args
        self.emu: x3emu.Emulator | None = None
        self.steps: list[dict] = []
        self.failures: list[str] = []

    # -- setup -------------------------------------------------------------- #
    def build_card(self, work_dir: Path) -> Path:
        """A FAT16 card with three books, so navigation and filters are visible.

        Always rebuilt: a card that keeps its state makes the run depend on the
        previous one (the firmware caches covers and the shelf index on it, and
        the menu can be left open at the moment the emulator is killed).
        """
        card = work_dir / "sd.img"
        if self.args.reuse_state and card.is_file():
            return card
        books_dir = OUT_DIR / "books"
        books_dir.mkdir(parents=True, exist_ok=True)
        cmd = [sys.executable or "python3", str(x3emu.ROOT / "tools/qemu/make-sd-image.py"),
               "--out", str(card)]
        for name, text in BOOKS.items():
            host = books_dir / name
            host.write_text(text)
            cmd += ["--add", f"{host}:/books/{name}"]
        proc = subprocess.run(cmd, cwd=x3emu.ROOT, capture_output=True, text=True)
        if proc.returncode != 0:
            raise x3emu.EmulatorError(f"make-sd-image.py failed:\n{proc.stdout}{proc.stderr}")
        return card

    def start(self) -> None:
        work_dir = OUT_DIR / "run"
        work_dir.mkdir(parents=True, exist_ok=True)
        card = self.build_card(work_dir)
        self.emu = x3emu.Emulator(firmware=self.args.firmware or None, work_dir=work_dir,
                                  fresh_flash=not self.args.reuse_state, sd_image=card)
        t0 = time.monotonic()
        self.emu.start()
        self.started_at = t0
        self.emu.wait_frames(3, timeout=self.args.boot_timeout)
        self.emu.wait_idle(0.6, timeout=self.args.boot_timeout)
        self.boot_seconds = time.monotonic() - t0
        print(f"booted: {self.boot_seconds:.2f} s wall to a stable screen, "
              f"{len(self.emu.frames.frames)} frame(s), "
              f"panic messages: {self.emu.panic_count() // 2} "
              f"(reboots: {self.emu.boot_count() - 1})", flush=True)

    # -- one press ---------------------------------------------------------- #
    def step(self, label: str, button: str, hold_ms: float, expect_change: bool,
             png: str | None, note: str = "") -> dict:
        assert self.emu is not None
        before = len(self.emu.frames.frames)
        before_sig = sig(self.emu.frames.frames[-1])
        t_press = time.monotonic()
        self.emu.tap(button, hold_ms)
        arrivals = []
        try:
            first = self.emu.wait_frame_after(t_press, timeout=self.args.timeout)
            arrivals.append(first.arrival - t_press)
        except x3emu.EmulatorError:
            print(f"  {label}: no panel refresh after {self.args.timeout:.0f} s", flush=True)
        # keep collecting until the screen settles, so a two-refresh redraw is visible
        deadline = time.monotonic() + 2.5
        while time.monotonic() < deadline:
            new = self.emu.poll_frames()
            if new:
                arrivals.extend(frame.arrival - t_press for frame in new)
                deadline = time.monotonic() + 0.25
            else:
                time.sleep(0.01)
        new_frames = self.emu.frames.frames[before:]
        after_sig = sig(new_frames[-1]) if new_frames else before_sig
        changed = after_sig != before_sig
        png_path = None
        if png and new_frames:
            png_path = OUT_DIR / png
            self.emu.frame_png(new_frames[-1], png_path)
        record = {
            "step": label,
            "button": button,
            "hold_ms": hold_ms,
            "frames": len(new_frames),
            "first_frame_ms": round(arrivals[0] * 1000) if arrivals else None,
            "all_frames_ms": [round(a * 1000) for a in arrivals],
            "before_sig": before_sig,
            "after_sig": after_sig,
            "changed": changed,
            "png": str(png_path.relative_to(x3emu.ROOT)) if png_path else None,
            "note": note,
        }
        self.steps.append(record)
        status = "OK" if (changed or not expect_change) else "FAIL (screen unchanged)"
        if status.startswith("FAIL"):
            self.failures.append(f"{label}: {status}")
        print(f"  {label:32s} {button:8s} {hold_ms:5.0f} ms -> "
              f"{record['first_frame_ms']} ms, {len(new_frames)} frame(s), {status}",
              flush=True)
        return record

    # -- scenarios ---------------------------------------------------------- #
    def capture_home(self) -> None:
        """PNG of the shelf as it came up: the reference for every other step."""
        assert self.emu is not None
        frame = self.emu.frames.frames[-1]
        png_path = OUT_DIR / "home.png"
        self.emu.frame_png(frame, png_path)
        self.steps.append({
            "step": "home (shelf after boot)", "button": "", "hold_ms": 0,
            "frames": 0, "first_frame_ms": None, "all_frames_ms": [],
            "before_sig": sig(frame), "after_sig": sig(frame), "changed": False,
            "png": str(png_path.relative_to(x3emu.ROOT)), "note": "baseline: no press",
        })
        print(f"  {'home (shelf after boot)':32s} {'-':8s} {'-':>8s} -> "
              f"{'-':>5} ms, 0 frame(s), OK", flush=True)

    def run(self) -> None:
        self.capture_home()
        self.step("menu: short Back", "back", 90, True, "menu.png")
        # The pop-up has seven entries, so the arrows really repaint here. The
        # grid itself cannot: the emulated card ends up with a single book (the
        # library index write to the card fails, see README "Q5"), so moving the
        # grid selection has nothing to move to.
        self.step("menu selection: Down", "down", 90, True, "menu-selection.png",
                  "arrow keys move the pop-up selection")
        self.step("menu selection: Up", "up", 90, True, None)
        self.step("close menu: short Back", "back", 90, True, None)
        self.step("book card: short Confirm", "confirm", 90, True, "book-card.png")
        self.step("back from card", "back", 90, True, None)
        self.step("long Back -> filters", "back", 1200, True, "filters.png")
        self.step("close filters", "back", 90, True, None)
        self.step("hold probe: Back held 2.5 s", "back", 2500, True, "filters-hold.png",
                  "when does the long press show up while the key is still down?")

    def boot_probe(self) -> None:
        """A key pressed before the UI exists is lost, not delayed (Q4 "1-5 s")."""
        assert self.emu is not None
        print("boot probe: restarting the emulator and pressing Back at 1.0 s", flush=True)
        self.emu.stop()
        self.emu = x3emu.Emulator(work_dir=OUT_DIR / "run", sd_image=OUT_DIR / "run/sd.img")
        t0 = time.monotonic()
        self.emu.start()
        time.sleep(1.0)
        t_press = time.monotonic()
        self.emu.tap("back", 90)
        self.emu.wait_frames(4, timeout=self.args.boot_timeout)
        self.emu.wait_idle(0.6, timeout=self.args.boot_timeout)
        ui_at = self.emu.frames.arrivals[-1] - t0
        first_after_press = next((a - t0 for a in self.emu.frames.arrivals if a > t_press), None)
        # The shelf frame from the earlier run is the reference: if the press had
        # been honoured, the first screen would be the pop-up menu instead.
        menu_sig = next((s["after_sig"] for s in self.steps if s["step"].startswith("menu")), "")
        final_sig = sig(self.emu.frames.frames[-1])
        record = {
            "step": "boot probe: Back at 1.0 s",
            "boot_seconds": round(self.boot_seconds, 2),
            "ui_stable_at_s": round(ui_at, 2),
            "first_frame_after_press_s": None if first_after_press is None else round(first_after_press, 2),
            "final_sig": final_sig,
            "menu_sig": menu_sig,
            "menu_opened": final_sig == menu_sig and bool(menu_sig),
            "frames": len(self.emu.frames.frames),
        }
        self.steps.append(record)
        print(f"  key at 1.0 s -> screen steady at {ui_at:.2f} s, "
              f"menu opened: {record['menu_opened']}", flush=True)
        if record["menu_opened"]:
            self.failures.append("boot probe: the key pressed during boot opened the menu "
                                 "(unexpected)")

    def stop(self) -> None:
        if self.emu is not None:
            self.emu.stop()
            self.emu = None

    # -- report ------------------------------------------------------------- #
    def write_report(self) -> float:
        OUT_DIR.mkdir(parents=True, exist_ok=True)
        reactions = [s["first_frame_ms"] for s in self.steps
                     if s.get("first_frame_ms") is not None and s.get("hold_ms", 0) <= 100]
        report = {
            "boot_seconds": round(self.boot_seconds, 2),
            "tap_reaction_ms": {
                "min": min(reactions) if reactions else None,
                "max": max(reactions) if reactions else None,
                "samples": reactions,
            },
            "steps": self.steps,
            "failures": self.failures,
        }
        (OUT_DIR / "report.json").write_text(json.dumps(report, indent=2) + "\n")

        lines = ["# Q5: инъекция кнопок через QEMU — измерения", ""]
        lines.append(f"Загрузка до устойчивого экрана: **{self.boot_seconds:.2f} с** "
                     f"(включая панику IDWDT на первом boot и reboot).")
        if reactions:
            lines.append(f"Отклик на короткое нажатие (нажатие → первый новый кадр): "
                         f"**{min(reactions)}–{max(reactions)} мс**.")
        lines += ["", "| шаг | кнопка | удержание | кадров | первый кадр | экран изменился | PNG |",
                  "|---|---|---|---|---|---|---|"]
        for step in self.steps:
            first = step.get("first_frame_ms")
            lines.append(f"| {step['step']} | {step.get('button') or '-'} | "
                         f"{step.get('hold_ms', 0):.0f} мс | {step.get('frames', '-')} | "
                         f"{'-' if first is None else str(first) + ' мс'} | "
                         f"{'да' if step.get('changed') else 'нет'} | "
                         f"{step.get('png') or '-'} |")
        if self.failures:
            lines += ["", "## Провалы", *[f"- {f}" for f in self.failures]]
        (OUT_DIR / "report.md").write_text("\n".join(lines) + "\n")
        return report


def check_rotation() -> bool:
    """rows_upright() must equal the naive reference on a real frame."""
    sample = OUT_DIR / "run/frame.bin"
    if not sample.is_file():
        return True
    stream = x3emu.FrameStream(sample)
    stream.poll()
    if not stream.frames:
        return True
    frame = stream.frames[-1]
    fast = x3emu.rows_upright(frame)
    slow = x3emu.rows_upright_naive(frame)
    ok = fast[0] == slow[0] and fast[1] == slow[1] and fast[2] == slow[2]
    print(f"rotation check: {'OK' if ok else 'MISMATCH'} "
          f"({fast[0]}x{fast[1]} vs {slow[0]}x{slow[1]})", flush=True)
    return ok


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--firmware", default="", help="firmware.bin to run (default: build/qemu/fw)")
    parser.add_argument("--timeout", type=float, default=12.0, help="seconds to wait for a reaction")
    parser.add_argument("--boot-timeout", type=float, default=90.0)
    parser.add_argument("--fresh-sd", action="store_true",
                        help="obsolete: the card is always rebuilt (kept for old callers)")
    parser.add_argument("--fresh-flash", action="store_true",
                        help="obsolete: the flash is clean by default (kept for old callers)")
    parser.add_argument("--reuse-state", action="store_true",
                        help="keep the flash and card already in build/qemu/q5/run")
    parser.add_argument("--no-boot-probe", action="store_true",
                        help="skip the 'key pressed during boot' run")
    args = parser.parse_args(argv)

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    runner = Runner(args)
    try:
        runner.start()
        runner.run()
        if not args.no_boot_probe:
            runner.boot_probe()
    finally:
        runner.stop()
    report = runner.write_report()
    ok = check_rotation() and not runner.failures
    taps = report["tap_reaction_ms"]
    print(f"\nreport: build/qemu/q5/report.md, build/qemu/q5/report.json")
    print(f"tap reaction: {taps['min']}-{taps['max']} ms; failures: {len(runner.failures)}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
