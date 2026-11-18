#!/usr/bin/env bash
# Boots the REAL Xteink X3 firmware on the QEMU built from source (Q2..Q4).
#
# Machine model (see tools/qemu/README.md):
#   - flash on SPI0/SPI1 as an MTD drive (bootloader + partitions + app + NVS)
#   - UC8253 e-paper panel and microSD card on GP-SPI2, software CS through the
#     GPIO matrix
#   - three I2C chips (BQ27220 fuel gauge at 0x55, DS3231 RTC, QMI8658 IMU)
#
# Outputs (all under build/qemu/):
#   boot.log   QEMU's own messages: the model traces ([UC8253], [X3I2C], ...)
#   uart.log   the guest's UART0: ROM banner, ESP-IDF/Arduino logs
#   frame.bin  panel RAM dump, one record per DISPLAY_REFRESH
#              (render it with tools/qemu/panel-frame.py)
#
# Environment:
#   QEMU_FRESH_FLASH=1  throw the flash image away (blank NVS, board probe runs)
#   QEMU_FRESH_SD=1     rebuild the SD image with tools/qemu/make-sd-image.py
#   QEMU_QMP=<path>     also expose a QMP socket (tools/qemu/qmp-probe.py)
#   XTEINK_PANEL_*      panel model knobs, see tools/qemu/README.md
#
# Usage: tools/qemu/boot.sh [seconds] [extra qemu args...]
set -euo pipefail
cd "$(dirname "$0")/../.."

QEMU_BIN=".venv/qemu/src/build/qemu-system-riscv32"
QEMU_LIB="$PWD/.venv/qemu/lib"
FLASH="build/qemu/flash.bin"
SD_IMG="build/qemu/sd.img"
LOG="build/qemu/boot.log"
UART_LOG="build/qemu/uart.log"
FRAME="build/qemu/frame.bin"
SECONDS_TO_RUN="${1:-20}"
shift || true

if [ ! -x "$QEMU_BIN" ]; then
  echo "QEMU missing at $QEMU_BIN: build it first (see tools/qemu/README.md)." >&2
  exit 1
fi

mkdir -p build/qemu
# Firmware artifacts: prefer a stable copy, because several agents share this
# checkout and .pio/build is regularly wiped or rebuilt underneath us.
FW_DIR="build/qemu/fw"
if [ ! -f "$FW_DIR/firmware.bin" ]; then
  if [ -f .pio/build/default/firmware.bin ]; then
    FW_DIR=".pio/build/default"
  else
    echo "Firmware missing: run .venv/bin/pio run -e default first, or put" >&2
    echo "firmware.bin/bootloader.bin/partitions.bin into $FW_DIR." >&2
    exit 1
  fi
fi
# NVS (0x9000) also carries the firmware's cached board fingerprint
# (namespace "cphw", key "dev_det"): once it holds "X4", HalGPIO::begin()
# skips the I2C fingerprint entirely and the run never touches the I2C model.
# QEMU_FRESH_FLASH=1 therefore throws the image away so NVS starts blank and
# the probe runs again.
if [ "${QEMU_FRESH_FLASH:-0}" = "1" ] && [ -f "$FLASH" ]; then
  echo "QEMU_FRESH_FLASH=1: discarding $FLASH (blank NVS, probe will run)"
  rm -f "$FLASH"
fi
if [ ! -f "$FLASH" ] || [ "$FW_DIR/firmware.bin" -nt "$FLASH" ]; then
  echo "Building full flash image (bootloader + partitions + boot_app0 + app)..."
  .venv/bin/esptool --chip esp32c3 merge-bin -o "$FLASH" \
    --flash_mode dio --flash_freq 80m --flash_size 16MB --fill-flash-size 16MB \
    0x0 "$FW_DIR/bootloader.bin" \
    0x8000 "$FW_DIR/partitions.bin" \
    0xe000 "$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin" \
    0x10000 "$FW_DIR/firmware.bin" >/dev/null
fi

# The card needs a filesystem, not just card identification: on a blank image
# SdFat finds no volume, Storage.begin() fails and the library index never
# starts. make-sd-image.py writes a FAT16 card with /books/test.txt and
# /.crosspoint/ (no mkfs.vfat/mtools needed, and the image is reproducible).
if [ ! -f "$SD_IMG" ] || [ "${QEMU_FRESH_SD:-0}" = "1" ]; then
  echo "Creating FAT16 SD image at $SD_IMG"
  # Real books for emulator testing: every file in build/qemu/sd-books/ is copied
  # into /books on the card. Kept out of git (build/ is ignored) because test
  # books are user content, not fixtures.
  BOOK_ARGS=()
  if [ -d build/qemu/sd-books ]; then
    for book in build/qemu/sd-books/*; do
      [ -f "$book" ] || continue
      BOOK_ARGS+=(--add "$book:/books/$(basename "$book")")
    done
  fi
  tools/qemu/make-sd-image.py --out "$SD_IMG" "${BOOK_ARGS[@]}"
fi

QMP_ARGS=()
if [ -n "${QEMU_QMP:-}" ]; then
  rm -f "$QEMU_QMP"
  QMP_ARGS=(-qmp "unix:$QEMU_QMP,server=on,wait=off")
  echo "QMP socket: $QEMU_QMP (probe with tools/qemu/qmp-probe.py)"
fi

# Serial goes to its own file, not to stdio: -nographic muxes UART onto
# stdin/stdout, and when the run is scripted without a tty stdin closes at once
# and every guest log line is lost.
rm -f "$FRAME"

echo "Booting $SECONDS_TO_RUN s in QEMU (machine esp32c3)..."
set +e
# SSISD_TRACE is on by default: 200 bounded lines are what proves the card was
# mounted (CMD0/8/41/58 then CMD18 reads of MBR, BPB and directory clusters).
# XTEINK_PANEL_TRACE_BUSY prints each BUSY pulse with its virtual timestamp, i.e.
# the edge the firmware's completion ISR wakes on. Set either to 0 to silence it.
timeout "$SECONDS_TO_RUN" env LD_LIBRARY_PATH="$QEMU_LIB" \
  SSISD_TRACE="${SSISD_TRACE:-1}" \
  XTEINK_PANEL_TRACE_BUSY="${XTEINK_PANEL_TRACE_BUSY:-1}" "$QEMU_BIN" \
  -display none -monitor none \
  -serial "file:$PWD/$UART_LOG" \
  -machine "esp32c3,xteink-panel=xteink-panel" \
  -drive "file=$PWD/$FLASH,if=mtd,format=raw" \
  -drive "file=$PWD/$SD_IMG,if=sd,format=raw" \
  -chardev "file,id=xteink-panel,path=$PWD/$FRAME" \
  "${QMP_ARGS[@]}" \
  "$@" > "$LOG" 2>&1
status=$?
set -e

echo "qemu exit=$status (124 means the run was cut by the timeout, which is expected)"
echo '--- boot markers (uart.log)'
grep -m 3 -E "ESP-ROM|entry 0x" "$UART_LOG" || true
echo '--- board fingerprint (I2C, boot.log)'
grep -m 6 -E "\[X3I2C\]" "$LOG" || echo '(no I2C fingerprint traffic: cached verdict in NVS? use QEMU_FRESH_FLASH=1)'
echo '--- panel (boot.log)'
grep -m 8 -E "\[UC8253\]" "$LOG" || echo '(no panel traffic captured)'
echo '--- panel command sequence (0x00 = UC8253/X3 profile, 0x12 first = X4)'
grep -o "cmd 0x[0-9a-f]* (#[0-9]*" "$LOG" | head -8 || true
echo '--- last panel commands + BUSY (did the firmware get past DRF?)'
grep -E "\[UC8253\]" "$LOG" | tail -6 || true
{ grep -c "BUSY -> 0" "$LOG" || true; } | sed 's/^/BUSY pulses raised: /'
echo '--- panel frame'
if [ -s "$FRAME" ]; then
  tools/qemu/panel-frame.py --input "$FRAME" --stats-only --frame -1 || true
else
  echo '(no frame dumped: no DISPLAY_REFRESH reached the panel)'
fi
echo '--- SD: card init (boot.log)'
grep -m 8 -E "ssi_sd:.*(cmd=(0|8|41|58)|r1 cmd=58)" "$LOG" || echo '(no SD trace; SSISD_TRACE=0?)'
echo '--- SD: filesystem reads (mount proof)'
grep -m 6 -E "ssi_sd:.*(cmd=18|read done)" "$LOG" || echo '(no filesystem traffic: card not mounted)'
echo '--- failures (uart.log)'
grep -m 5 -E "Guru Meditation|panic|abort|assert failed|Interrupt wdt" "$UART_LOG" || echo '(no panic in the captured window)'
echo "full logs: $LOG (QEMU), $UART_LOG (guest UART), $FRAME (panel frames)"
