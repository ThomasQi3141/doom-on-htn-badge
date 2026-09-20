#!/usr/bin/env bash
# Run this FIRST, before writing anything to the badge.
#
#   1. identifies the chip and reads its eFuse security state
#   2. dumps all 4 MB of stock flash to backup/
#
# If secure boot is enabled, custom firmware cannot run and this script says so.
# The flash dump is the only way back to the stock Lua badge, including its
# provisioned identity and contacts. Do not skip it.
#
# Usage:  tools/flash/badge-safety-check.sh [--port /dev/cu.usbmodemXXXX]

set -euo pipefail

# Python with esptool installed: $PY, else ~/esp/esp-venv, else whatever
# python3 is on PATH.
PY="${PY:-}"
if [[ -z "$PY" ]]; then
  if [[ -x "$HOME/esp/esp-venv/bin/python" ]]; then PY="$HOME/esp/esp-venv/bin/python"
  else PY="$(command -v python3)"; fi
fi
if ! "$PY" -m esptool version >/dev/null 2>&1; then
  echo "esptool not found for $PY"
  echo "Create a venv with:  python3 -m venv ~/esp/esp-venv && ~/esp/esp-venv/bin/pip install esptool"
  exit 1
fi
OUT="${OUT:-backup}"
PORT="${PORT:-}"

[[ "${1:-}" == "--port" ]] && PORT="$2"

if [[ -z "$PORT" ]]; then
  PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
fi
if [[ -z "$PORT" ]]; then
  cat <<'MSG'
No badge found on /dev/cu.usbmodem*.

Power the badge off, connect a USB *data* cable (not charge-only), power it on
normally, and retry. If it still does not appear, hold START while powering on
to force ROM download mode -- START is GPIO9, the ESP32-C3 boot strap.
MSG
  exit 1
fi

esptool()  { "$PY" -m esptool  --port "$PORT" "$@"; }
espefuse() { "$PY" -m espefuse --chip esp32c3 --port "$PORT" "$@"; }

mkdir -p "$OUT"
stamp=$(date +%Y%m%d-%H%M%S)
echo "Badge on $PORT -- writing to $OUT/"

echo
echo "=== chip identity ==="
esptool chip-id 2>&1 | tee "$OUT/chip-id-$stamp.txt"

echo
echo "=== security state: THIS IS THE GO / NO-GO ==="
esptool get-security-info 2>&1 | tee "$OUT/security-$stamp.txt" || \
  echo "(get-security-info unsupported by this ROM; see eFuse summary below)"

echo
echo "=== eFuse summary ==="
espefuse summary 2>&1 | tee "$OUT/efuse-$stamp.txt" || \
  echo "(espefuse failed; rely on get-security-info above)"

echo
echo "--- verdict ---"
if grep -qiE "SECURE_BOOT_EN .*= True|Secure Boot: +Enabled" "$OUT/efuse-$stamp.txt" "$OUT/security-$stamp.txt" 2>/dev/null; then
  echo "SECURE BOOT IS ENABLED. Custom firmware cannot be run on this badge."
  echo "Stopping before the flash dump -- nothing here is worth doing."
  exit 2
fi
if grep -qiE "SPI_BOOT_CRYPT_CNT .*= *[1-9]|Flash Encryption: +Enabled" "$OUT/efuse-$stamp.txt" "$OUT/security-$stamp.txt" 2>/dev/null; then
  echo "WARNING: flash encryption appears enabled. The dump below will be"
  echo "ciphertext and a plain re-flash may not boot. Read the eFuse summary"
  echo "carefully before writing anything."
fi
echo "No secure-boot fuse detected -- custom firmware should be able to run."

echo
echo "=== backing up all 4 MB of stock flash (takes ~1 min) ==="
esptool --baud 921600 read-flash 0 0x400000 "$OUT/stock-flash-$stamp.bin"

shasum -a 256 "$OUT/stock-flash-$stamp.bin" | tee "$OUT/stock-flash-$stamp.sha256"
ls -l "$OUT/stock-flash-$stamp.bin"

cat <<MSG

Backup complete.

Restore the stock badge at any time with:
  $PY -m esptool --port $PORT --baud 921600 write-flash 0 $OUT/stock-flash-$stamp.bin
MSG
