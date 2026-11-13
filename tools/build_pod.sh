#!/usr/bin/env bash
# Build (and optionally upload) one pod image.
#   tools/build_pod.sh 3                 compile only
#   tools/build_pod.sh 3 /dev/ttyUSB0    compile + upload
set -uo pipefail
cd "$(dirname "$0")/.."
ID="${1:?usage: $0 <0..16> [port]}"
PORT="${2:-}"
CLI="$(command -v arduino-cli || echo "$HOME/Downloads/squashfs-root/resources/app/lib/backend/resources/arduino-cli")"

# NOTE: compiler.cpp.extra_flags, NOT build.extra_flags.
# build.extra_flags REPLACES the board's own defines (ARDUINO_USB_CDC_ON_BOOT
# and friends), which silently breaks library platform detection -- WebSockets
# falls through to its Ethernet branch and fails on a missing Ethernet.h.
CFG="$(tools/arduino_env/mkconfig.sh pod)"
ARGS=(--config-file "$CFG" compile
      --fqbn esp32:esp32:twatch
      --build-property "compiler.cpp.extra_flags=-DMESQ_POD_ID=$ID")
[ -n "$PORT" ] && ARGS+=(--upload -p "$PORT")
ARGS+=(new_device_code/Pod_Watch_Binary)

echo "pod id $ID${PORT:+ -> $PORT}"
exec "$CLI" "${ARGS[@]}"
