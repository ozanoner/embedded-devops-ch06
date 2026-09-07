#!/usr/bin/env bash
#
# factory-flash.sh - clean, factory flash of an ESP32 running blinky.
#
# Assumes you already built the firmware manually:  ( cd blinky && idf.py build )
#
# Performs (each step shows a banner, then waits for Enter; Ctrl+C aborts):
#   1. sign the app   : whole-image POST to SignServer -> signed bin, then verify
#   2. clean the flash: `esptool erase-flash` (wipes bootloader + everything)
#   3. write NVS      : scripts/factory-nvs.sh -> flash nvs image
#   4. write firmware : bootloader + partition table + signed app (no reset yet)
#   5. show serial    : reset the board and stream its serial output for
#                       SERIAL_TIMEOUT seconds (see wifi/enrollment live -
#                       not gated on enrollment success)
#
# Flash addresses come from scripts/flash-layout.sh (flasher_args.json +
# partitions.txt) - no magic numbers.
#
# Usage:
#   WIFI_SSID=mywifi WIFI_PWD=secret \
#   ENROLL_URL=https://192.168.1.50:9443/enroll \
#   bash scripts/factory-flash.sh
#
# Env:
#   KEYS_DIR        dir with ManagementCA.crt / client.crt / client.key
#                   (+ device-factory.crt/.key for factory-nvs.sh)
#   SIGN_URL        SignServer worker REST URL
#                   (default https://localhost:8444/signserver/rest/v1/workers/PlainSigner/process)
#   WIFI_SSID       WiFi SSID to provision into NVS (required)
#   WIFI_PWD        WiFi password to provision into NVS (required)
#   ENROLL_URL      enroll URL reachable FROM THE DEVICE (host LAN IP, NOT localhost)
#   PORT            serial port (default /dev/ttyUSB0)
#   SERIAL_TIMEOUT  seconds to stream serial in step 5 (default 60)
#   BLINKY_DIR      blinky project dir (default: sibling of scripts/)
#   FLASH_NO_PROMPT 1 = do not pause between steps (CI)
#
set -euo pipefail

# Shared config/helpers (SCRIPT_DIR/ROOT_DIR/BLINKY_DIR, .env, banner/pause
# helpers, esp_secure()/esp_tool(), load_flash_layout()). factory-common.sh
# normalizes SCRIPT_DIR itself, so a plain dirname is enough here.
# shellcheck disable=SC1091
. "$(dirname "${BASH_SOURCE[0]}")/factory-common.sh"

KEYS_DIR="${KEYS_DIR:-$ROOT_DIR/keys}"
SIGN_URL="${SIGN_URL:-https://localhost:8444/signserver/rest/v1/workers/PlainSigner/process}"
PORT="${PORT:-/dev/ttyUSB0}"
SERIAL_TIMEOUT="${SERIAL_TIMEOUT:-60}"
FLASH_NO_PROMPT="${FLASH_NO_PROMPT:-0}"

# Flash layout from the existing build (you build manually). Fails early if the
# build artifacts are missing.
load_flash_layout

OUT_DIR="$(mktemp -d)"
trap 'rm -rf "$OUT_DIR"' EXIT

# ----------------------------------------------------------------- preflight
note "Preflight"
for t in jq curl openssl; do
  command -v "$t" >/dev/null 2>&1 || { echo "error: '$t' not found" >&2; exit 1; }
done
# The firmware must already be built (this script no longer builds it).
for a in "$BLINKY_DIR/build/flasher_args.json" "$BLINKY_DIR/build/blinky.bin" \
         "$FLASH_BOOTLOADER_FILE" "$FLASH_PARTITION_TABLE_FILE"; do
  if [[ ! -s "$a" ]]; then
    echo "error: missing build artifact: $a" >&2
    echo "       Run 'idf.py build' in $BLINKY_DIR first." >&2
    exit 1
  fi
done
for f in ManagementCA.crt client.crt client.key device-factory.crt device-factory.key; do
  [[ -s "$KEYS_DIR/$f" ]] || { echo "error: missing $KEYS_DIR/$f" >&2; exit 1; }
done
need_env WIFI_SSID
need_env WIFI_PWD
need_env ENROLL_URL
if [[ "$ENROLL_URL" == *localhost* ]]; then
  echo "warning: ENROLL_URL uses 'localhost' - from the device that means the device"
  echo "         itself. Use the host's LAN IP reachable from the WiFi network."
fi
echo "  blinky dir : $BLINKY_DIR"
echo "  keys dir   : $KEYS_DIR"
echo "  port       : $PORT"
echo "  enroll url : $ENROLL_URL"
echo "  sign url   : $SIGN_URL"
echo "  layout     : boot @$FLASH_BOOTLOADER_OFFSET, pt @$FLASH_PARTITION_TABLE_OFFSET,"
echo "               app @$FLASH_APP_OFFSET, nvs @$FLASH_NVS_OFFSET (size $FLASH_NVS_SIZE)"

# --- build freshness (informational - this script does not build) ---------
app_build_mtime="$(stat -c %Y "$BLINKY_DIR/build/blinky.bin" 2>/dev/null || echo 0)"
src_newest="$(find "$BLINKY_DIR" -type f \
  \( -name '*.c' -o -name '*.h' -o -name '*.txt' -o -name 'CMakeLists.txt' -o -name '*.yml' \) \
  -not -path '*/build/*' -not -path '*/managed_components/*' \
  -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -1)"
src_mtime="${src_newest%% *}"
src_mtime="${src_mtime%.*}"
src_file="${src_newest#* }"
echo "  built app : $(date -d "@$app_build_mtime" '+%Y-%m-%d %H:%M:%S')  (build/blinky.bin)"
if [[ -n "$src_file" ]]; then
  echo "  newest src: $(date -d "@$src_mtime" '+%Y-%m-%d %H:%M:%S')  ($src_file)"
fi
if [[ -n "$src_mtime" ]] && (( src_mtime > app_build_mtime )); then
  echo
  echo "  WARNING: a source file is NEWER than the built app - flashing a STALE build."
  echo "           Run 'idf.py build' in $BLINKY_DIR first if the code changed."
  echo
fi

if [[ "$FLASH_NO_PROMPT" != "1" ]]; then
  echo
  read -r -p "   >>>  Everything look right? Press ENTER to start, Ctrl+C to abort...  " _
fi

# ------------------------------------------------------------ 1. sign app
step_header "STEP 1/5  Sign the app via SignServer" "whole-image POST -> signed binary, then verify"
pause
note "Uploading the full unsigned image to SignServer and requesting a signature ..."
jq -n --rawfile b64 <(base64 -w0 "$BLINKY_DIR/build/blinky.bin") \
  '{data:$b64, encoding:"BASE64"}' > "$OUT_DIR/req.json"
curl --fail-with-body --silent --show-error \
  --cacert "$KEYS_DIR/ManagementCA.crt" --cert "$KEYS_DIR/client.crt" --key "$KEYS_DIR/client.key" \
  -H 'X-Keyfactor-Requested-With: REST' -H 'Content-Type: application/json' -H 'Accept: application/json' \
  --data @"$OUT_DIR/req.json" "$SIGN_URL" > "$OUT_DIR/resp.json"
echo "  signature received from SignServer."
jq -r .signerCertificate "$OUT_DIR/resp.json" | base64 -d > "$OUT_DIR/signer-cert.der"
openssl x509 -inform DER -in "$OUT_DIR/signer-cert.der" -pubkey -noout > "$OUT_DIR/pub.pem"
jq -r .data "$OUT_DIR/resp.json" | base64 -d > "$OUT_DIR/signature.bin"
echo "  assembling the Secure Boot v2 signature block and verifying ..."
esp_secure sign-data --version 2 \
  --pub-key "$OUT_DIR/pub.pem" --signature "$OUT_DIR/signature.bin" \
  --output "$OUT_DIR/blinky.signed.bin" "$BLINKY_DIR/build/blinky.bin"
esp_secure verify-signature --version 2 --keyfile "$OUT_DIR/pub.pem" "$OUT_DIR/blinky.signed.bin"
SIGNED_BYTES="$(stat -c%s "$OUT_DIR/blinky.signed.bin")"
OTA_BYTES="$((FLASH_OTA0_SIZE))"
echo "  signed image: ${SIGNED_BYTES} bytes (fits the app slot @${FLASH_APP_OFFSET}, ${OTA_BYTES} bytes)"
if (( SIGNED_BYTES > OTA_BYTES )); then
  echo "error: signed image does NOT fit the app partition - enlarge it or shrink the app" >&2
  exit 1
fi

# ------------------------------------------------------- 2. clean the flash
step_header "STEP 2/5  Clean the flash" "esptool erase-flash (wipes the bootloader too - reflashed in step 4)"
pause
note "Erasing entire flash ..."
esp_tool --chip esp32 -p "$PORT" -b 460800 --before default-reset --after no-reset erase-flash
echo "  flash erased."

# -------------------------------------------------------------- 3. write NVS
step_header "STEP 3/5  Write factory NVS" "scripts/factory-nvs.sh -> generate + flash nvs image @ $FLASH_NVS_OFFSET"
pause
note "Generating the NVS image (factory identity + wifi + enroll URL) and flashing it @ $FLASH_NVS_OFFSET ..."
KEYS_DIR="$KEYS_DIR" WIFI_SSID="$WIFI_SSID" WIFI_PWD="$WIFI_PWD" ENROLL_URL="$ENROLL_URL" \
  BLINKY_DIR="$BLINKY_DIR" FLASH=1 bash "$SCRIPT_DIR/factory-nvs.sh" "$OUT_DIR/factory-nvs.bin"
echo "  NVS written."

# ----------------------------------------------------------- 4. write firmware
step_header "STEP 4/5  Write firmware" "bootloader @$FLASH_BOOTLOADER_OFFSET, partition table @$FLASH_PARTITION_TABLE_OFFSET, signed app @$FLASH_APP_OFFSET"
pause
note "Flashing bootloader + partition table + signed app (no reset yet) ..."
esp_tool --chip esp32 -p "$PORT" -b 460800 --before default-reset --after no-reset write-flash \
  "$FLASH_BOOTLOADER_OFFSET"      "$FLASH_BOOTLOADER_FILE" \
  "$FLASH_PARTITION_TABLE_OFFSET" "$FLASH_PARTITION_TABLE_FILE" \
  "$FLASH_APP_OFFSET"             "$OUT_DIR/blinky.signed.bin"
echo "  firmware written (otadata left erased -> boots $FLASH_APP_OFFSET)."

# ------------------------------------------------------------ 5. show serial
step_header "STEP 5/5  Device serial output" "reset the board, then stream $PORT for ${SERIAL_TIMEOUT}s (watch wifi/enrollment)"
pause
note "Resetting the board into the app ..."
esp_tool --chip esp32 -p "$PORT" -b 460800 --before default-reset --after hard-reset flash-id

note "Streaming serial output for up to ${SERIAL_TIMEOUT}s (Ctrl+C to stop early) ..."
if [[ -n "$IDF_PY_BIN" && -x "$IDF_PY_BIN/python" ]]; then
  SERIAL_PY="$IDF_PY_BIN/python"
else
  SERIAL_PY="python3"
fi
"$SERIAL_PY" - "$PORT" "$SERIAL_TIMEOUT" <<'PY'
import sys, time

try:
    import serial
except ImportError:
    print("[factory] error: 'pyserial' is not available in this Python interpreter.", file=sys.stderr)
    print("[factory]        use the ESP-IDF python env or install pyserial.", file=sys.stderr)
    sys.exit(2)

port = sys.argv[1]
duration = float(sys.argv[2])
print(f"[factory] watching {port} for {duration:.0f}s - Ctrl+C to stop early", flush=True)
ser = serial.Serial(port, 115200, timeout=0.2)
end = time.time() + duration
try:
    while time.time() < end:
        data = ser.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()
except KeyboardInterrupt:
    pass
finally:
    ser.close()
print()
PY
