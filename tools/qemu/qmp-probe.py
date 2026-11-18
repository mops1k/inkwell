#!/usr/bin/env python3
"""Sample a running QEMU guest through QMP and report where the CPU is.

Q4 needs to answer "where does the firmware stand after the panel refresh
command (0x12)?" without a gdbstub for riscv32-esp. This tool connects to the
QMP socket that boot.sh/qemu can expose, asks the human monitor for the guest
registers a few times, and resolves the PC (plus anything on the stack that
looks like a return address) through riscv32-esp-elf-addr2line.

Usage:
  tools/qemu/qmp-probe.py --socket /tmp/q4-qmp.sock --samples 4 --interval 1.0 \
      --elf build/qemu/fw/firmware.elf

The QEMU side is a plain:
  qemu-system-riscv32 ... -qmp unix:/tmp/q4-qmp.sock,server=on,wait=off
"""

from __future__ import annotations

import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time

DEFAULT_ADDR2LINE = os.path.expanduser(
    "~/.platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-addr2line"
)

# Any address in these ranges is flash/IRAM code on the ESP32-C3 memory map.
CODE_RANGES = (
    (0x3C000000, 0x3C800000),  # DROM / IROM (flash mapped)
    (0x3FC00000, 0x3FCE0000),  # static code
    (0x40370000, 0x403E0000),  # IRAM
    (0x42000000, 0x42800000),  # flash text (IROM)
)


class Qmp:
    def __init__(self, path: str, timeout: float = 10.0):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(path)
        self.buf = b""
        self._read_message()  # greeting
        self.command("qmp_capabilities")

    def _read_message(self) -> dict:
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("QMP connection closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def command(self, name: str, **args) -> dict:
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            reply = self._read_message()
            if "event" in reply:
                continue
            if "error" in reply:
                raise RuntimeError(f"{name}: {reply['error']}")
            return reply.get("return")

    def hmp(self, command_line: str) -> str:
        return self.command("human-monitor-command", **{"command-line": command_line})

    def read_words(self, addr: int, count: int) -> list[int]:
        out = self.hmp(f"xp /{count}wx 0x{addr:08x}")
        words = []
        for tok in out.split():
            tok = tok.rstrip(":")
            if re.fullmatch(r"[0-9a-fA-F]{8}", tok):
                words.append(int(tok, 16))
        return words


def parse_regs(text: str) -> dict[str, int]:
    """Pull the registers we care about out of the HMP `info registers` dump."""
    regs: dict[str, int] = {}
    patterns = {
        "pc": r"^\s*pc\s+([0-9a-fA-F]+)\s*$",
        "mepc": r"^\s*mepc\s+([0-9a-fA-F]+)",
        "mcause": r"^\s*mcause\s+([0-9a-fA-F]+)",
        "mstatus": r"^\s*mstatus\s+([0-9a-fA-F]+)",
        "mip": r"^\s*mip\s+([0-9a-fA-F]+)",
        "ra": r"x1/ra\s+([0-9a-fA-F]+)",
        "sp": r"x2/sp\s+([0-9a-fA-F]+)",
    }
    for name, pattern in patterns.items():
        m = re.search(pattern, text, re.MULTILINE)
        if m:
            regs[name] = int(m.group(1), 16)
    return regs


def resolve(addr2line: str, elf: str, addrs: list[int]) -> dict[int, str]:
    if not addrs:
        return {}
    try:
        out = subprocess.run(
            [addr2line, "-f", "-C", "-e", elf] + [f"0x{a:08x}" for a in addrs],
            capture_output=True,
            text=True,
            check=False,
        ).stdout.splitlines()
    except OSError as exc:  # addr2line missing: degrade to raw numbers
        return {a: f"<no addr2line: {exc}>" for a in addrs}
    names = {}
    for i, addr in enumerate(addrs):
        func = out[i * 2] if i * 2 < len(out) else "?"
        loc = out[i * 2 + 1] if i * 2 + 1 < len(out) else "?"
        names[addr] = f"{func} ({loc})"
    return names


def looks_like_code(word: int) -> bool:
    return any(lo <= word < hi for lo, hi in CODE_RANGES)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--socket", default="/tmp/q4-qmp.sock", help="QMP unix socket path")
    ap.add_argument("--samples", type=int, default=1)
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--elf", default="build/qemu/fw/firmware.elf")
    ap.add_argument("--addr2line", default=DEFAULT_ADDR2LINE)
    ap.add_argument("--stack-words", type=int, default=64, help="words of stack to scan for return addresses")
    ap.add_argument("--raw", action="store_true", help="print the whole info registers dump")
    args = ap.parse_args()

    qmp = Qmp(args.socket)
    for i in range(args.samples):
        if i:
            time.sleep(args.interval)
        raw = qmp.hmp("info registers")
        regs = parse_regs(raw)
        pc = regs.get("pc", 0)
        print(f"[sample {i}] pc=0x{pc:08x} mstatus=0x{regs.get('mstatus', 0):08x} "
              f"mip=0x{regs.get('mip', 0):08x} mepc=0x{regs.get('mepc', 0):08x} "
              f"mcause=0x{regs.get('mcause', 0):08x} ra=0x{regs.get('ra', 0):08x} "
              f"sp=0x{regs.get('sp', 0):08x}")

        candidates = [pc, regs.get("ra", 0), regs.get("mepc", 0)]
        sp = regs.get("sp", 0)
        if sp and args.stack_words:
            try:
                candidates += qmp.read_words(sp, args.stack_words)
            except (RuntimeError, ValueError, OSError) as exc:
                print(f"    (stack read failed: {exc})")
        candidates = [a for a in candidates if looks_like_code(a)]
        # keep the first occurrence order, drop duplicates
        seen: list[int] = []
        for a in candidates:
            if a not in seen:
                seen.append(a)
        names = resolve(args.addr2line, args.elf, seen)
        for a in seen:
            marker = "<- pc" if a == pc else ("<- mepc" if a == regs.get("mepc") else "")
            print(f"    0x{a:08x} {names.get(a, '?')} {marker}".rstrip())
        if args.raw:
            print(raw)
    return 0


if __name__ == "__main__":
    sys.exit(main())
