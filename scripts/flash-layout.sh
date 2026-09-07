#!/usr/bin/env bash
#
# scripts/flash-layout.sh - single source of truth for the blinky flash layout.
#
# Collects the flash addresses used by the factory scripts from the project's
# authoritative address sources (no magic numbers):
#   - $BLINKY_DIR/build/flasher_args.json  -> bootloader / partition-table / otadata / app
#                                             (the exact file `idf.py flash` uses)
#   - $BLINKY_DIR/partitions.txt           -> nvs + ota_0 (+ any other partition)
#
# Source it (recommended):
#   BLINKY_DIR=/path/to/blinky . scripts/flash-layout.sh
#   -> sets/exportes FLASH_* variables in the current shell
#
# Execute it:
#   scripts/flash-layout.sh          # prints KEY=VALUE lines (handy for debug)
#
# Env:
#   BLINKY_DIR   blinky project dir (default: sibling of this script)
#
# Variables set:
#   FLASH_BOOTLOADER_OFFSET / FLASH_BOOTLOADER_FILE
#   FLASH_PARTITION_TABLE_OFFSET / FLASH_PARTITION_TABLE_FILE
#   FLASH_OTADATA_OFFSET
#   FLASH_APP_OFFSET / FLASH_APP_FILE          (app slot from flasher_args)
#   FLASH_NVS_OFFSET / FLASH_NVS_SIZE          (from partitions.txt)
#   FLASH_OTA0_OFFSET / FLASH_OTA0_SIZE        (ota_0 app partition from partitions.txt)
#
# Offsets/sizes are hex strings (0x...). File vars are absolute paths.

# Only enforce strictness when run directly (sourcing should not alter caller opts).
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  set -euo pipefail
fi

_FL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BLINKY_DIR="${BLINKY_DIR:-$(dirname "$_FL_DIR")/blinky}"

# Defaults (standard ESP-IDF) - overridden from the sources below when present.
FLASH_BOOTLOADER_OFFSET=0x1000
FLASH_PARTITION_TABLE_OFFSET=0x8000
FLASH_OTADATA_OFFSET=0xe000
FLASH_APP_OFFSET=0x10000
FLASH_NVS_OFFSET=0x9000
FLASH_NVS_SIZE=0x5000
FLASH_BOOTLOADER_FILE=""
FLASH_PARTITION_TABLE_FILE=""
FLASH_APP_FILE=""
FLASH_OTA0_OFFSET=""
FLASH_OTA0_SIZE=""

# --- 1) build/flasher_args.json (authoritative for the standard flash set) ---
if [[ -f "$BLINKY_DIR/build/flasher_args.json" ]]; then
  FA="$BLINKY_DIR/build/flasher_args.json"
  FLASH_BOOTLOADER_OFFSET="$(jq -r '.bootloader.offset' "$FA")"
  FLASH_PARTITION_TABLE_OFFSET="$(jq -r '."partition-table".offset' "$FA")"
  FLASH_OTADATA_OFFSET="$(jq -r '.otadata.offset' "$FA")"
  FLASH_APP_OFFSET="$(jq -r '.app.offset' "$FA")"
  FLASH_BOOTLOADER_FILE="$BLINKY_DIR/build/$(jq -r '.bootloader.file' "$FA")"
  FLASH_PARTITION_TABLE_FILE="$BLINKY_DIR/build/$(jq -r '."partition-table".file' "$FA")"
  FLASH_APP_FILE="$BLINKY_DIR/build/$(jq -r '.app.file' "$FA")"
fi

# --- 2) partitions.txt (authoritative for nvs + OTA app partitions) ---------
if [[ -f "$BLINKY_DIR/partitions.txt" ]]; then
  PT="$BLINKY_DIR/partitions.txt"
  FLASH_NVS_OFFSET="$(awk -F, '$1=="nvs"{gsub(/[ \t]/,"",$4); print $4}' "$PT")"
  FLASH_NVS_SIZE="$(awk -F, '$1=="nvs"{gsub(/[ \t]/,"",$5); print $5}' "$PT")"
  FLASH_OTADATA_OFFSET="$(awk -F, '$1=="otadata"{gsub(/[ \t]/,"",$4); print $4}' "$PT")"
  FLASH_OTA0_OFFSET="$(awk -F, '$1=="ota_0"{gsub(/[ \t]/,"",$4); print $4}' "$PT")"
  FLASH_OTA0_SIZE="$(awk -F, '$1=="ota_0"{gsub(/[ \t]/,"",$5); print $5}' "$PT")"
fi

export FLASH_BOOTLOADER_OFFSET FLASH_BOOTLOADER_FILE \
       FLASH_PARTITION_TABLE_OFFSET FLASH_PARTITION_TABLE_FILE \
       FLASH_OTADATA_OFFSET FLASH_APP_OFFSET FLASH_APP_FILE \
       FLASH_NVS_OFFSET FLASH_NVS_SIZE FLASH_OTA0_OFFSET FLASH_OTA0_SIZE

# --- run directly -> print KEY=VALUE for inspection -------------------------
if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  for _v in FLASH_BOOTLOADER_OFFSET FLASH_BOOTLOADER_FILE \
            FLASH_PARTITION_TABLE_OFFSET FLASH_PARTITION_TABLE_FILE \
            FLASH_OTADATA_OFFSET FLASH_APP_OFFSET FLASH_APP_FILE \
            FLASH_NVS_OFFSET FLASH_NVS_SIZE FLASH_OTA0_OFFSET FLASH_OTA0_SIZE; do
    printf '%s=%s\n' "$_v" "${!_v}"
  done
fi
