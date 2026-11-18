#!/usr/bin/env python3
"""x3emu — drive the Xteink X3 QEMU board model from Python (Q5).

One place for everything a front-end needs (see tools/qemu/README.md, "Q5"):

  * build the merged 16 MiB flash image from a firmware directory
    (``firmware.bin`` + ``bootloader.bin`` + ``partitions.bin``),
  * start/stop QEMU with the panel frame chardev and a button socket,
  * inject physical button presses/releases (ADC ladder + power GPIO),
  * stream the panel frames the model writes (``X3PN`` records) with the
    wall-clock arrival time of every record.

The module is imported by ``tools/qemu/q5-scenarios.py`` (headless checks) and
``tools/qemu/x3-gui.py`` (interactive window); no third-party packages.

Button model facts this module relies on (hw/gpio/xteink_buttons.c):

  * back/confirm/left/right sit on the ADC1 ladder of GPIO1, up/down on GPIO2,
    power is the digital GPIO3 line. A press is a lower-case character,
    a release the matching upper-case one, ``-`` releases everything,
  * the firmware samples the ladder every 10 ms in its own task and queues
    edges, so a ``tap`` only has to keep the level for a couple of samples:
    ``tap()`` holds 90 ms by default,
  * long press in the firmware is 1000 ms (power: 400 ms), see
    src/ButtonEventManager.h — hold longer than that to get a long press.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
QEMU_BIN = ROOT / ".venv/qemu/src/build/qemu-system-riscv32"
QEMU_LIB = ROOT / ".venv/qemu/lib"
ESPTOOL = ROOT / ".venv/bin/esptool"
BOOT_APP0 = Path.home() / ".platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"

FLASH_SIZE = 16 * 1024 * 1024
FRAME_MAGIC = 0x4E503358  # "X3PN"
FRAME_HDR = 32

# Button name -> character the xteink-buttons model knows (lower = press).
BUTTON_KEYS = {
    "back": "b",
    "confirm": "c",
    "left": "l",
    "right": "r",
    "up": "u",
    "down": "d",
    "power": "p",
}

# The X3 reads back/confirm/left/right from ADC1 channel 1 (GPIO1) and
# up/down from channel 2 (GPIO2); power is a plain active-low GPIO line.
BUTTON_DEVICE_ARGS = ("back-adc=1,confirm-adc=1,left-adc=1,right-adc=1,"
                      "up-adc=2,down-adc=2,power-gpio=3")

# Firmware long-press threshold (src/ButtonEventManager.h).
LONG_PRESS_MS = 1000
POWER_LONG_PRESS_MS = 400


class EmulatorError(RuntimeError):
    pass


def _panel_frame_module():
    """Import tools/qemu/panel-frame.py (dash in the name) as a module.

    Reusing it keeps one implementation of the frame parser, the PNG writer and
    the ASCII preview.
    """
    path = ROOT / "tools/qemu/panel-frame.py"
    spec = importlib.util.spec_from_file_location("panel_frame", path)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


def _sd_files_module():
    """Import tools/qemu/sd-files.py (dash in the name) as a module.

    One FAT16 reader for the host side: the smoke gate and the Q6 checks read
    the card image the guest wrote, with the same parser the CLI uses.
    """
    path = ROOT / "tools/qemu/sd-files.py"
    spec = importlib.util.spec_from_file_location("sd_files", path)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


panel_frame = _panel_frame_module()
sd_files = _sd_files_module()


def md5_of(path: Path) -> str:
    digest = hashlib.md5()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


# --------------------------------------------------------------------------- #
# Firmware / images
# --------------------------------------------------------------------------- #

def firmware_dirs(fw_dir: Path | None = None) -> list[Path]:
    """Directories searched for bootloader.bin/partitions.bin, in order."""
    dirs = []
    if fw_dir is not None:
        dirs.append(Path(fw_dir))
    dirs.append(ROOT / "build/qemu/fw")
    dirs.append(ROOT / ".pio/build/default")
    return dirs


def install_firmware(source: Path, fw_dir: Path | None = None) -> Path:
    """Copy ``source`` into build/qemu/fw/firmware.bin, keep .pio untouched.

    ``bootloader.bin``/``partitions.bin`` are copied next to it when the target
    directory does not have them yet (normally from .pio/build/default). The
    firmware ELF is copied too when it sits next to the binary, so
    tools/qemu/qmp-probe.py keeps working on the selected firmware.
    """
    source = Path(source).expanduser()
    if not source.is_file():
        raise EmulatorError(f"firmware not found: {source}")
    target_dir = Path(fw_dir) if fw_dir else ROOT / "build/qemu/fw"
    target_dir.mkdir(parents=True, exist_ok=True)

    target = target_dir / "firmware.bin"
    if source.resolve() != target.resolve():
        shutil.copy2(source, target)
        elf = source.with_suffix(".elf")
        if elf.is_file():
            shutil.copy2(elf, target_dir / "firmware.elf")
        else:
            # A different .bin without its own ELF: drop the stale one so
            # addr2line never decodes the wrong image.
            (target_dir / "firmware.elf").unlink(missing_ok=True)

    for name in ("bootloader.bin", "partitions.bin"):
        dest = target_dir / name
        if dest.is_file():
            continue
        for candidate_dir in firmware_dirs(target_dir):
            candidate = candidate_dir / name
            if candidate.is_file() and candidate.resolve() != dest.resolve():
                shutil.copy2(candidate, dest)
                break
        else:
            raise EmulatorError(
                f"{name} missing: put it into {target_dir} or build the firmware "
                f"with `.venv/bin/pio run -e default` (looked in "
                f"{', '.join(str(d) for d in firmware_dirs(target_dir))})")
    return target


def ensure_flash(fw_dir: Path, flash: Path, stamp: Path, force: bool = False) -> Path:
    """Merge bootloader+partitions+boot_app0+app into a 16 MiB image.

    Rebuilds only when one of the inputs changed (md5 stamp next to the image),
    so starting the GUI repeatedly does not pay the 16 MiB write each time.
    """
    inputs = [fw_dir / "firmware.bin", fw_dir / "bootloader.bin", fw_dir / "partitions.bin"]
    for path in inputs:
        if not path.is_file():
            raise EmulatorError(f"missing firmware artifact: {path}")
    if not BOOT_APP0.is_file():
        raise EmulatorError(f"boot_app0.bin missing: {BOOT_APP0} (PlatformIO install?)")
    fingerprint = {p.name: md5_of(p) for p in inputs}
    fingerprint["boot_app0"] = str(BOOT_APP0)

    if not force and flash.is_file() and stamp.is_file():
        try:
            if json.loads(stamp.read_text()).get("fingerprint") == fingerprint:
                return flash
        except (OSError, ValueError):
            pass

    if not ESPTOOL.is_file():
        raise EmulatorError(f"esptool missing: {ESPTOOL}")
    flash.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(ESPTOOL), "--chip", "esp32c3", "merge-bin", "-o", str(flash),
           "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB",
           "--fill-flash-size", "16MB",
           "0x0", str(inputs[1]),
           "0x8000", str(inputs[2]),
           "0xe000", str(BOOT_APP0),
           "0x10000", str(inputs[0])]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        raise EmulatorError(f"esptool merge-bin failed ({proc.returncode}):\n"
                            f"{proc.stdout[-2000:]}\n{proc.stderr[-2000:]}")
    stamp.write_text(json.dumps({"fingerprint": fingerprint,
                                 "built_at": time.strftime("%Y-%m-%dT%H:%M:%S")}))
    return flash


def sd_book_files(books_dir: Path | None = None) -> list[Path]:
    """Real books copied into /books when the card is (re)built.

    build/qemu/sd-books/ is not in git (build/ is ignored): test books are user
    content, not fixtures. When the directory is missing the card simply keeps
    make-sd-image.py's own /books/test.txt.
    """
    directory = Path(books_dir) if books_dir else ROOT / "build/qemu/sd-books"
    if not directory.is_dir():
        return []
    return sorted(p for p in directory.iterdir() if p.is_file() and not p.name.startswith("."))


def ensure_sd(image: Path, force: bool = False, books_dir: Path | None = None) -> Path:
    if image.is_file() and not force:
        return image
    cmd = [sys.executable or "python3", str(ROOT / "tools/qemu/make-sd-image.py"),
           "--out", str(image)]
    for book in sd_book_files(books_dir):
        cmd += ["--add", f"{book}:/books/{book.name}"]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        raise EmulatorError(f"make-sd-image.py failed ({proc.returncode}):\n"
                            f"{proc.stdout[-1500:]}{proc.stderr[-1500:]}")
    return image


def read_card(image: Path | str) -> dict:
    """Parse the FAT16 card image from the host side (Q6).

    Returns ``{"paths": [...], "entries": [...], "books": <books.json or None>}``.
    Reading the image outside the guest is the independent proof that a write
    really reached the file: the guest's own log says a command succeeded, this
    says the bytes are on the card.
    """
    vol = sd_files.Fat16(Path(image).read_bytes())
    entries = vol.walk()
    books = None
    try:
        index = vol.lookup("/.crosspoint/shelf/books.json")
    except sd_files.FatError:
        index = None
    if index is not None and not index["dir"]:
        try:
            books = json.loads(vol.read_file(index["cluster"], index["size"]))
        except ValueError:
            books = "corrupt"
    return {
        "paths": [entry["path"] for entry in entries],
        "entries": entries,
        "books": books,
    }


# --------------------------------------------------------------------------- #
# Panel frames
# --------------------------------------------------------------------------- #

# --------------------------------------------------------------------------- #
# Orientation
# --------------------------------------------------------------------------- #
#
# The panel RAM is 792 sources x 528 gates and the Inkwell UI is drawn rotated
# in it: seen as stored, every screen is landscape with the text running along
# the left edge. The device is a portrait reader, so a front-end shows the frame
# rotated 90 degrees clockwise (528 x 792). ``rows_upright()`` does that.

BITREV = bytes(int(f"{i:08b}"[::-1], 2) for i in range(256))


def _transpose8(packed: int) -> int:
    """Transpose the 8x8 bit matrix packed in ``packed`` (Hacker's Delight)."""
    t = (packed ^ (packed >> 7)) & 0x00AA00AA00AA00AA
    packed = packed ^ t ^ (t << 7)
    t = (packed ^ (packed >> 14)) & 0x0000CCCC0000CCCC
    packed = packed ^ t ^ (t << 14)
    t = (packed ^ (packed >> 28)) & 0x00000000F0F0F0F0
    packed = packed ^ t ^ (t << 28)
    return packed


def _plane_bytes(frame, plane: str) -> bytes:
    """Packed 1 bpp bytes of the requested plane, in panel RAM row order.

    ``screen`` is the composed picture (Q7): the BW page with the 4-level
    grayscale nudge applied, flattened to "any ink = black" so a 1-bit
    front-end (X11 window, terminal) shows the page instead of the gray code.
    """
    if plane == "screen":
        planes = frame.screen_planes()
        if planes is None:
            return frame.plane(1)
        msb, lsb = planes
        return bytes((~(m | l)) & 0xFF for m, l in zip(msb, lsb))
    return frame.plane(0 if plane == "dtm1" else 1)


def rows_upright(frame, plane: str = "screen") -> tuple[int, int, list[bytes]]:
    """Logical image of a panel record, rotated upright (528x792 for X3).

    Returns ``(width, height, rows)`` with 1 bit per pixel, MSB first inside a
    byte, 1 = white. The block transpose is ~15x faster than the pixel loop and
    is asserted against ``rows_upright_naive()`` in tools/qemu/q5-scenarios.py.
    """
    stride = frame.stride
    height = frame.height
    src = frame.rows_logical(_plane_bytes(frame, plane))  # top-down, 792 bits per row
    dest_width = height                              # 528
    dest_stride = (dest_width + 7) // 8              # 66
    dest_rows = frame.width                          # 792
    out = [bytearray(dest_stride) for _ in range(dest_rows)]
    for src_row0 in range(0, height, 8):
        dest_byte = src_row0 // 8
        for src_col0 in range(0, frame.width, 8):
            src_byte = src_col0 // 8
            packed = 0
            for i in range(8):
                packed = (packed << 8) | src[src_row0 + i][src_byte]
            t = _transpose8(packed)
            for i in range(8):
                out[src_col0 + i][dest_byte] = (t >> (56 - 8 * i)) & 0xFF
    rows = [bytes(row[::-1]).translate(BITREV) for row in out]
    _ = stride
    return dest_width, dest_rows, rows


def rows_upright_naive(frame, plane: str = "screen") -> tuple[int, int, list[bytes]]:
    """Reference implementation of rows_upright(), one pixel at a time."""
    src = frame.rows_logical(_plane_bytes(frame, plane))
    width, height = frame.width, frame.height
    out = []
    for y in range(width):
        row = bytearray((height + 7) // 8)
        for x in range(height):
            if (src[height - 1 - x][y >> 3] >> (7 - (y & 7))) & 1:
                row[x >> 3] |= 1 << (7 - (x & 7))
        out.append(bytes(row))
    return height, width, out


def levels_upright(frame) -> tuple[int, int, list[bytes]] | None:
    """Composed screen as shade levels 0..3 (0 = paper), rotated upright.

    ``None`` for records without the screen planes (a pre-Q7 frame dump).
    """
    levels = panel_frame.levels_rows(frame)
    if levels is None:
        return None
    height = frame.height                          # 528 -> output width
    width = frame.width                            # 792 -> output rows
    out = [bytes(levels[height - 1 - x][y] for x in range(height))
           for y in range(width)]
    return height, width, out


def rows_xybitmap(rows: list[bytes]) -> bytes:
    """X11 XYBitmap wants LSB-first bits: revert every byte."""
    return b"".join(row.translate(BITREV) for row in rows)


class FrameStream:
    """Tails the panel frame dump and stamps every record with its arrival.

    The model writes one ``X3PN`` record per DISPLAY_REFRESH through a QEMU file
    chardev, i.e. one unbuffered ``write()`` per record, so a record becomes
    visible in the file as soon as the panel is told to refresh. Polling the
    size (default every 10 ms) gives the arrival time a front-end needs to
    report "key -> visible reaction" without touching the model.
    """

    def __init__(self, path: Path):
        self.path = Path(path)
        self._offset = 0
        self._pending = bytearray()
        self.frames: list[panel_frame.Frame] = []
        self.arrivals: list[float] = []

    def reset(self) -> None:
        self._offset = 0
        self._pending.clear()
        self.frames.clear()
        self.arrivals.clear()

    def poll(self) -> list[panel_frame.Frame]:
        """Return the records completed since the previous call."""
        try:
            size = os.stat(self.path).st_size
        except FileNotFoundError:
            return []
        if size < self._offset:  # truncated by a restart
            self.reset()
        if size == self._offset:
            return []
        with open(self.path, "rb") as fh:
            fh.seek(self._offset)
            chunk = fh.read(size - self._offset)
        self._offset = size
        self._pending.extend(chunk)

        new: list[panel_frame.Frame] = []
        while len(self._pending) >= FRAME_HDR:
            header = struct.unpack_from("<8I", self._pending, 0)
            if header[0] != FRAME_MAGIC:
                # Not a frame dump (or a half-written header): resynchronise by
                # dropping one byte at a time.
                del self._pending[0]
                continue
            width, height, stride, planes = header[2], header[3], header[4], header[5]
            size_of_payload = stride * height * planes
            if len(self._pending) < FRAME_HDR + size_of_payload:
                break
            payload = bytes(self._pending[FRAME_HDR:FRAME_HDR + size_of_payload])
            del self._pending[:FRAME_HDR + size_of_payload]
            frame = panel_frame.Frame(header, payload)
            frame.arrival = time.monotonic()
            new.append(frame)
            self.frames.append(frame)
            self.arrivals.append(frame.arrival)
        return new

    @property
    def latest(self) -> panel_frame.Frame | None:
        return self.frames[-1] if self.frames else None


# --------------------------------------------------------------------------- #
# Emulator
# --------------------------------------------------------------------------- #

NVS_OFFSET = 0x9000
NVS_LENGTH = 0x5000


def reset_nvs_region(flash: Path) -> None:
    """Erase the NVS partition before the emulator boots.

    The firmware counts cold-boot crashes in the "boot-health" NVS namespace and
    falls back to SD recovery after three of them — pressing nothing at all then
    shows a "choose firmware .bin" screen instead of the library, which looks like
    a broken build. QEMU's own cold-boot panics hit that counter, so every emulator
    start begins with a clean NVS. Settings live on the SD card image, not in NVS,
    and the device type is re-detected on boot, so nothing worth keeping is lost.
    Set XTEINK_EMU_KEEP_NVS=1 to keep the previous state (e.g. to reproduce the
    recovery screen on purpose).
    """
    if os.environ.get("XTEINK_EMU_KEEP_NVS") == "1" or not flash.is_file():
        return
    with open(flash, "r+b") as fh:
        fh.seek(NVS_OFFSET)
        fh.write(b"\xff" * NVS_LENGTH)


class Emulator:
    """A single QEMU run of the X3 board with live buttons and panel frames."""

    def __init__(self, firmware: Path | str | None = None, *, work_dir: Path | str | None = None,
                 fw_dir: Path | str | None = None, fresh_flash: bool = False,
                 fresh_sd: bool = False, sd_image: Path | str | None = None,
                 extra_args: tuple[str, ...] = (),
                 adc_trace: bool = False, panel_trace: bool = False,
                 sd_trace: bool = False, reset_nvs: bool = True):
        self.work_dir = Path(work_dir) if work_dir else ROOT / "build/qemu/ui"
        self.work_dir.mkdir(parents=True, exist_ok=True)
        self.fw_dir = Path(fw_dir) if fw_dir else ROOT / "build/qemu/fw"

        if firmware:
            install_firmware(Path(firmware), self.fw_dir)
        self.flash = self.work_dir / "flash.bin"
        self.sd = Path(sd_image) if sd_image else self.work_dir / "sd.img"
        self.frames_path = self.work_dir / "frame.bin"
        self.uart_log = self.work_dir / "uart.log"
        self.qemu_log = self.work_dir / "boot.log"

        self.fresh_flash = fresh_flash
        self.fresh_sd = fresh_sd
        self.reset_nvs = reset_nvs
        self.extra_args = tuple(extra_args)
        self.adc_trace = adc_trace
        self.panel_trace = panel_trace
        self.sd_trace = sd_trace

        self.proc: subprocess.Popen | None = None
        self.keys: socket.socket | None = None
        self.port = 0
        self.frames = FrameStream(self.frames_path)
        self._started_at = 0.0

    # -- lifecycle ---------------------------------------------------------- #
    def start(self) -> "Emulator":
        if not QEMU_BIN.is_file():
            raise EmulatorError(f"QEMU missing at {QEMU_BIN}: build it first "
                                f"(tools/qemu/README.md, 'Сборка из источников')")
        ensure_flash(self.fw_dir, self.flash, self.work_dir / "flash.stamp",
                     force=self.fresh_flash)
        if self.reset_nvs:
            reset_nvs_region(self.flash)
        ensure_sd(self.sd, force=self.fresh_sd)
        self.frames_path.unlink(missing_ok=True)
        self.frames.reset()

        last_error = None
        for _ in range(3):
            self.port = free_port()
            if self._spawn():
                return self
            last_error = self.qemu_log.read_text(errors="replace")[-1200:] if self.qemu_log.is_file() else ""
        raise EmulatorError(f"QEMU did not start on the button socket: {last_error}")

    def _spawn(self) -> bool:
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = str(QEMU_LIB) + (":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
        env["SSISD_TRACE"] = "1" if self.sd_trace else "0"
        env["XTEINK_PANEL_TRACE_BUSY"] = "1" if self.panel_trace else "0"
        env["XTEINK_ADC_TRACE"] = "1" if self.adc_trace else "0"
        cmd = [str(QEMU_BIN), "-display", "none", "-monitor", "none",
               "-serial", f"file:{self.uart_log}",
               "-machine", "esp32c3,xteink-panel=xteink-panel",
               "-drive", f"file={self.flash},if=mtd,format=raw",
               "-drive", f"file={self.sd},if=sd,format=raw",
               "-chardev", f"file,id=xteink-panel,path={self.frames_path}",
               "-chardev", f"socket,id=keys,host=127.0.0.1,port={self.port},server=on,wait=off",
               "-device", f"xteink-buttons,chardev=keys,{BUTTON_DEVICE_ARGS}",
               *self.extra_args]
        self.uart_log.unlink(missing_ok=True)
        with open(self.qemu_log, "wb") as log:
            self.proc = subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        self._started_at = time.monotonic()

        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                return False
            try:
                self.keys = socket.create_connection(("127.0.0.1", self.port), timeout=0.5)
                self.keys.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                return True
            except OSError:
                time.sleep(0.05)
        self.stop()
        return False

    def stop(self, timeout: float = 3.0) -> int:
        if self.keys is not None:
            try:
                self.keys.close()
            except OSError:
                pass
            self.keys = None
        if self.proc is None:
            return 0
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        status = self.proc.returncode
        self.proc = None
        # Collect whatever the panel wrote before the run ended.
        self.frames.poll()
        return status if status is not None else 0

    def __enter__(self) -> "Emulator":
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()

    @property
    def running(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def uptime(self) -> float:
        return time.monotonic() - self._started_at

    # -- buttons ------------------------------------------------------------ #
    def key_down(self, button: str) -> float:
        """Press ``button`` (level held until key_up); returns the wall time."""
        char = BUTTON_KEYS.get(button)
        if char is None:
            raise ValueError(f"unknown button {button!r}; known: {', '.join(BUTTON_KEYS)}")
        if self.keys is None:
            raise EmulatorError("button socket is not connected (start() first)")
        self.keys.sendall(char.encode())
        return time.monotonic()

    def key_up(self, button: str) -> float:
        char = BUTTON_KEYS.get(button)
        if char is None:
            raise ValueError(f"unknown button {button!r}")
        if self.keys is None:
            raise EmulatorError("button socket is not connected")
        self.keys.sendall(char.upper().encode())
        return time.monotonic()

    def release_all(self) -> None:
        if self.keys is not None:
            self.keys.sendall(b"-")

    def tap(self, button: str, hold_ms: float = 90) -> tuple[float, float]:
        """Short press: down, hold (default 90 ms), up. Returns (down, up)."""
        down = self.key_down(button)
        time.sleep(hold_ms / 1000.0)
        up = self.key_up(button)
        return down, up

    def hold(self, button: str, hold_ms: float) -> tuple[float, float]:
        """Long/hold press: same as tap(), just long enough to be a long press."""
        return self.tap(button, hold_ms)

    # -- observations ------------------------------------------------------- #
    def poll_frames(self) -> list[panel_frame.Frame]:
        return self.frames.poll()

    def is_socket_open(self) -> bool:
        """QEMU closes/drops when the chardev disconnects: detect a dead guest."""
        return self.keys is not None

    def uart_text(self) -> str:
        try:
            return self.uart_log.read_text(errors="replace")
        except FileNotFoundError:
            return ""

    def qemu_text(self) -> str:
        try:
            return self.qemu_log.read_text(errors="replace")
        except FileNotFoundError:
            return ""

    def panic_count(self) -> int:
        text = self.uart_text()
        return text.count("Guru Meditation Error") + text.count("Interrupt wdt timeout")

    def boot_count(self) -> int:
        return self.uart_text().count("ESP-ROM:esp32c3")

    # -- wait helpers (wall-clock based, the guest runs faster than real time) #
    def wait_frames(self, count: int, timeout: float = 20.0) -> list[panel_frame.Frame]:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.poll_frames()
            if len(self.frames.frames) >= count:
                return self.frames.frames
            if not self.running:
                break
            time.sleep(0.01)
        raise EmulatorError(f"waited {timeout:.0f} s for {count} panel frame(s), got "
                            f"{len(self.frames.frames)}")

    def wait_idle(self, quiet: float = 0.4, timeout: float = 30.0) -> None:
        """Wait until no new panel frame arrived for ``quiet`` seconds."""
        deadline = time.monotonic() + timeout
        last_count = len(self.frames.frames)
        last_change = time.monotonic()
        while time.monotonic() < deadline:
            self.poll_frames()
            if len(self.frames.frames) != last_count:
                last_count = len(self.frames.frames)
                last_change = time.monotonic()
            elif time.monotonic() - last_change >= quiet:
                return
            if not self.running:
                return
            time.sleep(0.01)
        raise EmulatorError("panel kept refreshing, never went idle")

    def wait_frame_after(self, timestamp: float, timeout: float = 15.0) -> panel_frame.Frame:
        """First frame that arrived after ``timestamp`` (a key-press wall time)."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.poll_frames()
            for frame, arrival in zip(self.frames.frames, self.frames.arrivals):
                if arrival >= timestamp:
                    return frame
            if not self.running:
                break
            time.sleep(0.005)
        raise EmulatorError(f"no panel refresh within {timeout:.1f} s after the key")

    # -- capture ------------------------------------------------------------ #
    def frame_png(self, frame, out: Path, plane: str = "screen",
                  upright: bool = True) -> Path:
        """Write the frame as a PNG.

        ``plane="screen"`` (default) renders the composed picture the panel
        shows -- four shades, 8-bit greyscale, so a reader page looks like a
        page and not like the gray code in RAM. ``plane="dtm1"``/``"dtm2"``
        keep the old 1-bit RAM plane view.

        ``upright`` (default) rotates it the way a human holds the device
        (528x792); ``upright=False`` keeps the panel RAM orientation
        (792x528), which is what tools/qemu/panel-frame.py renders.
        """
        out = Path(out)
        out.parent.mkdir(parents=True, exist_ok=True)
        if plane == "screen":
            if upright:
                sized = levels_upright(frame)
            else:
                levels = panel_frame.levels_rows(frame)
                sized = None if levels is None else (frame.width, frame.height, levels)
            if sized is not None:
                width, height, levels = sized
                out.write_bytes(panel_frame.png_gray_bytes(width, height, levels))
                return out
            # A pre-Q7 record has no screen planes: fall back to the RAM plane.
            plane = "dtm2"
        if upright:
            width, height, rows = rows_upright(frame, plane)
        else:
            width, height = frame.width, frame.height
            rows = frame.rows_logical(_plane_bytes(frame, plane))
        out.write_bytes(panel_frame.png_bytes(width, height, rows))
        return out

    def frame_ascii(self, frame, plane: str = "screen",
                    upright: bool = True) -> str:
        if plane == "screen":
            sized = levels_upright(frame) if upright else None
            if sized is None:
                levels = panel_frame.levels_rows(frame)
                sized = None if levels is None else (frame.width, frame.height, levels)
            if sized is not None:
                width, height, levels = sized
                return panel_frame.ascii_preview_levels(levels, width, height)
            plane = "dtm2"
        if upright:
            width, height, rows = rows_upright(frame, plane)
        else:
            width, height = frame.width, frame.height
            rows = frame.rows_logical(_plane_bytes(frame, plane))
        return panel_frame.ascii_preview(rows, width, height)

    def frame_stats(self, frame, plane: str = "dtm2") -> dict:
        """Statistics of a raw RAM plane (kept 1-bit for the Q4/Q5 checks)."""
        return panel_frame.stats(_plane_bytes(frame, plane), frame.width,
                                 frame.height, plane.upper())

    def frame_screen_stats(self, frame) -> dict | None:
        """Shade shares of the composed screen (Q7), ``None`` on a Q4 dump."""
        levels = panel_frame.levels_rows(frame)
        return None if levels is None else panel_frame.level_stats(levels)


def describe_firmware(fw_dir: Path) -> str:
    """Short human label for the status bar: name, size, build time."""
    path = fw_dir / "firmware.bin"
    if not path.is_file():
        return "нет прошивки"
    built = time.strftime("%d.%m %H:%M", time.localtime(path.stat().st_mtime))
    return f"{path.name} {path.stat().st_size / 1e6:.2f} MiB, {built}"


if __name__ == "__main__":
    print(__doc__)
    print(f"QEMU:   {QEMU_BIN} ({'ok' if QEMU_BIN.is_file() else 'MISSING'})")
    print(f"esptool:{ESPTOOL} ({'ok' if ESPTOOL.is_file() else 'MISSING'})")
    print(f"firmware: {describe_firmware(ROOT / 'build/qemu/fw')}")
