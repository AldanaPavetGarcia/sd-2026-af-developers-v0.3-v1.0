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
# pip gives up on a slow download after 15 s by default; be patient and retry instead.
# pip >= 25.1 also resumes a download that stalls halfway (PIP_RESUME_RETRIES).
export PIP_DEFAULT_TIMEOUT=60 PIP_RETRIES=10 PIP_RESUME_RETRIES=10
# Large shallow fetches over HTTP/2 end in "early EOF" on unstable links; HTTP/1.1 gets through.
# Only for the git commands this script runs.
export GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=http.version GIT_CONFIG_VALUE_0=HTTP/1.1
ESP32=0
[[ "${1:-}" == "--esp32" ]] && ESP32=1

mkdir -p "$MANIFEST_DIR"
cp "$ROOT/firmware/eop-edge/west.yml" "$MANIFEST_DIR/west.yml"

if [[ ! -x "$WS/.venv/bin/west" ]]; then
    python3 -m venv "$WS/.venv"
    "$WS/.venv/bin/pip" install --quiet --upgrade "pip>=25.1"
    "$WS/.venv/bin/pip" install --quiet west
fi
# shellcheck disable=SC1091
source "$WS/.venv/bin/activate"

[[ -d "$WS/.west" ]] || west init -l "$MANIFEST_DIR"
cd "$WS"
if (( ESP32 )); then
    west config manifest.group-filter -- +hal
fi
# Fetching Zephyr is the step a flaky connection breaks (an early EOF ends the shallow fetch), so
# it is retried; west resumes from what it already has.
for attempt in 1 2 3; do
    west update --narrow --fetch-opt=--depth=1 && break
    (( attempt == 3 )) && { echo "zephyr-setup: west update failed 3 times" >&2; exit 1; }
    echo "zephyr-setup: west update failed (attempt $attempt), retrying" >&2
    sleep 10
done
pip install --quiet -r "$WS/zephyr/scripts/requirements-base.txt"
pip install --quiet -r "$ROOT/firmware/eop-edge/requirements-test.txt"
if (( ESP32 )); then
    west blobs fetch hal_espressif
    west packages pip --install    # the HAL's Python tools (esptool)
fi

echo "zephyr-setup: workspace ready in $WS (zephyr $(west list zephyr -f "{revision}"))"
