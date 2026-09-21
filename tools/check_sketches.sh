#!/usr/bin/env bash
# =========================================================================
#  tools/check_sketches.sh -- type-check both .ino sketches with g++
#
#  There is no arduino-cli here, so this is NOT a target build. It strips the
#  library #includes, substitutes tools/arduino_stubs/mesq_stubs.h, and asks
#  g++ to compile the sketch body. It catches syntax errors, type errors and
#  wrong call signatures. It does NOT prove the real libraries agree, that the
#  image links, or that anything works on hardware.
# =========================================================================
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT="$PWD"
STUB="$ROOT/tools/arduino_stubs/mesq_stubs.h"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
rc=0

check() {
  local ino="$1" name="$2" extra="${3:-}"
  local out="$WORK/$name.cpp"
  # Arduino auto-prototypes every function in a .ino; plain g++ does not, so
  # the helper below reproduces that and strips the stubbed library includes.
  python3 "$ROOT/tools/arduino_stubs/preprocess.py" "$ino" "$ROOT/tools/arduino_stubs" $extra > "$out" || { echo "PREPROCESS FAIL"; rc=1; return; }

  printf '  %-34s ' "$name${extra:+ [$extra]}"
  if g++ -std=c++17 -fsyntax-only -DMESQ_STUB_BUILD $extra \
       -I"$ROOT/tools/arduino_stubs" \
       -I"$ROOT/Device code/Pod_Watch_Binary" \
       -I"$ROOT/Device code/Dongle_Binary" \
       -Wno-write-strings -Wno-unused-value \
       "$out" 2> "$WORK/$name.err"; then
    echo "OK"
  else
    echo "FAIL"
    sed -n '1,25p' "$WORK/$name.err" | sed 's/^/      /'
    rc=1
  fi
}

echo "Type-checking sketches against tools/arduino_stubs/ (NOT a target build):"
check "$ROOT/Device code/Pod_Watch_Binary/Pod_Watch_Binary.ino"  pod         "-DMESQ_POD_ID=3"
check "$ROOT/Device code/Pod_Watch_Binary/Pod_Watch_Binary.ino"  pod_instr   "-DMESQ_POD_ID=3 -DMESQ_INSTR=1"
check "$ROOT/Device code/Pod_Watch_Binary/Pod_Watch_Binary.ino"  pod_sens02  "-DMESQ_POD_ID=3 -DMESQ_DISABLE_UNUSED_DMP_STREAMS=1"
check "$ROOT/Device code/Dongle_Binary/Dongle_Binary.ino"        hub
check "$ROOT/Device code/Dongle_Binary/Dongle_Binary.ino"        hub_instr   "-DMESQ_INSTR=1"
exit $rc
