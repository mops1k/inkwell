#!/usr/bin/env bash
# Boots the REAL firmware in Espressif's QEMU fork (ESP32-C3) and captures the
# UART log. Plan p_46c273a4a2e1, phase E2.
#
# What this proves: bootloader + application start, early init logs, absence of
# a boot-time crash in code that does not touch real peripherals.
# What it does NOT prove: UI, e-ink panel (UC8253) and SD card are not emulated,
# so display/SD init fails and the app settles into a watchdog reset loop. Never
# use this instead of the on-device acceptance (phase Ф7).
#
# Prerequisites (installed locally, no system changes):
#   .venv/qemu/qemu/bin/qemu-system-riscv32   Espressif QEMU 9.2.2 release
#   .venv/qemu/lib/libslirp.so.0              local copy, LD_LIBRARY_PATH only
#   .venv/bin/esptool                         for merge-bin
set -euo pipefail
cd "$(dirname "$0")/.."

QEMU_BIN=".venv/qemu/qemu/bin/qemu-system-riscv32"
QEMU_LIB="$PWD/.venv/qemu/lib"
FLASH="build/qemu/flash.bin"
SECONDS_TO_RUN="${1:-20}"

if [ ! -x "$QEMU_BIN" ]; then
  echo "QEMU missing at $QEMU_BIN (download the espressif/qemu riscv32 release into .venv/qemu)." >&2
  exit 1
fi
if [ ! -f .pio/build/default/firmware.bin ]; then
  echo "Firmware missing: run .venv/bin/pio run -e default first." >&2
  exit 1
fi

mkdir -p build/qemu
if [ ! -f "$FLASH" ] || [ .pio/build/default/firmware.bin -nt "$FLASH" ]; then
  echo "Building full flash image (bootloader + partitions + boot_app0 + app)..."
  .venv/bin/esptool --chip esp32c3 merge-bin -o "$FLASH" \
    --flash_mode dio --flash_freq 80m --flash_size 16MB --fill-flash-size 16MB \
    0x0 .pio/build/default/bootloader.bin \
    0x8000 .pio/build/default/partitions.bin \
    0xe000 "$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin" \
    0x10000 .pio/build/default/firmware.bin >/dev/null
fi

echo "Booting $SECONDS_TO_RUN s in QEMU (machine esp32c3)..."
set +e
timeout "$SECONDS_TO_RUN" env LD_LIBRARY_PATH="$QEMU_LIB" "$QEMU_BIN" \
  -nographic -machine esp32c3 -drive "file=$PWD/$FLASH,if=mtd,format=raw" \
  > build/qemu/boot.log 2>&1
status=$?
set -e

echo "qemu exit=$status (124 means the run was cut by the timeout, which is expected)"
echo '--- boot markers'
grep -m 3 -E "ESP-ROM|entry 0x" build/qemu/boot.log || true
echo '--- failures'
grep -m 5 -E "Guru Meditation|panic|abort|assert failed" build/qemu/boot.log || echo '(no panic in the captured window)'
echo "full log: build/qemu/boot.log"
