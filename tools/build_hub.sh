#!/usr/bin/env bash
# Build (and optionally upload) the hub image.
#   tools/build_hub.sh                   compile only
#   tools/build_hub.sh /dev/ttyACM0      compile + upload
set -uo pipefail
cd "$(dirname "$0")/.."
PORT="${1:-}"
CLI="$(command -v arduino-cli || echo "$HOME/Downloads/squashfs-root/resources/app/lib/backend/resources/arduino-cli")"

CFG="$(tools/arduino_env/mkconfig.sh hub)"
ARGS=(--config-file "$CFG" compile
      --fqbn esp32:esp32:lilygo_t_display_s3)
[ -n "$PORT" ] && ARGS+=(--upload -p "$PORT")
ARGS+=(new_device_code/Dongle_Binary)

echo "hub${PORT:+ -> $PORT}"
exec "$CLI" "${ARGS[@]}"
