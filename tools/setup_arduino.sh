#!/usr/bin/env bash
# =========================================================================
#  tools/setup_arduino.sh -- build two isolated arduino-cli environments
#
#  WHY TWO: the pod and the hub need DIFFERENT ESP32 core versions, and
#  arduino-cli installs one version per platform per data directory.
#
#    pod  T-Watch 2019   core 2.0.17  (TTGO_TWatch_Library's vendored
#                                      TFT_eSPI does not build on IDF 5.x)
#    hub  T-Display-S3   core 3.x     (sketch and async libs target it)
#
#  Nothing is installed into ~/Arduino or ~/.arduino15, so this cannot
#  disturb the Arduino IDE or anything else already set up.
#
#  Usage:  tools/setup_arduino.sh [pod|hub|both]
# =========================================================================
set -uo pipefail
cd "$(dirname "$0")/.."
TARGET="${1:-both}"

CLI="$(command -v arduino-cli || true)"
[ -n "$CLI" ] || CLI="$HOME/Downloads/squashfs-root/resources/app/lib/backend/resources/arduino-cli"
[ -x "$CLI" ] || { echo "arduino-cli not found. Set CLI in this script."; exit 1; }
echo "Using: $CLI ($("$CLI" version | head -1))"

setup_pod() {
  local C; C="$(tools/arduino_env/mkconfig.sh pod)"
  echo
  echo "=== POD environment (core 2.0.17) ==============================="
  "$CLI" --config-file "$C" core update-index
  "$CLI" --config-file "$C" core install esp32:esp32@2.0.17
  "$CLI" --config-file "$C" lib install "SparkFun 9DoF IMU Breakout - ICM 20948 - Arduino Library"
  "$CLI" --config-file "$C" lib install "WebSockets"
  # Not in the Library Manager -- must be cloned.
  local LIBDIR="$HOME/.mesquite-arduino/pod/user/libraries"
  mkdir -p "$LIBDIR"
  if [ ! -d "$LIBDIR/TTGO_TWatch_Library" ]; then
    git clone --depth 1 https://github.com/Xinyuan-LilyGO/TTGO_TWatch_Library \
      "$LIBDIR/TTGO_TWatch_Library"
  else
    echo "TTGO_TWatch_Library already present"
  fi
}

setup_hub() {
  local C; C="$(tools/arduino_env/mkconfig.sh hub)"
  echo
  echo "=== HUB environment (core 3.x) =================================="
  "$CLI" --config-file "$C" core update-index
  "$CLI" --config-file "$C" core install esp32:esp32
  # mathieucarbou's forks -- the ones maintained for core 3.x. Other forks
  # (lacamera, dvarrel) fail with mbedtls _ret and pxCurrentTCB errors.
  "$CLI" --config-file "$C" lib install "AsyncTCP"
  "$CLI" --config-file "$C" lib install "ESPAsyncWebServer"
}

case "$TARGET" in
  pod)  setup_pod ;;
  hub)  setup_hub ;;
  both) setup_pod; setup_hub ;;
  *) echo "usage: $0 [pod|hub|both]"; exit 1 ;;
esac

echo
echo "Done. Build with:"
echo "  tools/build_pod.sh <0..16> [port]"
echo "  tools/build_hub.sh [port]"
