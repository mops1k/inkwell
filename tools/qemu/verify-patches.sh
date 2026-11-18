#!/usr/bin/env bash
# Round-trip check of the fork patch set (Q4/Q5/Q6/Q7).
#
#   1. unpack the pristine Espressif tarball into build/qemu/q4/verify
#   2. apply 0001..0025 with `patch -p0 --forward` (must be 0 failures)
#   3. compare md5 of every file any patch touches with the working tree
#   4. reverse 0025..0017 and compare the Q4/Q5/Q6/Q7 files with the pre-Q4 tree
#
# The pre-Q4 reference (build/qemu/q4/clean) is a second copy of the tarball with
# 0001..0016 applied; a mismatch there means Q4, Q5, Q6 or Q7 changed something
# it should not. `--make-pre` builds that reference (one more extraction,
# ~2.8 GB) instead of skipping the comparison, `--keep` leaves the trees in place
# for inspection. Everything it writes lives in build/qemu/q4/ (gitignored).
set -uo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
Q4=$ROOT/build/qemu/q4
V=$Q4/verify
PRE=$Q4/clean          # fresh tarball + 0001..0016 == pre-Q4 state
PATCHES=$ROOT/tools/qemu/patches
WORK=$ROOT/.venv/qemu/src
TARBALL=$ROOT/.venv/qemu/qemu-esp_develop_9.2.2_20261109-src.tar.xz

MAKE_PRE=0
KEEP=0
for arg in "$@"; do
  case "$arg" in
    --make-pre) MAKE_PRE=1 ;;
    --keep) KEEP=1 ;;
    -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done

rm -rf "$V"
mkdir -p "$V"
echo "== extracting tarball to $V"
tar -xf "$TARBALL" -C "$V" --strip-components=1 || exit 1

if [ "$MAKE_PRE" = "1" ] || [ ! -d "$PRE" ]; then
  if [ "$MAKE_PRE" = "1" ]; then
    echo "== building the pre-Q4 reference in $PRE (0001..0016)"
    rm -rf "$PRE"
    mkdir -p "$PRE"
    tar -xf "$TARBALL" -C "$PRE" --strip-components=1 || exit 1
    for p in "$PATCHES"/00{01,02,03,04,05,06,07,08,09,10,11,12,13,14,15,16}-*.patch; do
      (cd "$PRE" && patch -p0 --forward -s < "$p") || echo "PRE_APPLY_FAILED $(basename "$p")"
    done
  else
    echo "== no pre-Q4 reference at $PRE: skipping the reverse comparison (use --make-pre)"
  fi
fi

echo "== applying 0001..0025"
fails=0
for p in "$PATCHES"/00*.patch; do
  out=$(cd "$V" && patch -p0 --forward < "$p" 2>&1)
  rc=$?
  printf '%-60s rc=%s\n' "$(basename "$p")" "$rc"
  if [ $rc -ne 0 ]; then
    echo "$out" | head -8
    fails=$((fails + 1))
  fi
done
echo "FORWARD_FAILS=$fails"

echo "== md5: patched tree vs working tree (every file any patch touches)"
files=$(grep -h '^+++ ' "$PATCHES"/00*.patch | sed 's|^+++ ||' | sed 's/[[:space:]]*$//' | sort -u)
mismatch=0
for f in $files; do
  a=$(md5sum < "$V/$f" 2>/dev/null)
  b=$(md5sum < "$WORK/$f" 2>/dev/null)
  if [ -z "$a" ] || [ "$a" != "$b" ]; then
    echo "MISMATCH $f"
    mismatch=$((mismatch + 1))
  fi
done
echo "TOUCHED_FILES=$(echo "$files" | wc -l) MISMATCHES=$mismatch"

echo "== reverse 0025..0017 -> compare with pre-Q4 tree"
revfail=0
for p in $(ls -r "$PATCHES"/00{17,18,19,20,21,22,23,24,25}-*.patch); do
  out=$(cd "$V" && patch -p0 -R < "$p" 2>&1)
  rc=$?
  printf '%-60s reverse rc=%s\n' "$(basename "$p")" "$rc"
  if [ $rc -ne 0 ]; then
    echo "$out" | head -8
    revfail=$((revfail + 1))
  fi
done
echo "REVERSE_FAILS=$revfail"

q4files="include/hw/gpio/esp32_gpio.h hw/gpio/esp32_gpio.c include/hw/gpio/esp32c3_gpio.h hw/gpio/esp32c3_gpio.c include/hw/display/xteink_uc8253.h hw/display/xteink_uc8253.c hw/riscv/esp32c3.c hw/i2c/xteink_x3_i2c.c hw/sd/ssi-sd.c hw/sd/sd.c hw/misc/esp32c3_saradc.c"
premismatch=0
preskipped=0
if [ -d "$PRE" ]; then
  for f in $q4files; do
    a=$(md5sum < "$V/$f")
    b=$(md5sum < "$PRE/$f")
    if [ "$a" != "$b" ]; then
      echo "PRE_Q4_MISMATCH $f"
      premismatch=$((premismatch + 1))
    fi
  done
else
  preskipped=1
  echo "PRE_Q4_SKIPPED (no reference tree at $PRE; run with --make-pre to build it)"
fi
echo "PRE_Q4_FILES=$(echo $q4files | wc -w) PRE_Q4_MISMATCHES=$premismatch PRE_Q4_SKIPPED=$preskipped"
echo "RESULT forward=$fails md5_mismatch=$mismatch reverse=$revfail pre_q4_mismatch=$premismatch pre_q4_skipped=$preskipped"

if [ "$KEEP" = "0" ]; then
  echo "== removing $Q4 (use --keep to inspect the trees)"
  rm -rf "$Q4"
fi
exit $(( fails || mismatch || revfail || premismatch ))
