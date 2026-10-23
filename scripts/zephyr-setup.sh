#!/usr/bin/env bash
# Creates (or updates) the Zephyr workspace of the edge firmware from firmware/eop-edge/west.yml.
#
#   scripts/zephyr-setup.sh [--esp32]
#
# The workspace lives outside the repository, in $EOP_ZEPHYR_WS (default ~/.cache/eop-zephyr-ws),
# with its own Python virtualenv. --esp32 also fetches the Espressif HAL and its binary blobs for
# the board build (the native_sim build does not need them). Afterwards:
#   source "$EOP_ZEPHYR_WS/.venv/bin/activate"; export ZEPHYR_BASE="$EOP_ZEPHYR_WS/zephyr"
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WS="${EOP_ZEPHYR_WS:-$HOME/.cache/eop-zephyr-ws}"
MANIFEST_DIR="$WS/eop-edge-manifest"
ESP32=0
[[ "${1:-}" == "--esp32" ]] && ESP32=1

mkdir -p "$MANIFEST_DIR"
cp "$ROOT/firmware/eop-edge/west.yml" "$MANIFEST_DIR/west.yml"

if [[ ! -x "$WS/.venv/bin/west" ]]; then
    python3 -m venv "$WS/.venv"
    "$WS/.venv/bin/pip" install --quiet west
fi
# shellcheck disable=SC1091
source "$WS/.venv/bin/activate"

[[ -d "$WS/.west" ]] || west init -l "$MANIFEST_DIR"
cd "$WS"
if (( ESP32 )); then
    west config manifest.group-filter -- +hal
fi
west update --narrow --fetch-opt=--depth=1
pip install --quiet -r "$WS/zephyr/scripts/requirements-base.txt"
pip install --quiet -r "$ROOT/firmware/eop-edge/requirements-test.txt"
if (( ESP32 )); then
    west blobs fetch hal_espressif
fi

echo "zephyr-setup: workspace ready in $WS (zephyr $(west list zephyr -f "{revision}"))"
