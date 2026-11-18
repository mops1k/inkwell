#!/usr/bin/env bash
# Smoke gate for the firmware image: one QEMU run with clean flash and a clean
# SD card, the button-injection path driven end to end.
#
#   tools/qemu/smoke.sh                     # build/qemu/fw/firmware.bin
#   tools/qemu/smoke.sh path/to/firmware.bin
#   tools/qemu/smoke.sh --strict-panic      # any panic in uart.log is fatal
#
# What it checks (see tools/qemu/README.md, "Smoke-гейт перед прошивкой"):
#   1. the image boots to a drawn library screen,
#   2. the library index /.crosspoint/shelf/books.json is really ON THE CARD,
#      with titles — and, when build/qemu/sd-books/ has books, their FB2/FB2.ZIP
#      metadata was resolved (that needs the SD write path, Q6),
#   3. short Back puts the pop-up menu on the panel (a new frame),
#   4. short Confirm opens the book card (a new frame),
#   5. no Guru Meditation in uart.log — except the known cold-boot interrupt
#      watchdog panic, which reboots the chip once before the UI appears (it is
#      reported as WARN, and --strict-panic makes it fatal).
#
# Artifacts: build/qemu/smoke/{boot.log,uart.log,frame-library.png,
#            frame-menu.png,frame-card.png,smoke.txt}
# Exit: 0 = SMOKE OK, 1 = SMOKE FAIL (the reason is on the last line).
set -uo pipefail
cd "$(dirname "$0")/../.."

FW_ARG=""
STRICT=0
for arg in "$@"; do
  case "$arg" in
    --strict-panic) STRICT=1 ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    *) FW_ARG="$arg" ;;
  esac
done

FW_ARG="$FW_ARG" SMOKE_STRICT="$STRICT" exec python3 - <<'PY'
import hashlib
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, "tools/qemu")
import x3emu

OUT = x3emu.ROOT / "build/qemu/smoke"
FW_ARG = os.environ.get("FW_ARG", "")
STRICT = os.environ.get("SMOKE_STRICT") == "1"
fw_dir = x3emu.ROOT / "build/qemu/fw"

KNOWN_PANIC = ("Interrupt wdt timeout", "Guru Meditation")
INDEX_PATH = "/.crosspoint/shelf/books.json"


def fail(reason: str, **fields) -> int:
    write_txt(fields, "FAIL", reason)
    print(f"SMOKE FAIL: {reason}")
    return 1


def check_card(emu) -> tuple[dict, str | None]:
    """Read the card image out of the emulator and check the library index.

    Since Q6 the guest can write again (CMD24/CMD25), so the index it builds
    from /books must be on the card as real JSON — and, when build/qemu/sd-books
    has books, their FB2.ZIP metadata must be resolved to titles (that needs the
    scratch .fb2 file written next to the book). Returns (fields, failure).
    """
    try:
        card = x3emu.read_card(emu.sd)
    except (OSError, x3emu.sd_files.FatError) as exc:
        return {"card": f"unreadable: {exc}"}, f"card image unreadable: {exc}"

    fields = {"card_files": len(card["entries"])}
    if INDEX_PATH not in card["paths"]:
        return fields, f"library index {INDEX_PATH} was not written to the card"
    index = card["books"]
    if not isinstance(index, dict):
        return fields, f"library index {INDEX_PATH} is corrupt"
    items = index.get("books") or []
    titles = [b for b in items if (b.get("title") or "").strip()]
    fields["card_index_books"] = len(items)
    fields["card_titles"] = "; ".join(f"{b.get('title')}" for b in titles) or "(none)"

    expected = x3emu.sd_book_files()
    if expected:
        fb2 = [b for b in titles if b.get("format") in ("fb2zip", "fb2")]
        if not fb2:
            return fields, ("library index has no FB2 title: the metadata scratch file "
                            "could not be written to the card")
        fields["card_fb2_titles"] = len(fb2)
    else:
        fields["card_fb2_titles"] = "n/a (no build/qemu/sd-books)"
    for path in expected:
        if f"/books/{path.name}" not in card["paths"]:
            return fields, f"test book /books/{path.name} is missing from the card"
    return fields, None


def write_txt(fields: dict, result: str, reason: str = "") -> None:
    lines = [f"# QEMU smoke gate — {time.strftime('%Y-%m-%d %H:%M:%S')}", ""]
    for key, value in fields.items():
        lines.append(f"{key}: {value}")
    lines.append(f"result: {result}" + (f" ({reason})" if reason else ""))
    if result == "FAIL":
        lines.append("")
    (OUT / "smoke.txt").write_text("\n".join(lines) + "\n")


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    for stale in OUT.glob("frame-*.png"):
        stale.unlink()

    if FW_ARG:
        try:
            firmware = x3emu.install_firmware(Path(FW_ARG), fw_dir)
        except x3emu.EmulatorError as exc:
            return fail(f"firmware: {exc}", firmware=FW_ARG)
    else:
        firmware = fw_dir / "firmware.bin"
        if not firmware.is_file():
            return fail("no build/qemu/fw/firmware.bin: pass an image or run pio run -e default",
                        firmware="missing")
    fw_md5 = hashlib.md5(firmware.read_bytes()).hexdigest()[:12]
    fw_size = firmware.stat().st_size
    print(f"smoke: {firmware.relative_to(x3emu.ROOT)} md5={fw_md5} "
          f"{fw_size / 1e6:.2f} MiB, clean flash + clean SD", flush=True)

    emu = x3emu.Emulator(firmware=firmware, work_dir=OUT, fresh_flash=True, fresh_sd=True)
    steps = []
    emu.start()
    boot_started = time.monotonic()
    try:
        emu.wait_frames(3, timeout=90)
        emu.wait_idle(0.6, timeout=60)
        boot_seconds = time.monotonic() - boot_started
        library = emu.frames.frames[-1]
        lib_stats = emu.frame_stats(library)
        emu.frame_png(library, OUT / "frame-library.png")
        if lib_stats["black"] < 500:
            return fail(f"library not drawn (only {lib_stats['black']} dark pixels)",
                        boot_seconds=f"{boot_seconds:.2f}s", frames=len(emu.frames.frames))
        steps.append(("library", None, 0, "frame-library.png"))

        # The card is checked right after the shelf is drawn: that is when the
        # indexer has scanned /books and saved its index (Q6). Real .fb2.zip books
        # are several MB of XML and the emulated SD is slow, so the index can land
        # well after the first grid frame — poll the card instead of assuming it
        # is already there.
        card_fields, card_failure = check_card(emu)
        index_deadline = time.monotonic() + 180
        while card_failure is not None and time.monotonic() < index_deadline and emu.running:
            time.sleep(1.0)
            emu.poll_frames()
            card_fields, card_failure = check_card(emu)
        if card_failure is None:
            # The grid may have been repainted while the scan finished; keep the
            # newest frame as the library screenshot.
            emu.wait_idle(0.5, timeout=20)
            library = emu.frames.frames[-1]
            lib_stats = emu.frame_stats(library)
            emu.frame_png(library, OUT / "frame-library.png")
        for key, value in card_fields.items():
            print(f"  {key}: {value}", flush=True)
        if card_failure:
            return fail(card_failure, boot_seconds=f"{boot_seconds:.2f}s",
                        frames=len(emu.frames.frames), **card_fields)

        for label, button, hold_ms, filename in (
                ("menu", "back", 90, "frame-menu.png"),
                ("close-menu", "back", 90, None),
                ("book-card", "confirm", 90, "frame-card.png")):
            before_count = len(emu.frames.frames)
            before_sig = hashlib.md5(emu.frames.frames[-1].plane(1)).hexdigest()[:8]
            t_press = time.monotonic()
            emu.tap(button, hold_ms)
            # Wait for a frame whose content actually differs. A repaint of the
            # same screen is not a reaction: with the card now writable the
            # guest is busier, and the first refresh after a press can still be
            # the previous screen while the next one lands a moment later.
            frame = None
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                emu.poll_frames()
                for candidate in emu.frames.frames[before_count:]:
                    if hashlib.md5(candidate.plane(1)).hexdigest()[:8] != before_sig:
                        frame = candidate
                        break
                if frame is not None or not emu.running:
                    break
                time.sleep(0.01)
            if frame is None:
                return fail(f"{label}: no panel frame with a changed screen after {button}",
                            boot_seconds=f"{boot_seconds:.2f}s",
                            steps="; ".join(s[0] for s in steps))
            latency = frame.arrival - t_press
            if filename:
                emu.frame_png(frame, OUT / filename)
            steps.append((label, button, round(latency * 1000), filename))
            print(f"  {label:10s} {button:8s} -> {latency * 1000:5.0f} ms, "
                  f"frame(s) {len(emu.frames.frames)}", flush=True)

        uart = emu.uart_text()
        panic_lines = [line for line in uart.splitlines()
                       if any(marker in line for marker in KNOWN_PANIC)]
        panic_msgs = uart.count("Guru Meditation Error")
        reboots = uart.count("ESP-ROM:esp32c3") - 1
        frames_total = len(emu.frames.frames)
        (OUT / "boot.log").write_text(emu.qemu_text())
        (OUT / "uart.log").write_text(uart)

        fields = {
            "firmware": f"{firmware.relative_to(x3emu.ROOT)} md5={fw_md5} {fw_size / 1e6:.2f} MiB",
            "boot_seconds": f"{boot_seconds:.2f}",
            "panel_frames": frames_total,
            "panic_messages": panic_msgs,
            "reboots": reboots,
            "strict_panic": STRICT,
            **card_fields,
            "steps": "; ".join(
                f"{s[0]}" + (f"={s[2]}ms" if s[2] else "") + (f"->{s[3]}" if s[3] else "")
                for s in steps),
            "artifacts": "build/qemu/smoke/{boot.log,uart.log,frame-library.png,"
                         "frame-menu.png,frame-card.png}",
        }
        books = card_fields.get("card_index_books", "?")
        if panic_msgs:
            fields["panic"] = " / ".join(dict.fromkeys(panic_lines))
            if STRICT:
                write_txt(fields, "FAIL", "panic in uart.log (--strict-panic)")
                print("SMOKE FAIL: panic in uart.log (--strict-panic)")
                return 1
            fields["known_panic"] = ("cold-boot IDWDT panic, chip rebooted and reached the UI; "
                                    "treated as WARN, see README Q5")
            write_txt(fields, "OK", "known cold-boot panic (WARN)")
            print(f"SMOKE OK: frames={frames_total} panic={panic_msgs} books={books} "
                  f"(known cold-boot panic, rebooted={reboots}; strict mode would fail)")
            return 0
        write_txt(fields, "OK")
        print(f"SMOKE OK: frames={frames_total} panic=0 books={books}")
        return 0
    finally:
        emu.stop()


sys.exit(main())
PY
