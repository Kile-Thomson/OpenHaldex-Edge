#!/usr/bin/env bash
# Fast firmware flash for the ESP32-C6 that bypasses PlatformIO's dependency
# scanner (LDF). The .pio build tree lives in a Dropbox-synced folder and the
# LDF wedges reproducibly at "Scanning dependencies" on every `pio run -t upload`,
# so this writes the already-built firmware straight to the chip with esptool.
#
# It writes firmware.bin to the ota_0 slot (0x10000) and resets otadata (0xd000)
# with boot_app0.bin so the chip boots the freshly-flashed image regardless of
# which OTA slot was previously active.
#
# Usage:
#   scripts/flash.sh            # auto-detect the ESP32-C6 port
#   scripts/flash.sh COM4       # force a specific port
#
# Requires: a completed `pio run` (firmware.bin present) and a Python with the
# pyserial-backed esptool that PlatformIO already installed.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIRMWARE="$PROJECT_DIR/.pio/build/esp32c6/firmware.bin"

PARTITIONS="$PROJECT_DIR/.pio/build/esp32c6/partitions.bin"
PIO_PKGS="$HOME/.platformio/packages"
ESPTOOL="$PIO_PKGS/tool-esptoolpy/esptool.py"
BOOT_APP0="$PIO_PKGS/framework-arduinoespressif32/tools/partitions/boot_app0.bin"

# Pick a Python that actually has pyserial (esptool needs it). The `py`
# launcher often resolves to an interpreter without it, so probe each
# candidate and keep the first that can import serial.
PYTHON="${PYTHON:-}"
if [ -n "$PYTHON" ]; then
  "$PYTHON" -c "import serial" 2>/dev/null || { echo "ERROR: \$PYTHON=$PYTHON cannot import pyserial" >&2; exit 1; }
else
  for cand in python python3 py; do
    command -v "$cand" >/dev/null 2>&1 || continue
    if "$cand" -c "import serial" 2>/dev/null; then PYTHON="$cand"; break; fi
  done
  [ -n "$PYTHON" ] || { echo "ERROR: no python with pyserial found on PATH (set PYTHON=... to one that has esptool deps)" >&2; exit 1; }
fi

# ota_0 and otadata offsets are read from the partition table that was just
# built (src/partitions_4mb.csv), so a layout change needs no edit here.
[ -f "$PARTITIONS" ] || { echo "ERROR: missing $PARTITIONS (run pio run first)" >&2; exit 1; }
read -r OTA0_OFFSET OTADATA_OFFSET < <("$PYTHON" - "$PARTITIONS" <<'PY'
import struct, sys
data = open(sys.argv[1], "rb").read()
ota0 = otadata = None
for i in range(0, len(data) - 31, 32):
    e = data[i:i + 32]
    if e[0:2] != b"\xaa\x50":
        break  # MD5 marker or padding: end of the table
    ptype, subtype = e[2], e[3]
    offset = struct.unpack("<I", e[4:8])[0]
    if ptype == 0x00 and subtype == 0x10:
        ota0 = offset
    elif ptype == 0x01 and subtype == 0x00:
        otadata = offset
if ota0 is None or otadata is None:
    sys.exit("ota_0 or otadata not found in the partition table")
print("0x%x 0x%x" % (ota0, otadata))
PY
) || { echo "ERROR: could not read ota_0 / otadata from $PARTITIONS" >&2; exit 1; }

for f in "$FIRMWARE" "$ESPTOOL" "$BOOT_APP0"; do
  [ -f "$f" ] || { echo "ERROR: missing $f" >&2; exit 1; }
done

PORT="${1:-}"
if [ -z "$PORT" ]; then
  # Auto-detect: Espressif native USB-JTAG shows up as VID:PID 303A:1001.
  PORT="$("$PYTHON" - <<'PY'
import sys
try:
    from serial.tools import list_ports
except Exception:
    sys.exit(0)
for p in list_ports.comports():
    if (p.vid, p.pid) == (0x303A, 0x1001):
        print(p.device); break
PY
)"
  [ -n "$PORT" ] || { echo "ERROR: no ESP32-C6 (303A:1001) found - pass the port explicitly, e.g. scripts/flash.sh COM4" >&2; exit 1; }
  echo "Auto-detected ESP32-C6 on $PORT"
fi

echo "Flashing $FIRMWARE -> $OTA0_OFFSET on $PORT (LDF scan bypassed)"
"$PYTHON" "$ESPTOOL" --chip esp32c6 --port "$PORT" --baud 921600 \
  write_flash --flash_mode keep --flash_freq keep --flash_size keep \
  "$OTA0_OFFSET" "$FIRMWARE" \
  "$OTADATA_OFFSET" "$BOOT_APP0"

echo "Flash complete and verified."
