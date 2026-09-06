#!/usr/bin/env bash
#
# Install the small toolset the self-hosted runner needs to sign firmware via
# the local SignServer: jq (distro package) and espsecure (esptool via uv).
# This is NOT the full ESP-IDF toolchain - just enough to assemble and verify
# an ESP32 Secure Boot v2 signature.
#
# Called by the "Install signing tools (jq + esptool)" step in
# .github/workflows/release.yml. Requires root on the runner (apt).
set -euo pipefail

export PATH="$HOME/.local/bin:$PATH"
mkdir -p "$HOME/.local/bin"

# jq: native package (requires root on the runner)
if ! command -v jq >/dev/null 2>&1; then
  apt-get update -y
  apt-get install -y jq
fi

# espsecure (esptool): via uv - a single static binary that manages its own
# Python, so we don't depend on system pip and get the esptool 5.x that apt
# does not provide.
if ! command -v espsecure >/dev/null 2>&1; then
  curl -LsSf https://astral.sh/uv/install.sh | sh
  export PATH="$HOME/.local/bin:$PATH"
  uv tool install --quiet esptool
  export PATH="$HOME/.local/bin:$PATH"
fi

command -v jq
command -v espsecure
espsecure --help >/dev/null 2>&1
