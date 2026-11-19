#!/usr/bin/env python3
"""x3-gui.py — the Xteink X3 firmware in a window, driven by the keyboard.

Starts QEMU with the X3 board model, shows the live e-paper frames the panel
model dumps and turns key presses into presses of the device's physical
buttons, so the firmware can be used the way a human uses the device:

    arrows    Up / Down  (side buttons) and Left / Right (front strip)
    Enter     Confirm
    Esc, BS   Back
    P         Power
    O         "Выбрать прошивку…" — file dialog over the panel
    R         restart the emulator (same firmware)
    F12       save a window screenshot (PNG)
    Q         quit

Holding a key holds the button: the model keeps the ADC ladder / GPIO line in
the pressed state until the key is released, which is what the firmware sees for
its 1 s long-press threshold (src/ButtonEventManager.h).

Only the standard library is used. The window is drawn with the X11 client
library through ctypes (no tkinter: it is not installed here, and no PIL either);
``--backend terminal`` gives an ANSI fallback for machines without a display.

See tools/qemu/README.md ("Q5") for the measurements and the known limits.
"""

from __future__ import annotations

import argparse
import ctypes
import os
import select
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import x3emu  # noqa: E402  (local module next to this script)

PANEL_W, PANEL_H = 528, 792      # upright view of the 792x528 panel RAM
STATUS_H = 36                    # two text lines above the panel
HINT_H = 22
WIN_W, WIN_H = PANEL_W, STATUS_H + PANEL_H + HINT_H

# Character cell of the X11 core font "fixed" (6x13); the status line and the
# firmware dialog are laid out with these numbers.
CHAR_W, CHAR_H = 6, 13
STATUS_BASELINE = 15
MESSAGE_BASELINE = 31
HINT_BASELINE = STATUS_H + PANEL_H + 15

BLACK, WHITE = 0x000000, 0xFFFFFF

# X11 keysym name -> device button (xteink-buttons character).
KEYMAP = {
    "Left": "left",
    "Right": "right",
    "Up": "up",
    "Down": "down",
    "Return": "confirm",
    "KP_Enter": "confirm",
    "Escape": "back",
    "BackSpace": "back",
    "p": "power",
    "P": "power",
}

HINT_TEXT = "arrows: flags  Enter=Confirm  Esc/BS=Back  P=Power  O=load  Q=quit"


# --------------------------------------------------------------------------- #
# X11 through ctypes
# --------------------------------------------------------------------------- #

KEY_PRESS, KEY_RELEASE = 2, 3
EXPOSE, CLIENT_MESSAGE = 12, 33


class _XKeyEvent(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong),
                ("send_event", ctypes.c_int), ("display", ctypes.c_void_p),
                ("window", ctypes.c_ulong), ("root", ctypes.c_ulong),
                ("subwindow", ctypes.c_ulong), ("time", ctypes.c_ulong),
                ("x", ctypes.c_int), ("y", ctypes.c_int),
                ("x_root", ctypes.c_int), ("y_root", ctypes.c_int),
                ("state", ctypes.c_uint), ("keycode", ctypes.c_uint),
                ("same_screen", ctypes.c_int)]


class _XClientMessageEvent(ctypes.Structure):
    _fields_ = [("type", ctypes.c_int), ("serial", ctypes.c_ulong),
                ("send_event", ctypes.c_int), ("display", ctypes.c_void_p),
                ("window", ctypes.c_ulong), ("message_type", ctypes.c_ulong),
                ("format", ctypes.c_int), ("data", ctypes.c_ulong * 5)]


class _XSizeHints(ctypes.Structure):
    _fields_ = [("flags", ctypes.c_long),
                ("x", ctypes.c_int), ("y", ctypes.c_int),
                ("width", ctypes.c_int), ("height", ctypes.c_int),
                ("min_width", ctypes.c_int), ("min_height", ctypes.c_int),
                ("max_width", ctypes.c_int), ("max_height", ctypes.c_int),
                ("width_inc", ctypes.c_int), ("height_inc", ctypes.c_int),
                ("min_aspect_x", ctypes.c_int), ("min_aspect_y", ctypes.c_int),
                ("max_aspect_x", ctypes.c_int), ("max_aspect_y", ctypes.c_int),
                ("base_width", ctypes.c_int), ("base_height", ctypes.c_int),
                ("win_gravity", ctypes.c_int)]


class _XFontStructHead(ctypes.Structure):
    _fields_ = [("ext_data", ctypes.c_void_p), ("fid", ctypes.c_ulong)]


class _XImageHead(ctypes.Structure):
    """First fields of XImage; enough to read a grabbed image back."""

    _fields_ = [("width", ctypes.c_int), ("height", ctypes.c_int),
                ("xoffset", ctypes.c_int), ("format", ctypes.c_int),
                ("data", ctypes.c_void_p), ("byte_order", ctypes.c_int),
                ("bitmap_unit", ctypes.c_int), ("bitmap_bit_order", ctypes.c_int),
                ("bitmap_pad", ctypes.c_int), ("depth", ctypes.c_int),
                ("bytes_per_line", ctypes.c_int), ("bits_per_pixel", ctypes.c_int)]


class X11Window:
    """A fixed-size window that shows a 1-bit bitmap and reports key events."""

    def __init__(self, title: str, width: int, height: int):
        try:
            lib = ctypes.CDLL("libX11.so.6")
        except OSError as exc:
            raise RuntimeError(f"libX11.so.6 is missing ({exc}); use --backend terminal") from exc
        self.lib = lib
        c = ctypes
        lib.XOpenDisplay.restype = c.c_void_p
        lib.XOpenDisplay.argtypes = [c.c_char_p]
        lib.XDefaultScreen.argtypes = [c.c_void_p]
        lib.XDefaultScreen.restype = c.c_int
        lib.XDefaultRootWindow.argtypes = [c.c_void_p]
        lib.XDefaultRootWindow.restype = c.c_ulong
        lib.XCreateSimpleWindow.argtypes = [c.c_void_p, c.c_ulong, c.c_int, c.c_int,
                                            c.c_uint, c.c_uint, c.c_uint, c.c_ulong, c.c_ulong]
        lib.XCreateSimpleWindow.restype = c.c_ulong
        lib.XSelectInput.argtypes = [c.c_void_p, c.c_ulong, c.c_long]
        lib.XMapWindow.argtypes = [c.c_void_p, c.c_ulong]
        lib.XStoreName.argtypes = [c.c_void_p, c.c_ulong, c.c_char_p]
        lib.XFlush.argtypes = [c.c_void_p]
        lib.XSync.argtypes = [c.c_void_p, c.c_int]
        lib.XNextEvent.argtypes = [c.c_void_p, c.c_void_p]
        lib.XPending.argtypes = [c.c_void_p]
        lib.XPending.restype = c.c_int
        lib.XConnectionNumber.argtypes = [c.c_void_p]
        lib.XConnectionNumber.restype = c.c_int
        lib.XLookupKeysym.argtypes = [c.c_void_p, c.c_int]
        lib.XLookupKeysym.restype = c.c_ulong
        lib.XKeysymToString.argtypes = [c.c_ulong]
        lib.XKeysymToString.restype = c.c_char_p
        lib.XInternAtom.argtypes = [c.c_void_p, c.c_char_p, c.c_int]
        lib.XInternAtom.restype = c.c_ulong
        lib.XSetWMProtocols.argtypes = [c.c_void_p, c.c_ulong, c.POINTER(c.c_ulong), c.c_int]
        lib.XSetWMNormalHints.argtypes = [c.c_void_p, c.c_ulong, c.POINTER(_XSizeHints)]
        lib.XCreateGC.argtypes = [c.c_void_p, c.c_ulong, c.c_ulong, c.c_void_p]
        lib.XCreateGC.restype = c.c_void_p
        lib.XSetForeground.argtypes = [c.c_void_p, c.c_void_p, c.c_ulong]
        lib.XSetBackground.argtypes = [c.c_void_p, c.c_void_p, c.c_ulong]
        lib.XSetFont.argtypes = [c.c_void_p, c.c_void_p, c.c_ulong]
        lib.XLoadQueryFont.argtypes = [c.c_void_p, c.c_char_p]
        lib.XLoadQueryFont.restype = c.c_void_p
        lib.XCreateImage.argtypes = [c.c_void_p, c.c_void_p, c.c_uint, c.c_int, c.c_int,
                                     c.c_char_p, c.c_uint, c.c_uint, c.c_int, c.c_int]
        lib.XCreateImage.restype = c.c_void_p
        lib.XPutImage.argtypes = [c.c_void_p, c.c_ulong, c.c_void_p, c.c_void_p,
                                  c.c_int, c.c_int, c.c_int, c.c_int, c.c_uint, c.c_uint]
        lib.XFillRectangle.argtypes = [c.c_void_p, c.c_ulong, c.c_void_p,
                                       c.c_int, c.c_int, c.c_uint, c.c_uint]
        lib.XDrawRectangle.argtypes = [c.c_void_p, c.c_ulong, c.c_void_p,
                                       c.c_int, c.c_int, c.c_uint, c.c_uint]
        lib.XDrawString.argtypes = [c.c_void_p, c.c_ulong, c.c_void_p,
                                    c.c_int, c.c_int, c.c_char_p, c.c_int]
        lib.XGetImage.argtypes = [c.c_void_p, c.c_ulong, c.c_int, c.c_int,
                                  c.c_uint, c.c_uint, c.c_ulong, c.c_int]
        lib.XGetImage.restype = c.c_void_p
        lib.XDefaultVisual.argtypes = [c.c_void_p, c.c_int]
        lib.XDefaultVisual.restype = c.c_void_p
        lib.XDefaultDepth.argtypes = [c.c_void_p, c.c_int]
        lib.XDefaultDepth.restype = c.c_uint
        lib.XkbSetDetectableAutoRepeat.argtypes = [c.c_void_p, c.c_int, c.c_void_p]

        self.display = lib.XOpenDisplay(None)
        if not self.display:
            raise RuntimeError("cannot open the X display (DISPLAY is not set/not running); "
                               "use --backend terminal")
        self.width, self.height = width, height
        self.screen = lib.XDefaultScreen(self.display)
        self.root = lib.XDefaultRootWindow(self.display)
        self.depth = lib.XDefaultDepth(self.display, self.screen)
        self.visual = lib.XDefaultVisual(self.display, self.screen)
        self.window = lib.XCreateSimpleWindow(
            self.display, self.root, 60, 40, width, height, 1, BLACK, WHITE)
        lib.XStoreName(self.display, self.window, title.encode()[:200])
        lib.XSelectInput(self.display, self.window, 1 | 2 | (1 << 15))  # keys + exposure
        # No resizing: the panel is 1:1.
        hints = _XSizeHints()
        hints.flags = 16 | 32  # PMinSize | PMaxSize
        hints.min_width = hints.max_width = width
        hints.min_height = hints.max_height = height
        lib.XSetWMNormalHints(self.display, self.window, ctypes.byref(hints))
        self.wm_delete = lib.XInternAtom(self.display, b"WM_DELETE_WINDOW", 0)
        atom = ctypes.c_ulong(self.wm_delete)
        lib.XSetWMProtocols(self.display, self.window, ctypes.byref(atom), 1)
        lib.XMapWindow(self.display, self.window)
        lib.XSync(self.display, 0)

        self.gc = lib.XCreateGC(self.display, self.window, 0, None)
        font = lib.XLoadQueryFont(self.display, b"fixed")
        self.has_text = bool(font)
        if font:
            fid = ctypes.cast(font, ctypes.POINTER(_XFontStructHead)).contents.fid
            lib.XSetFont(self.display, self.gc, fid)
        try:
            lib.XkbSetDetectableAutoRepeat(self.display, 1, None)  # no fake repeat releases
        except AttributeError:
            pass
        self._image = None
        self._image_size = (0, 0)
        self._buf = None
        self._ev = ctypes.create_string_buffer(256)

    # -- events ------------------------------------------------------------- #
    def fd(self) -> int:
        return self.lib.XConnectionNumber(self.display)

    def pump(self, timeout: float) -> list[dict]:
        """Wait up to ``timeout`` for events; return them as small dicts."""
        if not self.lib.XPending(self.display):
            try:
                ready, _, _ = select.select([self.fd()], [], [], timeout)
            except InterruptedError:
                return []
            if not ready:
                return []
        events = []
        while self.lib.XPending(self.display):
            self.lib.XNextEvent(self.display, ctypes.byref(self._ev))
            kind = ctypes.cast(self._ev, ctypes.POINTER(ctypes.c_int)).contents.value
            if kind in (KEY_PRESS, KEY_RELEASE):
                ev = ctypes.cast(self._ev, ctypes.POINTER(_XKeyEvent)).contents
                keysym = self.lib.XLookupKeysym(ctypes.byref(ev), 0)
                name = self.lib.XKeysymToString(keysym)
                events.append({"kind": "press" if kind == KEY_PRESS else "release",
                               "name": name.decode() if name else "",
                               "keycode": ev.keycode})
            elif kind == CLIENT_MESSAGE:
                ev = ctypes.cast(self._ev, ctypes.POINTER(_XClientMessageEvent)).contents
                if ev.data[0] == self.wm_delete:
                    events.append({"kind": "close"})
            elif kind == EXPOSE:
                events.append({"kind": "expose"})
        return events

    # -- drawing ------------------------------------------------------------ #
    def _ensure_image(self, width: int, height: int, stride: int):
        if self._image is not None and self._image_size == (width, height):
            return
        self._image_size = (width, height)
        self._buf = ctypes.create_string_buffer(stride * height)
        # depth 1, XYBitmap (format 0), LSB-first bits: 1 -> gc foreground
        self._image = self.lib.XCreateImage(
            self.display, self.visual, 1, 0, 0, self._buf, width, height, 8, stride)

    def put_bitmap(self, data: bytes, width: int, height: int, origin_y: int = 0):
        """Blit a 1-bit MSB-first image; bit 1 is painted with the fg colour."""
        stride = (width + 7) // 8
        if len(data) < stride * height:
            raise ValueError("bitmap too short")
        xy = x3emu.rows_xybitmap([data[y * stride:(y + 1) * stride] for y in range(height)])
        self._ensure_image(width, height, stride)
        ctypes.memmove(self._buf, xy, stride * height)
        self.lib.XSetForeground(self.display, self.gc, WHITE)
        self.lib.XSetBackground(self.display, self.gc, BLACK)
        self.lib.XPutImage(self.display, self.window, self.gc, self._image,
                           0, 0, 0, origin_y, width, height)
        self.lib.XFlush(self.display)

    def fill_rect(self, x: int, y: int, w: int, h: int, color: int):
        self.lib.XSetForeground(self.display, self.gc, color)
        self.lib.XFillRectangle(self.display, self.window, self.gc, x, y, w, h)

    def rectangle(self, x: int, y: int, w: int, h: int, color: int):
        self.lib.XSetForeground(self.display, self.gc, color)
        self.lib.XDrawRectangle(self.display, self.window, self.gc, x, y, w, h)

    def text(self, x: int, y: int, text: str, color: int = BLACK):
        if not self.has_text:
            return
        self.lib.XSetForeground(self.display, self.gc, color)
        # The X11 core font is ISO8859-1: anything else becomes "?".
        data = text.encode("latin-1", "replace")
        self.lib.XDrawString(self.display, self.window, self.gc, x, y, data, len(data))
        self.lib.XFlush(self.display)

    def screenshot_png(self, path: Path) -> Path:
        """Grab the window back through XGetImage and write a 1-bit PNG."""
        image_ptr = self.lib.XGetImage(self.display, self.window, 0, 0,
                                       self.width, self.height, 0xFFFFFFFF, 2)
        if not image_ptr:
            raise RuntimeError("XGetImage failed")
        image = ctypes.cast(image_ptr, ctypes.POINTER(_XImageHead)).contents
        width, height = image.width, image.height
        raw = ctypes.string_at(image.data, image.bytes_per_line * height)
        step = max(1, image.bits_per_pixel // 8)
        rows = []
        for y in range(height):
            row = bytearray((width + 7) // 8)
            off = y * image.bytes_per_line
            for x in range(width):
                pixel = int.from_bytes(raw[off + x * step: off + x * step + 3], "little")
                if pixel & 0xFFFFFF > 0x7F7F7F:
                    row[x >> 3] |= 1 << (7 - (x & 7))
            rows.append(bytes(row))
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(x3emu.panel_frame.png_bytes(width, height, rows))
        return path

    def close(self):
        try:
            self.lib.XFlush(self.display)
        except Exception:
            pass


# --------------------------------------------------------------------------- #
# Firmware file dialog (list + directory navigation, no toolkit available)
# --------------------------------------------------------------------------- #

def _latin(text: str) -> str:
    """The window font is ISO8859-1: keep ASCII, replace the rest."""
    return "".join(ch if 32 <= ord(ch) < 127 else "?" for ch in text)


class FirmwareDialog:
    """Pick a firmware file from the filesystem, drawn over the panel.

    Keys: Up/Down move, Enter opens a directory or loads the file, Esc/Back
    goes up one directory (and closes the dialog at the top of the tree).
    """

    MAX_LINES = 30

    def __init__(self, start: Path):
        self.dir = start if start.is_dir() else start.parent
        self.entries: list[Path] = []
        self.index = 0
        self.top = 0
        self.refresh()

    def refresh(self):
        entries = []
        parent = self.dir.parent
        if parent != self.dir:
            entries.append(parent)
        try:
            children = sorted(self.dir.iterdir(), key=lambda p: (not p.is_dir(), p.name.lower()))
        except OSError:
            children = []
        for child in children:
            if child.is_dir() and not child.name.startswith("."):
                entries.append(child)
            elif child.is_file() and child.suffix in (".bin",):
                entries.append(child)
        self.entries = entries
        self.index = min(self.index, max(0, len(entries) - 1))

    def move(self, delta: int):
        if not self.entries:
            return
        self.index = (self.index + delta) % len(self.entries)
        if self.index < self.top:
            self.top = self.index
        elif self.index >= self.top + self.MAX_LINES:
            self.top = self.index - self.MAX_LINES + 1

    def selected(self) -> Path | None:
        return self.entries[self.index] if self.entries else None

    def activate(self) -> Path | None:
        """Enter: return the firmware to load, or navigate into a directory."""
        path = self.selected()
        if path is None:
            return None
        if path.is_dir():
            self.dir = path
            self.index = self.top = 0
            self.refresh()
            return None
        return path

    def up(self) -> bool:
        """Esc: go to the parent directory; False means 'close the dialog'."""
        parent = self.dir.parent
        if parent == self.dir:
            return False
        self.dir = parent
        self.index = self.top = 0
        self.refresh()
        return True

    def draw(self, win: X11Window):
        x0, y0, w, h = 8, STATUS_H + 8, PANEL_W - 16, PANEL_H - 16
        win.fill_rect(x0, y0, w, h, WHITE)
        win.rectangle(x0, y0, w, h, BLACK)
        win.rectangle(x0 + 1, y0 + 1, w - 2, h - 2, BLACK)
        win.text(x0 + 8, y0 + 18, "Load firmware")
        win.text(x0 + 8, y0 + 34, _latin(self.dir.name or str(self.dir))[:70])
        y = y0 + 54
        for i in range(self.top, min(len(self.entries), self.top + self.MAX_LINES)):
            entry = self.entries[i]
            label = entry.name + ("/" if entry.is_dir() else "")
            if entry.is_dir() and entry.parent == self.dir.parent and entry.parent != self.dir:
                label = ".."
            label = _latin(label)[:60]
            if i == self.index:
                win.fill_rect(x0 + 6, y - CHAR_H + 3, w - 12, CHAR_H + 4, BLACK)
                win.text(x0 + 10, y, label, WHITE)
            else:
                win.text(x0 + 10, y, label, BLACK)
            y += CHAR_H + 5
        win.text(x0 + 8, y0 + h - 30, "Enter=open/load   Esc=up   Arrows=move")
        win.text(x0 + 8, y0 + h - 14, _latin(str(self.dir))[:70])


# --------------------------------------------------------------------------- #
# Application
# --------------------------------------------------------------------------- #

class App:
    def __init__(self, args):
        self.args = args
        self.emu: x3emu.Emulator | None = None
        self.fw_dir = Path(args.fw_dir)
        self.firmware = Path(args.firmware) if args.firmware else None
        self.dialog: FirmwareDialog | None = None
        self.held: dict[str, float] = {}
        self.quit = False
        self.status = ""
        self.last_frame = None
        self.last_frame_at = 0.0
        self.boot_started = 0.0
        self.boot_to_ui: float | None = None
        self.frame_count_seen = 0
        self.key_latencies: list[float] = []
        self.pending_key_at: float | None = None
        self.screenshot_path = Path(args.screenshot)
        self.win: X11Window | None = None
        self.message = ""

    # -- emulator ----------------------------------------------------------- #
    def have_firmware(self) -> bool:
        return bool(self.firmware) or (self.fw_dir / "firmware.bin").is_file()

    def start_emu(self):
        if self.emu is not None:
            self.emu.stop()
            self.emu = None
        self.last_frame = None
        self.frame_count_seen = 0
        self.held.clear()
        if not self.have_firmware():
            self.show_placeholder()
            self.message = "no firmware: press O to choose a .bin file"
            self.render_status()
            return
        self.message = "starting QEMU..."
        self.render_status()
        self.uart_probe_offset = 0
        self.recovery_restarts = 0
        self.emu = x3emu.Emulator(
            firmware=self.firmware, work_dir=self.args.work_dir, fw_dir=self.fw_dir,
            fresh_flash=self.args.fresh_flash, fresh_sd=self.args.fresh_sd,
            adc_trace=self.args.adc_trace, panel_trace=self.args.panel_trace)
        self.emu.start()
        self.boot_started = time.monotonic()
        self.boot_to_ui = None
        self.message = "booting..."

    def show_placeholder(self) -> None:
        """Clear the panel area and invite the user to pick a firmware."""
        if self.win is None:
            return
        self.win.fill_rect(0, 0, WIN_W, STATUS_H, WHITE)
        self.win.fill_rect(0, STATUS_H, WIN_W, PANEL_H, WHITE)
        self.win.text(14, STATUS_H + 24, "No firmware loaded.")
        self.win.text(14, STATUS_H + 44, "Press O to pick a firmware .bin,")
        self.win.text(14, STATUS_H + 60, "or start with: x3-gui.py --firmware PATH")
        self.render_hint()

    def emu_note(self) -> str:
        if self.emu is None:
            return "no emulator"
        if not self.emu.running:
            return "QEMU exited"
        return f"up {time.monotonic() - self.boot_started:.1f}s"

    # -- input -------------------------------------------------------------- #
    def key_down(self, button: str):
        if self.emu is None or not self.emu.running or button in self.held:
            return
        self.emu.key_down(button)
        self.held[button] = time.monotonic()
        self.render_status()

    def key_up(self, button: str):
        if self.emu is None or button not in self.held:
            return
        self.emu.key_up(button)
        self.held.pop(button, None)
        self.render_status()

    def release_all(self):
        for button in list(self.held):
            self.key_up(button)

    def handle_key(self, kind: str, name: str):
        if kind == "release":
            button = KEYMAP.get(name)
            if button:
                self.key_up(button)
            return
        # press
        # Keys that work in every mode, including while the file dialog is open.
        if name in ("F12",):
            self.take_screenshot()
            return
        if name in ("q", "Q"):
            self.quit = True
            return
        if self.dialog is not None:
            self.dialog_key(name)
            return
        if name in ("o", "O"):
            self.open_dialog()
            return
        if name in ("r", "R"):
            self.start_emu()
            self.redraw_panel()
            return
        button = KEYMAP.get(name)
        if button:
            if self.pending_key_at is None:
                self.pending_key_at = time.monotonic()
            self.key_down(button)

    def dialog_key(self, name: str):
        assert self.dialog is not None
        if name in ("Up", "KP_Up"):
            self.dialog.move(-1)
        elif name in ("Down", "KP_Down"):
            self.dialog.move(1)
        elif name in ("Return", "KP_Enter"):
            chosen = self.dialog.activate()
            if chosen is not None:
                self.load_firmware(chosen)
                return
        elif name in ("Escape", "BackSpace"):
            if not self.dialog.up():
                self.dialog = None
        else:
            return
        self.dialog.draw(self.win)

    def open_dialog(self):
        start = self.firmware.parent if self.firmware else Path(self.args.fw_dir)
        self.release_all()
        self.dialog = FirmwareDialog(start)
        self.dialog.draw(self.win)

    def load_firmware(self, path: Path):
        self.dialog = None
        self.firmware = path
        try:
            x3emu.install_firmware(path, self.fw_dir)
        except x3emu.EmulatorError as exc:
            self.message = str(exc)[:70]
            self.render_status()
            return
        self.start_emu()

    # -- drawing ------------------------------------------------------------ #
    def redraw_panel(self):
        if self.win is None:
            return
        self.win.fill_rect(0, 0, WIN_W, STATUS_H, WHITE)
        self.win.fill_rect(0, STATUS_H, WIN_W, PANEL_H, WHITE)
        if self.last_frame is None:
            self.win.text(10, STATUS_H + 20, "waiting for the first panel refresh...")
        else:
            height = self.last_frame.height
            width, h, rows = x3emu.rows_upright(self.last_frame, self.args.plane)
            if (width, h) != (PANEL_W, PANEL_H):
                # unexpected geometry: scale by cropping, but say so
                rows = rows[:PANEL_H]
            data = b"".join(rows)
            self.win.put_bitmap(data, width, h, origin_y=STATUS_H)
        self.render_status()
        self.render_hint()

    def render_status(self):
        if self.win is None:
            return
        win = self.win
        win.fill_rect(0, 0, WIN_W, STATUS_H, WHITE)
        fw_name = Path(self.firmware).name if self.firmware else (
            self.fw_dir / "firmware.bin").name
        parts = [fw_name[:26]]
        if self.held:
            button, since = max(self.held.items(), key=lambda kv: kv[1])
            held = time.monotonic() - since
            tag = "LONG" if held >= x3emu.LONG_PRESS_MS / 1000.0 else ""
            parts.append(f"{button.upper()} {held:.1f}s {tag}".strip())
        else:
            parts.append("key -")
        parts.append(f"frames {len(self.emu.frames.frames) if self.emu else 0}")
        age = time.monotonic() - self.last_frame_at if self.last_frame_at else 0.0
        parts.append(f"frame {age:.1f}s")
        if self.key_latencies:
            parts.append(f"key->frame {self.key_latencies[-1] * 1000:.0f}ms")
        parts.append(self.emu_note())
        line = " | ".join(parts)
        win.text(6, STATUS_BASELINE, _latin(line)[:88])
        if self.held:
            # reverse-video tag for the pressed button
            label = f" {max(self.held).upper()} "
            x = 6 + CHAR_W * (len(fw_name[:26]) + 3)
            win.fill_rect(x, 2, len(label) * CHAR_W + 2, CHAR_H + 5, BLACK)
            win.text(x + 2, STATUS_BASELINE, label, WHITE)
        if self.message:
            win.text(6, MESSAGE_BASELINE, _latin(self.message)[:88])

    def render_hint(self):
        self.win.fill_rect(0, STATUS_H + PANEL_H, WIN_W, HINT_H, WHITE)
        self.win.text(6, HINT_BASELINE, HINT_TEXT[:88])

    def take_screenshot(self) -> Path:
        path = self.win.screenshot_png(self.screenshot_path)
        self.message = f"screenshot: {path}"
        self.render_status()
        print(f"screenshot: {path}", flush=True)
        return path

    def maybe_restart_after_recovery(self) -> None:
        """Restart QEMU with a clean NVS when the firmware latches SD recovery.

        BootHealth counts cold-boot crashes in NVS and, after three of them, boots the
        "choose a firmware .bin" screen instead of the library. In the emulator those
        crashes are QEMU's own cold-boot panics, so during one interactive session the
        counter fills up and the window becomes unusable. The firmware's boot path must
        not be touched for that, and the flash device keeps its writes in RAM (host-side
        edits to flash.bin do not reach a running QEMU), so the only reliable cure is a
        QEMU restart with the NVS partition erased.
        """
        if self.emu is None or self.recovery_restarts >= 2:
            return
        try:
            log = self.emu.uart_log
            if not log.is_file():
                return
            size = log.stat().st_size
            if size <= self.uart_probe_offset:
                return
            with log.open("rb") as fh:
                fh.seek(self.uart_probe_offset)
                chunk = fh.read(min(size - self.uart_probe_offset, 1 << 16))
            self.uart_probe_offset = size
            if b"entering SD recovery" not in chunk:
                return
        except OSError:
            return
        self.message = "SD recovery latched — restarting QEMU with a clean NVS"
        self.render_status()
        flash = self.emu.flash
        self.emu.stop()
        x3emu.reset_nvs_region(flash)
        self.recovery_restarts += 1
        self.uart_probe_offset = 0
        self.start_emu()

    # -- main loop ---------------------------------------------------------- #
    def run(self) -> int:
        self.start_emu()
        if self.args.choose or not self.have_firmware():
            # Only reached when there is nothing to boot (or --choose was given).
            self.open_dialog()
        last_status = 0.0
        last_recovery_probe = 0.0
        while not self.quit:
            for event in self.win.pump(0.02):
                if event["kind"] == "close":
                    self.quit = True
                elif event["kind"] == "expose":
                    self.redraw_panel()
                else:
                    self.handle_key(event["kind"], event["name"])
            if self.emu is not None:
                new_frames = self.emu.poll_frames()
                if new_frames:
                    if self.boot_to_ui is None:
                        self.boot_to_ui = new_frames[0].arrival - self.boot_started
                    if self.pending_key_at is not None:
                        self.key_latencies.append(new_frames[0].arrival - self.pending_key_at)
                        self.pending_key_at = None
                    self.last_frame = new_frames[-1]
                    self.last_frame_at = new_frames[-1].arrival
                    self.message = ""  # boot finished: the first frame is on screen
                    self.redraw_panel()
                if not self.emu.running:
                    self.message = "QEMU stopped — press R to restart"
                    self.render_status()
                    time.sleep(0.2)
            if time.monotonic() - last_status > 0.1:
                last_status = time.monotonic()
                if self.held or self.message:
                    self.render_status()
                if time.monotonic() - last_recovery_probe > 0.5:
                    last_recovery_probe = time.monotonic()
                    self.maybe_restart_after_recovery()
        self.shutdown()
        return 0

    def shutdown(self):
        self.release_all()
        if self.emu is not None:
            self.emu.stop()
            self.emu = None
        if self.win is not None:
            self.win.close()


# --------------------------------------------------------------------------- #
# Self-test (headless-friendly: no keyboard needed)
# --------------------------------------------------------------------------- #

def self_test(app: App, seconds: float) -> int:
    app.start_emu()
    deadline = time.monotonic() + seconds
    frames_seen = 0
    while time.monotonic() < deadline:
        for event in app.win.pump(0.02):
            if event["kind"] == "expose":
                app.redraw_panel()
        new = app.emu.poll_frames()
        if new:
            if app.boot_to_ui is None:
                app.boot_to_ui = new[0].arrival - app.boot_started
            app.last_frame, app.last_frame_at = new[-1], new[-1].arrival
            frames_seen += len(new)
            app.redraw_panel()
            if frames_seen >= 4:
                break
    if app.last_frame is None:
        print("SELFTEST: no frame", file=sys.stderr)
        app.shutdown()
        return 2
    app.emu.wait_idle(0.4, timeout=20)
    app.redraw_panel()
    # Inject a Back tap through the same path the keyboard uses, and time it.
    start = time.monotonic()
    app.handle_key("press", "Escape")
    time.sleep(0.09)
    app.handle_key("release", "Escape")
    got = None
    while time.monotonic() - start < 15:
        for event in app.win.pump(0.0):
            if event["kind"] == "expose":
                app.redraw_panel()
        new = app.emu.poll_frames()
        if new:
            got = new[0].arrival - start
            app.last_frame = new[-1]
            app.last_frame_at = new[-1].arrival
            app.redraw_panel()
            break
    print(f"SELFTEST: boot_to_first_frame={app.boot_to_ui if app.boot_to_ui is None else round(app.boot_to_ui, 2)}s "
          f"back_to_frame={got if got is None else round(got, 3)}s "
          f"frames={len(app.emu.frames.frames)} panics={app.emu.panic_count()}",
          flush=True)
    app.take_screenshot()
    app.shutdown()
    return 0 if got is not None else 3


# --------------------------------------------------------------------------- #
# Terminal fallback
# --------------------------------------------------------------------------- #

class Terminal:
    """ANSI rendering of the panel plus raw-key input (no display needed)."""

    def __init__(self, cols: int = 88, rows: int = 44):
        self.cols, self.rows = cols, rows

    def enter(self):
        self._old = None
        try:
            import termios
            import tty
            fd = sys.stdin.fileno()
            self._old = termios.tcgetattr(fd)
            tty.setcbreak(fd)
        except Exception:
            pass
        sys.stdout.write("\033[?1049h\033[2J\033[?25l")

    def leave(self):
        sys.stdout.write("\033[?25h\033[?1049l")
        sys.stdout.flush()
        if self._old is not None:
            import termios
            termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, self._old)

    def title(self, text: str):
        sys.stdout.write(f"\033]0;{text}\007")
        sys.stdout.flush()

    def draw(self, frame, status: str):
        width, height, rows = x3emu.rows_upright(frame)
        cell_w = max(1, width // self.cols)
        cell_h = max(1, height // (self.rows * 2))
        out = ["\033[H"]
        for cy in range(self.rows):
            line = []
            for cx in range(self.cols):
                top = self._sample(rows, width, height, cx * cell_w, cy * cell_h * 2, cell_w, cell_h)
                bot = self._sample(rows, width, height, cx * cell_w,
                                   cy * cell_h * 2 + cell_h, cell_w, cell_h)
                if top < 128 and bot < 128:
                    line.append("\033[30m\033[47m \033[0m")
                elif top < 128:
                    line.append("\033[30;47m▀\033[0m")
                elif bot < 128:
                    line.append("\033[30;47m▄\033[0m")
                else:
                    line.append("\033[30;47m \033[0m")
            out.append("".join(line) + "\033[0m\n")
        out.append("\033[0m" + status[: self.cols] + "\033[K")
        sys.stdout.write("".join(out))
        sys.stdout.flush()

    @staticmethod
    def _sample(rows, width, height, x0, y0, cw, ch) -> int:
        y0 = min(y0, height - 1)
        x0 = min(x0, width - 1)
        dark = total = 0
        for y in range(y0, min(height, y0 + ch)):
            row = rows[y]
            for x in range(x0, min(width, x0 + cw)):
                total += 1
                if not (row[x >> 3] >> (7 - (x & 7))) & 1:
                    dark += 1
        return 0 if total and dark * 2 > total else 255


TERM_KEYS = {
    "\x1b[A": "Up", "\x1b[B": "Down", "\x1b[C": "Right", "\x1b[D": "Left",
    "\r": "Return", "\n": "Return", "\x7f": "BackSpace",
}


def run_terminal(app: App) -> int:
    if not app.have_firmware():
        print("no firmware: pass --firmware PATH or put firmware.bin into build/qemu/fw",
              file=sys.stderr)
        return 2
    term = Terminal()
    term.enter()
    term.title("X3 QEMU — Inkwell")
    try:
        app.start_emu()
        sys.stdout.write("keys: arrows / Enter=Confirm / Backspace=Back / p=Power / q=quit\n")
        while not app.quit:
            new = app.emu.poll_frames()
            if new:
                app.last_frame, app.last_frame_at = new[-1], new[-1].arrival
                held = ""
                if app.held:
                    button, since = next(iter(app.held.items()))
                    held = f" {button.upper()} {time.monotonic() - since:.1f}s"
                frames = len(app.emu.frames.frames)
                term.draw(app.last_frame,
                          f"{Path(app.firmware).name if app.firmware else 'firmware.bin'}"
                          f" | frames {frames}{held} | q=quit")
            ready, _, _ = select.select([sys.stdin], [], [], 0.05)
            if not ready:
                continue
            data = os.read(sys.stdin.fileno(), 8).decode("latin-1", "ignore")
            if data in ("q", "Q"):
                break
            name = TERM_KEYS.get(data)
            if name is None and len(data) == 1 and data in "pP":
                name = data
            if name is None:
                continue
            if name in KEYMAP:
                app.handle_key("press", name)
                # terminal keys are taps: the firmware needs the release as well
                time.sleep(0.12)
                app.handle_key("release", name)
        return 0
    finally:
        app.shutdown()
        term.leave()


# --------------------------------------------------------------------------- #
# main
# --------------------------------------------------------------------------- #

def parse_args(argv: list[str]) -> argparse.Namespace:
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--firmware", default="", help="firmware.bin to boot (copied to build/qemu/fw)")
    parser.add_argument("--choose", action="store_true",
                        help="open the firmware file dialog at startup")
    parser.add_argument("--fw-dir", default=str(root / "build/qemu/fw"),
                        help="directory holding firmware.bin/bootloader.bin/partitions.bin")
    parser.add_argument("--work-dir", default=str(root / "build/qemu/ui"),
                        help="where the flash image, frame dump and logs go")
    parser.add_argument("--backend", choices=("auto", "x11", "terminal"), default="auto")
    parser.add_argument("--plane", choices=("screen", "dtm1", "dtm2"), default="screen",
                        help="screen (default) = the composed page with the gray "
                             "nudge applied; dtm1/dtm2 = raw panel RAM plane")
    parser.add_argument("--fresh-flash", action="store_true", help="rebuild the flash image, blank NVS")
    parser.add_argument("--fresh-sd", action="store_true", help="rebuild the FAT16 card image")
    parser.add_argument("--adc-trace", action="store_true", help="log ADC conversions to boot.log")
    parser.add_argument("--panel-trace", action="store_true", help="log panel BUSY edges to boot.log")
    parser.add_argument("--screenshot", default=str(root / "build/qemu/q5/gui-window.png"))
    parser.add_argument("--self-test", type=float, default=0.0, metavar="SECONDS",
                        help="boot, inject one Back tap, save a screenshot, exit")
    return parser.parse_args(argv)


def resolve_firmware(args) -> None:
    """Pick the firmware to boot before the window opens.

    Order: --firmware PATH, then build/qemu/fw/firmware.bin (the file `pio run`
    leaves here after tools/qemu/run-ui.sh copied it), then the PlatformIO build
    in .pio/build/default, and only if there is none at all the file dialog —
    a user who just wants to look at the firmware must not land in a file
    chooser. ``--choose`` opens the dialog regardless.
    """
    if args.firmware:
        args.firmware = str(x3emu.install_firmware(Path(args.firmware), Path(args.fw_dir)))
        return
    fw_dir = Path(args.fw_dir)
    if (fw_dir / "firmware.bin").is_file():
        args.firmware = str(fw_dir / "firmware.bin")
        return
    fallback = x3emu.ROOT / ".pio/build/default/firmware.bin"
    if fallback.is_file():
        args.firmware = str(x3emu.install_firmware(fallback, fw_dir))
        return
    args.firmware = ""
    args.choose = True


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    resolve_firmware(args)
    backend = args.backend
    if backend == "auto":
        backend = "x11" if os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY") else "terminal"
    app = App(args)
    if backend == "terminal":
        return run_terminal(app)
    try:
        name = Path(args.firmware).stem if args.firmware else "no firmware"
        app.win = X11Window(f"X3 QEMU — {name}", WIN_W, WIN_H)
    except RuntimeError as exc:
        print(f"{exc}\nfaling back to the terminal UI.", file=sys.stderr)
        app.win = None
        return run_terminal(app)
    try:
        if args.self_test:
            return self_test(app, args.self_test)
        return app.run()
    except KeyboardInterrupt:
        return 130
    finally:
        if app.win is not None:
            app.shutdown()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
