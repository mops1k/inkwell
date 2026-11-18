#!/usr/bin/env python3
"""Q7 check: the reader screen really shows the page, not a black frame.

The reader paints its page in two passes: first the 1-bit BW picture, then a
4-level grayscale pass that only writes the anti-aliasing planes (LSB -> DTM1,
MSB -> DTM2) and refreshes again. Rendering that last record through the RAM
planes shows the gray *code*, which looks like a black page -- that was Q7. The
fix is in the panel model (patch 0025): every frame record also carries the
composed screen, the picture the panel displays.

This script drives the emulator the way a user does (library -> book card ->
reader), waits for the reader page, and checks the composed screen:

  * ink share (dark + black) inside a sane range -- a text page is a few percent,
    a black frame is ~100%, an empty page is 0%,
  * enough rows carry a plausible amount of ink, and those rows are made of many
    narrow runs (glyphs): a cover, a book card or an error screen does not look
    like that, so the check cannot pass on the wrong screen,
  * the frame is different from the library frame it followed.

Artifacts in build/qemu/q7/: reader-page.png (composed, 4 shades),
reader-raw-dtm2.png (the raw RAM plane, for the "before" comparison),
reader-library.png, boot.log, uart.log, reader-screen.txt.

Usage:
  tools/qemu/reader-screen.py                       # firmware from build/qemu/fw
  tools/qemu/reader-screen.py --firmware p/fw.bin --book "Личный враг"
  tools/qemu/reader-screen.py --check-dump build/qemu/ui/frame.bin   # offline check
Exit: 0 = OK (page on screen), 1 = FAIL (reason on the last line).
"""

from __future__ import annotations

import argparse
import hashlib
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import x3emu  # noqa: E402

OUT = x3emu.ROOT / "build/qemu/q7"

# A text page: mostly paper, a few percent of ink. A black frame (the Q7 bug) is
# ~100% ink, a blank page has none. Measured on the real FB2 books: 6-9% ink,
# ~410 rows with 2%+ ink, and ~11 separate ink runs per inked row (one per
# glyph). An error page ("Ошибка: общая ошибка") is 1.4% ink, a book card is
# 14% ink but solid blocks, i.e. ~4 runs per row: both are rejected by these
# numbers, so the check cannot silently pass on the wrong screen.
INK_MIN_PCT = 2.5
INK_MAX_PCT = 45.0
PAPER_MIN_PCT = 55.0
ROW_INK = 0.02            # a row counts as a text row from 2% ink
MIN_TEXT_ROWS = 250       # real page: ~410, an error page: ~119, a card: ~250
MIN_RUNS = 7.0            # glyphs per row; a card/error screen has ~4
DEFAULT_BOOK = "fb2"


def sig(frame) -> str:
    return hashlib.md5(frame.plane(1)).hexdigest()[:8]


def text_rows(levels: list[bytes], width: int) -> list[float]:
    """Per-row ink share of a composed screen (0..1 per row)."""
    shares = []
    for row in levels:
        ink = 0
        for value in row:
            if value >= 2:      # dark gray and black are ink
                ink += 1
        shares.append(ink / width if width else 0.0)
    return shares


def row_runs(levels: list[bytes]) -> tuple[int, float]:
    """(inked rows, mean number of separate ink runs per inked row).

    Text lines are made of many narrow runs (glyphs); a cover, a status bar or a
    button row is one or two wide runs, so this separates a page from a card.
    """
    inked = 0
    runs = 0
    for row in levels:
        count = 0
        prev = False
        for value in row:
            ink = value >= 2
            if ink and not prev:
                count += 1
            prev = ink
        if count:
            inked += 1
            runs += count
    return inked, (runs / inked) if inked else 0.0


def check_levels(levels: list[bytes], width: int, height: int) -> tuple[bool, str, dict]:
    """Does a composed screen look like a page of text?"""
    shares = text_rows(levels, width)
    stats = x3emu.panel_frame.level_stats(levels)
    rows_with_text = sum(1 for share in shares if ROW_INK <= share <= 0.6)
    inked_rows, runs = row_runs(levels)
    fields = {
        "ink_pct": round(stats["ink_pct"], 3),
        "paper_pct": round(stats["paper_pct"], 3),
        "gray_pct": round(stats["gray_pct"], 3),
        "black_pct": round(stats["black_pct"], 3),
        "text_rows": rows_with_text,
        "rows_with_ink": inked_rows,
        "ink_runs_per_row": round(runs, 2),
    }
    if stats["ink_pct"] < INK_MIN_PCT:
        return False, (f"reader frame has only {stats['ink_pct']:.2f}% ink: not a page "
                       f"of text (an error/splash screen)"), fields
    if stats["ink_pct"] > INK_MAX_PCT:
        return False, (f"reader frame is {stats['ink_pct']:.1f}% ink: looks like the "
                       f"black Q7 frame, not a page"), fields
    if stats["paper_pct"] < PAPER_MIN_PCT:
        return False, f"reader frame is only {stats['paper_pct']:.1f}% paper", fields
    if rows_with_text < MIN_TEXT_ROWS:
        return False, (f"no page of text on the reader frame "
                       f"({rows_with_text} rows with 2%+ ink, need {MIN_TEXT_ROWS})"), fields
    if runs < MIN_RUNS:
        return False, (f"no text lines on the reader frame ({runs:.1f} ink runs per "
                       f"row, a text line has ~{MIN_RUNS:.0f}+): this is a card or an "
                       f"error screen, not a book page"), fields
    return True, "reader page drawn", fields


def print_frame_report(label: str, frame) -> None:
    st = x3emu.panel_frame.stats(frame.plane(1), frame.width, frame.height, "DTM2")
    extra = "4-level gray" if frame.gray4 else "1-bit BW"
    print(f"  {label}: md5={sig(frame)} refresh={extra} planes={frame.planes} "
          f"dtm2_black={st['black_pct']:.1f}%", flush=True)


def finish(fields: dict, result: str, reason: str, pngs: list[Path]) -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    lines = [f"# Q7 reader screen — {time.strftime('%Y-%m-%d %H:%M:%S')}", ""]
    for key, value in fields.items():
        lines.append(f"{key}: {value}")
    lines.append(f"png: {', '.join(str(p.relative_to(x3emu.ROOT)) for p in pngs)}")
    lines.append(f"result: {result}" + (f" ({reason})" if reason else ""))
    (OUT / "reader-screen.txt").write_text("\n".join(lines) + "\n")
    print(f"READER SCREEN {result}: {reason}" if reason else f"READER SCREEN {result}")
    return 0 if result == "OK" else 1


# --------------------------------------------------------------------------- #
# Offline: check an existing frame dump (used for regression and to show the
# pre-patch failure).
# --------------------------------------------------------------------------- #

def check_dump(path: Path, index: int) -> int:
    frames = x3emu.panel_frame.read_frames(str(path))
    if not frames:
        print(f"READER SCREEN FAIL: {path} holds no frame record")
        return 1
    frame = frames[index]
    print(f"{path}: {len(frames)} record(s), v{frame.version}, planes {frame.planes}, "
          f"checking record {frames.index(frame)}")
    pngs = []
    levels = x3emu.panel_frame.levels_rows(frame)
    if levels is None:
        # Pre-Q7 record: the only available view is the raw RAM plane.
        print("  record has no composed screen (pre-Q7 model): checking the RAM plane")
        plane = frame.plane(1)
        rows = frame.rows_logical(plane)
        stats = x3emu.panel_frame.stats(plane, frame.width, frame.height, "DTM2")
        ink_pct = 100.0 - stats["white"] * 100.0 / stats["pixels"]
        ok = INK_MIN_PCT <= ink_pct <= INK_MAX_PCT
        png = OUT / "reader-raw-dtm2.png"
        png.write_bytes(x3emu.panel_frame.png_bytes(frame.width, frame.height, rows))
        print(f"  raw DTM2 ink {ink_pct:.1f}%")
        reason = ("raw RAM plane has no readable page (the Q7 black frame)"
                  if not ok else "raw RAM plane looks like a page")
        return finish({"mode": "pre-q7 raw RAM plane", "ink_pct": round(ink_pct, 2)},
                      "OK" if ok else "FAIL", reason, [png])
    ok, reason, fields = check_levels(levels, frame.width, frame.height)
    for key, value in fields.items():
        print(f"  {key}: {value}")
    width, height, upright = x3emu.levels_upright(frame)
    (OUT / "reader-page.png").write_bytes(
        x3emu.panel_frame.png_gray_bytes(width, height, upright))
    pngs.append(OUT / "reader-page.png")
    return finish(fields, "OK" if ok else "FAIL", reason, pngs)


# --------------------------------------------------------------------------- #
# Live: drive the emulator to the reader and check the frame it paints.
# --------------------------------------------------------------------------- #

def pick_card(arg: str | None) -> Path:
    """A card image that already has the library index, or a fresh one."""
    if arg:
        return Path(arg).expanduser().resolve()
    for candidate in (x3emu.ROOT / "build/qemu/ui/sd.img",
                      x3emu.ROOT / "build/qemu/nav/sd.img"):
        if candidate.is_file():
            try:
                if x3emu.read_card(candidate).get("books"):
                    print(f"card: reusing {candidate.relative_to(x3emu.ROOT)} "
                          f"(library index already on it)")
                    return candidate
            except (OSError, x3emu.sd_files.FatError):
                pass
    fresh = OUT / "sd.img"
    print(f"card: building {fresh.relative_to(x3emu.ROOT)} (first scan can take minutes)")
    return x3emu.ensure_sd(fresh, force=True)


def book_index(emu, book: str) -> tuple[int, int]:
    """(position, count) of the shelf: the position is the index in books.json."""
    deadline = time.monotonic() + 300
    books: list[dict] = []
    while time.monotonic() < deadline and emu.running:
        index = x3emu.read_card(emu.sd).get("books") or {}
        books = (index.get("books") or []) if isinstance(index, dict) else []
        if books:
            break
        emu.poll_frames()
        time.sleep(2)
    if not books:
        raise x3emu.EmulatorError("library index never appeared on the card")
    for i, item in enumerate(books):
        haystack = f"{item.get('path', '')} {item.get('title', '')}".lower()
        if book.lower() in haystack:
            print(f"book: #{i} {item.get('title') or item.get('path')} "
                  f"({len(books)} on the shelf)")
            return i, len(books)
    raise x3emu.EmulatorError(f"no book matching {book!r} in the index "
                              f"({[b.get('path') for b in books]})")


def wait_changed(emu, before_count: int, before_sig: str, timeout: float):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and emu.running:
        emu.poll_frames()
        for candidate in emu.frames.frames[before_count:]:
            if sig(candidate) != before_sig:
                return candidate
        time.sleep(0.01)
    return None


def tap_and_wait(emu, button: str, timeout: float) -> tuple[object | None, float]:
    """Tap a button and wait for a frame whose content differs from the previous.

    The guest repaints ~150 ms after the release but its idle slot can be busy
    with a cover decode, so waiting for "no new frame for a while" is not
    enough: the next key would land while the previous repaint is still pending.
    """
    before_count = len(emu.frames.frames)
    before_sig = sig(emu.frames.frames[-1])
    t0 = time.monotonic()
    emu.tap(button, 90)
    frame = wait_changed(emu, before_count, before_sig, timeout=timeout)
    return frame, (time.monotonic() - t0) * 1000


def recover_to_grid(emu, timeout: float) -> None:
    """Back out of the reader page and the book card, back to the shelf grid."""
    for _ in range(2):
        frame, _ms = tap_and_wait(emu, "back", timeout=min(timeout, 30.0))
        if frame is None:
            return
        emu.wait_idle(0.4, timeout=30)


def open_book(emu, args, fields: dict) -> object | None:
    """Confirm twice: shelf cell -> book card -> reader. Returns the reader frame."""
    frame = None
    for label in ("book card", "reader"):
        before_count = len(emu.frames.frames)
        before_sig = sig(emu.frames.frames[-1])
        t0 = time.monotonic()
        emu.tap("confirm", 90)
        frame = wait_changed(emu, before_count, before_sig, timeout=args.timeout)
        if frame is None:
            return None
        fields[f"{label.replace(' ', '_')}_ms"] = round((time.monotonic() - t0) * 1000)
        print_frame_report(label, frame)
        if label == "reader":
            emu.wait_idle(1.0, timeout=args.timeout)
    return frame


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--firmware", default="", help="firmware.bin to boot")
    ap.add_argument("--sd", default="", help="card image with a library index")
    ap.add_argument("--book", default=DEFAULT_BOOK,
                    help=f"substring of the book path/title to open (default: {DEFAULT_BOOK!r})")
    ap.add_argument("--check-dump", default="", metavar="FRAME.BIN",
                    help="do not emulate: check a record of an existing frame dump")
    ap.add_argument("--frame", type=int, default=-1,
                    help="record to check with --check-dump (-1 = the last one)")
    ap.add_argument("--timeout", type=float, default=240.0,
                    help="how long to wait for the reader page (default 240 s)")
    args = ap.parse_args()

    OUT.mkdir(parents=True, exist_ok=True)
    for stale in ("reader-page.png", "reader-raw-dtm2.png", "reader-library.png",
                  "reader-screen.txt"):
        (OUT / stale).unlink(missing_ok=True)

    if args.check_dump:
        return check_dump(Path(args.check_dump), args.frame)

    firmware = Path(args.firmware).expanduser() if args.firmware else None
    if firmware is None:
        default_fw = x3emu.ROOT / "build/qemu/fw/firmware.bin"
        if not default_fw.is_file():
            return finish({"firmware": "missing"}, "FAIL",
                          "no build/qemu/fw/firmware.bin: pass --firmware or run "
                          "`.venv/bin/pio run -e default`", [])
        firmware = default_fw
    print(f"firmware: {firmware.relative_to(x3emu.ROOT) if firmware.is_relative_to(x3emu.ROOT) else firmware}")

    card = pick_card(args.sd)
    private_card = OUT / "sd.img"
    if card.resolve() != private_card.resolve():
        # Work on a copy: the guest writes to the card (index, caches) and the
        # reused image usually belongs to another run (build/qemu/ui).
        private_card.write_bytes(card.read_bytes())
        card = private_card
    emu = x3emu.Emulator(firmware=firmware if args.firmware else None,
                         work_dir=OUT, fresh_flash=True, fresh_sd=False,
                         sd_image=card)
    fields: dict = {"firmware": str(firmware), "card": str(card)}
    pngs: list[Path] = []
    emu.start()
    try:
        boot_started = time.monotonic()
        emu.wait_frames(3, timeout=180)
        emu.wait_idle(0.8, timeout=180)
        fields["boot_seconds"] = round(time.monotonic() - boot_started, 2)
        library = emu.frames.frames[-1]
        print_frame_report("library", library)
        emu.frame_png(library, OUT / "reader-library.png")
        pngs.append(OUT / "reader-library.png")

        index, shelf_count = book_index(emu, args.book)
        fields["book_index"] = index
        _ = index

        # Shelf navigation is not deterministic enough for a single blind tap:
        # the first Right can land while the grid is still decoding covers and
        # gets folded into that repaint, so the Confirm then opens the first
        # book. Open a cell, check what came out, and if it is not a page (the
        # card was the wrong book) go back and shift the selection once more.
        # Bounded, and every attempt has to produce the page.
        attempts = max(1, min(3, shelf_count))
        reader = None
        check: dict = {}
        reason = ""
        for attempt in range(attempts):
            if attempt == 0:
                # Walk to the requested cell. Each Right is followed by a frame
                # wait, but the shelf can still fold a key into the cover-decode
                # repaint (it then looks like one screen), which is why the
                # attempts below exist.
                for step in range(index):
                    frame, ms = tap_and_wait(emu, "right", timeout=args.timeout)
                    if frame is None:
                        return finish(fields | {"step": "grid"}, "FAIL",
                                      f"the shelf did not repaint after Right "
                                      f"#{step + 1}", pngs)
                    print(f"  grid right {step + 1}: {sig(frame)} in {ms:.0f} ms",
                          flush=True)
                emu.wait_idle(0.5, timeout=60)
            else:
                recover_to_grid(emu, args.timeout)
                frame, _ms = tap_and_wait(emu, "right", timeout=args.timeout)
                if frame is None:
                    break
                emu.wait_idle(0.5, timeout=60)
            opened = open_book(emu, args, fields)
            if opened is None:
                return finish(fields | {"step": "reader"}, "FAIL",
                              f"no panel refresh after opening the book "
                              f"(waited {args.timeout:.0f} s)", pngs)
            reader = emu.frames.frames[-1]
            print_frame_report(f"reader attempt {attempt + 1}", reader)
            if not reader.has_screen:
                return finish(fields | {"step": "reader"}, "FAIL",
                              "frame record has no composed screen planes: the panel "
                              "model is the pre-Q7 one (rebuild QEMU with patches "
                              "0001..0025)", pngs)
            ok, reason, check = check_levels(
                x3emu.panel_frame.levels_rows(reader), reader.width, reader.height)
            if ok:
                break
            print(f"  attempt {attempt + 1}: {reason}", flush=True)

        assert reader is not None
        fields.update(check)
        fields["reader_refresh"] = "4-level gray" if reader.gray4 else "1-bit BW"
        fields["reader_md5"] = sig(reader)
        fields["library_md5"] = sig(library)

        emu.frame_png(reader, OUT / "reader-page.png")
        pngs.append(OUT / "reader-page.png")
        emu.frame_png(reader, OUT / "reader-raw-dtm2.png", plane="dtm2")
        pngs.append(OUT / "reader-raw-dtm2.png")
        for key, value in check.items():
            print(f"  {key}: {value}")

        if fields["reader_md5"] == fields["library_md5"]:
            return finish(fields, "FAIL",
                          "reader frame equals the library frame: the reader never "
                          "painted", pngs)
        fields["panics"] = emu.panic_count()
        (OUT / "boot.log").write_text(emu.qemu_text())
        (OUT / "uart.log").write_text(emu.uart_text())
        return finish(fields, "OK" if ok else "FAIL", reason if not ok else "", pngs)
    finally:
        emu.stop()


if __name__ == "__main__":
    sys.exit(main())
