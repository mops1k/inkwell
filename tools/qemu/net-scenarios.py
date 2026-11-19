#!/usr/bin/env python3
"""net-scenarios.py — the network screens must not reboot the emulator.

Boots the real firmware in QEMU, walks the physical keyboard into the two
places that start the WiFi radio (Settings -> System -> WiFi Networks and
File Transfer -> Create Hotspot) and checks that the chip keeps running:

    wifi      after opening "WiFi Networks" the screen changes to the error
              state (no radio on the emulated board), the chip does not reset,
              no panic is printed, and Back leaves the screen (it is not hung);
    hotspot   "Create Hotspot" returns to the library instead of starting an
              access point, again without a reset or a panic.

Why this exists: the QEMU esp32c3 model has no WiFi MAC/PHY, and a real
esp_wifi_init() aborts in esp_phy_enable() (phy_init.c:310, the SYSCON modem
clock gates read 0) and reboots the chip with rst:0xc RTC_SW_CPU_RST — the
"network functions reboot the emulator" symptom. src/network/WifiRadioGuard.cpp
interposes esp_wifi_init() for the dev build and returns an error instead, so
these screens stay alive. This script is the regression check for that.

    tools/qemu/net-scenarios.py                 # both scenarios, ~60 s
    tools/qemu/net-scenarios.py --scenario wifi
    tools/qemu/net-scenarios.py --firmware path/to/firmware.bin --keep

Artifacts: build/qemu/net/{frame-*.png,uart.log,boot.log,report.json,report.md}
Exit: 0 = all checks passed, 1 = at least one check failed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import x3emu  # noqa: E402

OUT_DIR = x3emu.ROOT / "build/qemu/net"
MIN_DARK_PIXELS = 500          # the library screen, same threshold as smoke.sh
PANIC_MARKERS = ("Guru Meditation", "Interrupt wdt timeout", "assert failed")


def sig(frame) -> str:
    return hashlib.md5(frame.plane(1)).hexdigest()[:8]


class Runner:
    def __init__(self, args):
        self.args = args
        self.emu: x3emu.Emulator | None = None
        self.failures: list[str] = []
        self.checks: list[dict] = []
        self.card: Path | None = None
        self.boot_base = 0
        self.last_frame = None
        self.last_sig = ""

    # -- setup -------------------------------------------------------------- #
    def start(self) -> None:
        OUT_DIR.mkdir(parents=True, exist_ok=True)
        card = OUT_DIR / "sd.img"
        if not self.args.reuse_state or not card.is_file():
            # A plain card (no build/qemu/sd-books): the library index is one
            # text file, so the shelf appears in a couple of seconds and the
            # scenario does not depend on how fast FB2 covers get parsed.
            import subprocess
            proc = subprocess.run(
                [sys.executable or "python3", str(x3emu.ROOT / "tools/qemu/make-sd-image.py"),
                 "--out", str(card)], cwd=x3emu.ROOT, capture_output=True, text=True)
            if proc.returncode != 0:
                raise x3emu.EmulatorError(f"make-sd-image.py failed:\n{proc.stdout}{proc.stderr}")
        self.card = card
        self.spawn(fresh_flash=not self.args.reuse_state)

    def spawn(self, fresh_flash: bool = False) -> None:
        """Start a QEMU run and wait until the shelf is drawn and stable.

        Every scenario gets its own run: they end in different screens, and
        walking back out of Settings to the library between them is exactly the
        kind of fragile input choreography this check should not depend on.
        """
        if self.emu is not None:
            self.emu.stop()
        self.emu = x3emu.Emulator(
            firmware=Path(self.args.firmware) if self.args.firmware else None,
            fw_dir=x3emu.ROOT / "build/qemu/fw",
            work_dir=OUT_DIR, fresh_flash=fresh_flash,
            fresh_sd=False, sd_image=self.card)
        self.emu.start()
        self.wait_library()
        # The POWERON + DSLEEP pair the firmware itself performs on a cold start
        # is not a reset; count reboots from here.
        self.boot_base = self.uart().count("ESP-ROM:esp32c3")

    def next_scenario(self) -> None:
        self.save(self.last_frame, "frame-library.png")
        self.spawn(fresh_flash=False)

    # -- observations ------------------------------------------------------- #
    def uart(self) -> str:
        return self.emu.uart_text()

    def panics(self) -> int:
        text = self.uart()
        return sum(text.count(marker) for marker in PANIC_MARKERS)

    def reboots(self) -> int:
        return max(0, self.uart().count("ESP-ROM:esp32c3") - self.boot_base)

    def dark(self, frame) -> int:
        try:
            return self.emu.frame_stats(frame)["black"]
        except Exception:  # noqa: BLE001 - diagnostics only
            return -1

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        self.checks.append({"check": name, "ok": ok, "detail": detail})
        print(f"  {'ok  ' if ok else 'FAIL'} {name}" + (f" — {detail}" if detail else ""), flush=True)
        if not ok:
            self.failures.append(f"{name}: {detail}" if detail else name)
        return ok

    # -- waiting ------------------------------------------------------------ #
    def poll(self) -> None:
        self.emu.poll_frames()

    def wait_frame_change(self, before: int, current: str, timeout: float) -> object | None:
        """First frame after index ``before`` whose signature differs from ``current``."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.poll()
            for frame in self.emu.frames.frames[before:]:
                if sig(frame) != current:
                    return frame
            if not self.emu.running:
                return None
            time.sleep(0.01)
        return None

    def press(self, button: str, hold_ms: float = 90) -> None:
        self.emu.tap(button, hold_ms)
        time.sleep(0.1)

    def step(self, label: str, button: str, timeout: float = 6.0) -> bool:
        current = self.last_sig
        before = len(self.emu.frames.frames)
        self.press(button)
        frame = self.wait_frame_change(before, current, timeout)
        if frame is None:
            # One retry: a press that lands while the panel is refreshing is lost.
            before = len(self.emu.frames.frames)
            self.press(button, 120)
            frame = self.wait_frame_change(before, current, timeout)
        if frame is None:
            self.save(self.last_frame, f"frame-fail-{label.replace(' ', '-').replace(':', '')}.png")
            self.check(f"{label}: {button} redraws the screen", False,
                       f"no new frame within {timeout:.0f} s")
            return False
        self.last_sig = sig(frame)
        self.last_frame = frame
        return True

    def wait_library(self, timeout: float = 120.0) -> None:
        """Wait for the drawn shelf: a dark, stable frame and a stable boot count."""
        deadline = time.monotonic() + timeout
        quiet_since = None
        last_frames = -1
        while time.monotonic() < deadline:
            self.poll()
            frames = len(self.emu.frames.frames)
            if frames >= 3 and self.dark(self.emu.frames.frames[-1]) >= MIN_DARK_PIXELS:
                if frames != last_frames:
                    last_frames = frames
                    quiet_since = time.monotonic()
                elif time.monotonic() - quiet_since >= 1.0:
                    self.last_frame = self.emu.frames.frames[-1]
                    self.last_sig = sig(self.last_frame)
                    return
            if not self.emu.running:
                raise x3emu.EmulatorError("QEMU exited during boot")
            time.sleep(0.05)
        raise x3emu.EmulatorError("the library screen did not settle")

    def settle(self, seconds: float) -> None:
        """Let the guest run, then report what happened meanwhile."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.poll()
            if not self.emu.running:
                break
            time.sleep(0.1)

    def save(self, frame, name: str) -> None:
        if frame is not None:
            try:
                self.emu.frame_png(frame, OUT_DIR / name, plane="screen")
            except Exception:  # noqa: BLE001 - diagnostics only
                pass

    # -- navigation --------------------------------------------------------- #
    def to_system_settings(self) -> bool:
        if not self.step("main menu", "back"):
            return False
        for i in range(4):                      # Files..Transfer -> Settings
            if not self.step(f"menu down {i + 1}", "down"):
                return False
        if not self.step("open settings", "confirm"):
            return False
        for i in range(3):                      # Display -> Reader -> Controls -> System
            if not self.step(f"tab {i + 2}", "confirm"):
                return False
        for i in range(3):                      # File visibility, Language, ... -> WiFi
            if not self.step(f"settings down {i + 1}", "down"):
                return False
        return True

    def to_file_transfer(self) -> bool:
        if not self.step("main menu", "back"):
            return False
        for i in range(3):                      # Files, Recents, OPDS -> Transfer
            if not self.step(f"menu down {i + 1}", "down"):
                return False
        return self.step("open file transfer", "confirm")

    # -- scenarios ---------------------------------------------------------- #
    def scenario_wifi(self) -> None:
        print("scenario wifi: Settings -> System -> WiFi Networks", flush=True)
        if not self.to_system_settings():
            return
        self.save(self.last_frame, "frame-settings-system.png")
        current = self.last_sig
        before = len(self.emu.frames.frames)
        self.press("confirm")                   # WifiSelectionActivity: radio init
        frame = self.wait_frame_change(before, current, 20)
        reboots_before = self.reboots()
        panics_before = self.panics()
        self.settle(6.0)                        # would have rebooted by now
        reboots = self.reboots() - reboots_before
        panics = self.panics() - panics_before
        self.check("wifi: the screen changes when the radio is requested", frame is not None,
                   "error state drawn" if frame is not None else "no new frame in 20 s")
        if frame is not None:
            self.last_sig = sig(frame)
            self.last_frame = frame
            self.save(frame, "frame-wifi-error.png")
            dark = self.dark(frame)
            self.check("wifi: the error frame has content", dark >= MIN_DARK_PIXELS,
                       f"{dark} dark pixels")
        self.check("wifi: no chip reset after opening the screen", reboots == 0, f"{reboots} reset(s)")
        self.check("wifi: no panic after opening the screen", panics == 0, f"{panics} panic line(s)")
        self.check("wifi: QEMU still running", self.emu.running, "guest alive")
        if frame is not None:
            if self.step("wifi: Back leaves the screen", "back", 8):
                self.save(self.last_frame, "frame-back-in-settings.png")
            self.check("wifi: no reset after Back", self.reboots() - reboots_before == 0,
                       f"{self.reboots() - reboots_before} reset(s)")

    def scenario_hotspot(self) -> None:
        print("scenario hotspot: File Transfer -> Create Hotspot", flush=True)
        if not self.to_file_transfer():
            return
        # NetworkModeSelection: Join, Calibre, Hotspot, USB
        for i in range(2):
            if not self.step(f"mode down {i + 1}", "down"):
                return
        self.save(self.last_frame, "frame-network-modes.png")
        current = self.last_sig
        before = len(self.emu.frames.frames)
        reboots_before = self.reboots()
        panics_before = self.panics()
        self.press("confirm")
        frame = self.wait_frame_change(before, current, 20)
        self.settle(6.0)
        reboots = self.reboots() - reboots_before
        panics = self.panics() - panics_before
        self.check("hotspot: the screen reacts to Create Hotspot", frame is not None,
                   "returned to the library" if frame is not None else "no new frame in 20 s")
        if frame is not None:
            self.last_sig = sig(frame)
            self.last_frame = frame
            self.save(frame, "frame-hotspot-after.png")
        self.check("hotspot: no chip reset", reboots == 0, f"{reboots} reset(s)")
        self.check("hotspot: no panic", panics == 0, f"{panics} panic line(s)")

    # -- report ------------------------------------------------------------- #
    def write_report(self, firmware: Path) -> None:
        result = "OK" if not self.failures else "FAIL"
        report = {
            "firmware": str(firmware),
            "firmware_md5": x3emu.md5_of(firmware)[:12] if firmware.is_file() else "",
            "result": result,
            "failures": self.failures,
            "checks": self.checks,
            "frames": len(self.emu.frames.frames) if self.emu else 0,
            "reboots": self.reboots(),
        }
        (OUT_DIR / "report.json").write_text(json.dumps(report, indent=2))
        lines = [f"# QEMU network screens — {time.strftime('%Y-%m-%d %H:%M:%S')}", "",
                 f"firmware: `{firmware}` md5={report['firmware_md5']}", "",
                 "| check | result | detail |", "|---|---|---|"]
        for c in self.checks:
            lines.append(f"| {c['check']} | {'ok' if c['ok'] else 'FAIL'} | {c['detail']} |")
        lines += ["", f"result: {result}" + (f" ({'; '.join(self.failures)})" if self.failures else "")]
        (OUT_DIR / "report.md").write_text("\n".join(lines) + "\n")
        if self.emu:
            (OUT_DIR / "uart.log").write_text(self.uart())
            (OUT_DIR / "boot.log").write_text(self.emu.qemu_text())
        print(f"NET {'OK' if result == 'OK' else 'FAIL'}: frames={report['frames']} "
              f"reboots={report['reboots']} checks={len(self.checks)} "
              f"failures={len(self.failures)}", flush=True)
        if self.failures:
            print("  " + "\n  ".join(self.failures), flush=True)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", choices=("wifi", "hotspot", "all"), default="all")
    ap.add_argument("--firmware", default="", help="default: build/qemu/fw/firmware.bin")
    ap.add_argument("--reuse-state", action="store_true",
                    help="keep the flash/card of the previous run")
    ap.add_argument("--keep", action="store_true", help="leave QEMU running")
    args = ap.parse_args()

    firmware = Path(args.firmware) if args.firmware else x3emu.ROOT / "build/qemu/fw/firmware.bin"
    if not firmware.is_file():
        print(f"NET FAIL: no firmware at {firmware}")
        return 1

    runner = Runner(args)
    try:
        runner.start()
        scenarios = ["wifi", "hotspot"] if args.scenario == "all" else [args.scenario]
        for index, name in enumerate(scenarios):
            if index:
                runner.next_scenario()
            getattr(runner, f"scenario_{name}")()
        runner.write_report(firmware)
        return 0 if not runner.failures else 1
    finally:
        if args.keep:
            print(f"QEMU left running (pid {runner.emu.proc.pid if runner.emu and runner.emu.proc else '?'})")
        elif runner.emu is not None:
            runner.emu.stop()


if __name__ == "__main__":
    sys.exit(main())
