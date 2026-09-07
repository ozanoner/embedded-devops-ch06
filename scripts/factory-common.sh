#!/usr/bin/env bash
#
# scripts/factory-common.sh - shared helpers/config for the ESP32 factory
# scripts (factory-flash.sh, factory-nvs.sh).
#
# SOURCE it (do not execute):
#   . "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/factory-common.sh"
#
# On source it:
#   - sets SCRIPT_DIR / ROOT_DIR / BLINKY_DIR (override BLINKY_DIR via env)
#   - loads .env defaults into the environment (real exported env vars win)
#   - defines helpers: note / step_header / pause / need_env
#   - resolves ESP-IDF tooling and defines esp_secure() / esp_tool()
#   - defines load_flash_layout() - call it AFTER a build so the flash file
#     paths from build/flasher_args.json actually exist (sets FLASH_* vars)
#
# Executing it directly just prints a short note.

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  echo "factory-common.sh is meant to be sourced by the factory scripts." >&2
  exit 0
fi

# --- directories ------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="${ROOT_DIR:-$(dirname "$SCRIPT_DIR")}"
BLINKY_DIR="${BLINKY_DIR:-$ROOT_DIR/blinky}"

# --- .env loader ------------------------------------------------------------
# `source .env` alone only sets non-exported shell vars, which child scripts do
# not inherit - so load it here, exporting each value (real env wins).
load_dotenv() {
  local f="$ROOT_DIR/.env" k v
  [[ -f "$f" ]] || return 0
  # `|| [[ -n "$k" ]]` also processes a final line that lacks a trailing newline.
  while IFS='=' read -r k v || [[ -n "$k" ]]; do
    k="${k%% *}"
    [[ "$k" =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] || continue
    [[ -n "${!k:-}" ]] && continue
    v="${v%\"}"; v="${v#\"}"
    v="${v%\'}"; v="${v#\'}"
    export "$k=$v"
  done < "$f"
}
load_dotenv

# --- flash layout ------------------------------------------------------------
# Loads FLASH_* vars (bootloader/partition-table/otadata/app/nvs addresses)
# from scripts/flash-layout.sh (build/flasher_args.json + partitions.txt).
load_flash_layout() {
  # shellcheck disable=SC1091
  . "$SCRIPT_DIR/flash-layout.sh"
}

# --- UI helpers --------------------------------------------------------------
note() { printf '\n== %s\n' "$1"; }
step_header() {
  echo
  echo "=============================================================="
  echo "   $1"
  echo "   $2"
  echo "=============================================================="
}
pause() {
  [[ "${FLASH_NO_PROMPT:-0}" == "1" ]] && { echo "    (no-prompt mode)"; return 0; }
  echo
  read -r -p "   >>>  Press ENTER to run this step, or Ctrl+C to abort...  " _
  echo "   -----------------------------------------------------------"
}
need_env() {
  [[ -n "${!1:-}" ]] || {
    echo "error: $1 is required (export it, or add it to $ROOT_DIR/.env)" >&2
    exit 1
  }
}

# --- ESP-IDF tooling (prefer the IDF python env, fall back to PATH) ---------
IDF_PY_BIN=""
for d in "${IDF_PYTHON_ENV_DIR:+$IDF_PYTHON_ENV_DIR/bin}" /opt/esp/python_env/idf6.0_py3.12_env/bin; do
  [[ -n "$d" && -x "$d/espsecure" ]] && { IDF_PY_BIN="$d"; break; }
done
esp_secure() {
  if [[ -n "$IDF_PY_BIN" ]]; then "$IDF_PY_BIN/espsecure" "$@"
  elif command -v espsecure >/dev/null; then espsecure "$@"
  else python3 -m espsecure "$@"; fi
}
esp_tool() {
  # Prefer 'esptool' (no '.py' suffix - avoids the deprecation warning).
  if [[ -n "$IDF_PY_BIN" && -x "$IDF_PY_BIN/esptool" ]]; then "$IDF_PY_BIN/esptool" "$@"
  elif command -v esptool >/dev/null 2>&1; then esptool "$@"
  elif [[ -n "$IDF_PY_BIN" && -x "$IDF_PY_BIN/esptool.py" ]]; then "$IDF_PY_BIN/esptool.py" "$@"
  elif command -v esptool.py >/dev/null 2>&1; then esptool.py "$@"
  else python3 -m esptool "$@"; fi
}
