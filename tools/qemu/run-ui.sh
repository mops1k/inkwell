#!/usr/bin/env bash
# Opens the interactive X3 window: tools/qemu/x3-gui.py with a real firmware.
#
#   tools/qemu/run-ui.sh                     # build/qemu/fw/firmware.bin
#   tools/qemu/run-ui.sh path/to/firmware.bin
#   tools/qemu/run-ui.sh --fresh-flash       # extra arguments go to x3-gui.py
#
# The selected image is copied into build/qemu/fw/firmware.bin (with its .elf,
# so tools/qemu/qmp-probe.py can still decode the addresses). .pio/build is only
# ever read: when build/qemu/fw lacks bootloader.bin/partitions.bin they are
# copied there from .pio/build/default. The copy logic lives in x3emu.py — one
# implementation for the GUI, this script and the headless scenarios.
#
# Keys inside the window (see tools/qemu/README.md, "Q5"):
#   arrows Up/Down/Left/Right   Enter Confirm   Esc/Backspace Back   P Power
#   O  choose firmware…         R restart       F12 screenshot       Q quit
set -euo pipefail
cd "$(dirname "$0")/../.."

PY="${PYTHON:-python3}"
if ! command -v "$PY" >/dev/null 2>&1; then
  echo "python3 not found" >&2
  exit 1
fi

copy_firmware() {
  "$PY" - "$1" <<'PY'
import sys
sys.path.insert(0, "tools/qemu")
import x3emu

try:
    print(x3emu.install_firmware(sys.argv[1]))
except x3emu.EmulatorError as exc:
    print(f"run-ui.sh: {exc}", file=sys.stderr)
    sys.exit(1)
PY
}

if [ $# -gt 0 ] && [ "${1:0:2}" != "--" ]; then
  FW="$1"; shift
  [ -f "$FW" ] || { echo "run-ui.sh: no such firmware: $FW" >&2; exit 1; }
  copy_firmware "$FW"
elif [ ! -f build/qemu/fw/firmware.bin ]; then
  if [ -f .pio/build/default/firmware.bin ]; then
    echo "no build/qemu/fw/firmware.bin: taking the .pio/build/default build"
    copy_firmware .pio/build/default/firmware.bin
  else
    echo "run-ui.sh: no firmware. Pass a path, or build one:" >&2
    echo "  .venv/bin/pio run -e default" >&2
    exit 1
  fi
else
  # Even with a firmware in place, bootloader.bin/partitions.bin may be missing.
  copy_firmware build/qemu/fw/firmware.bin >/dev/null
fi

exec "$PY" tools/qemu/x3-gui.py "$@"
