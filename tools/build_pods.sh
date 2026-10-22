#!/usr/bin/env bash
# =========================================================================
#  MESQUITE PHASE 2 - per-node pod build  (addresses NODE-05, §4.2 BLOCKER)
#
#  Generates one firmware image per bone id from a single source, with the
#  id injected as a build flag. Replaces the Phase 1 workflow of editing a
#  comment block 17 times, which is how duplicate sendIDs get created --
#  a failure that is SILENT at runtime.
#
#  Usage:
#     tools/build_pods.sh                 # build all 17
#     tools/build_pods.sh 3 4 5           # build only these ids
#     FQBN=... tools/build_pods.sh        # override board
#     EXTRA_FLAGS="-DMESQ_INSTR=1" tools/build_pods.sh 3   # a build variant
#     OUTROOT=/some/dir tools/build_pods.sh                # output elsewhere
#     ARDUINO_CLI=/path/to/arduino-cli tools/build_pods.sh # a CLI not on PATH
#     SOURCE_DATE_EPOCH=<unix time> tools/build_pods.sh    # fix the FW_BUILD stamp
#
#  Needs arduino-cli: from ARDUINO_CLI, else on PATH, else the copy bundled
#  inside Arduino IDE.app on macOS. Outputs to $OUTROOT/pod_<id>_<NAME>/
#  (default build/pods) and writes $OUTROOT/MANIFEST.txt, which the next run
#  replaces; a copy of each run's manifest stays in $OUTROOT/manifests/.
#
#  BUILD FLAGS. The id goes into compiler.cpp.extra_flags, NOT
#  build.extra_flags: the ESP32 core's platform.txt defines build.extra_flags
#  itself (-DESP32, the debug level, the loop and event cores, the board
#  defines), so overriding it drops -DESP32 and the build fails. The
#  compiler.c/cpp.extra_flags properties are empty in the core and exist for
#  exactly this use.
#
#  DMP SUPPORT. The SparkFun ICM-20948 library compiles its DMP code
#  only when ICM_20948_USE_DMP is defined. Upstream ships that line commented
#  out (src/util/ICM_20948_C.h), so an unedited library builds an image whose
#  DMP initialisation fails at boot. The define is passed here for the C and
#  C++ compilers. It is passed EMPTY (-DICM_20948_USE_DMP=) so that it is
#  identical to the library's own "#define ICM_20948_USE_DMP" on machines
#  where that line was uncommented by hand: no redefinition warning either way.
#
#  REPEATABLE IMAGES. The boot banner prints FW_BUILD from __DATE__
#  and __TIME__. Left alone, those put the compile time into every image, so
#  no two builds are ever identical: the duplicate-image check at the end
#  could never fire, and no image could be rebuilt to the same hash. GCC
#  takes both from SOURCE_DATE_EPOCH when it is set, so this script sets it
#  once per run: the commit time when the sketch folder and this script match
#  the commit, otherwise the time the run started. The stamp goes into the
#  manifest, so a pod's FW_BUILD and POD_ID lines point at one manifest line.
# =========================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

# macOS: the ESP32 core's build recipes call python3, and Apple's
# /usr/bin/python3 is a shim that refuses to run (exit 69) until the Xcode
# licence is accepted -- unless it is pointed at the Command Line Tools.
if [ "$(uname -s)" = "Darwin" ] && [ -z "${DEVELOPER_DIR:-}" ] \
   && [ -d /Library/Developer/CommandLineTools ]; then
  export DEVELOPER_DIR=/Library/Developer/CommandLineTools
fi

SKETCH="Device code/Pod_Watch_Binary"
OUTROOT="${OUTROOT:-build/pods}"
FQBN="${FQBN:-esp32:esp32:twatch}"
EXTRA_FLAGS="${EXTRA_FLAGS:-}"
DMP_FLAG="-DICM_20948_USE_DMP="

# The core and library versions this firmware has been built with for the
# watch. Others are allowed, with a warning, because a silent toolchain
# change is how unreproducible images are made.
TESTED_CORE="2.0.17"
TESTED_ICM_LIB="1.3.0"

NAMES=(Head Spine HipsAlt LeftArm LeftForeArm LeftHand RightArm RightForeArm \
       RightHand LeftUpLeg LeftLeg LeftFoot RightUpLeg RightLeg RightFoot \
       LeftShoulder RightShoulder)

CLI="${ARDUINO_CLI:-}"
if [ -z "$CLI" ]; then
  if command -v arduino-cli >/dev/null 2>&1; then
    CLI="$(command -v arduino-cli)"
  elif [ -x "/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli" ]; then
    CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
  fi
fi
if [ -z "$CLI" ] || [ ! -x "$CLI" ]; then
  echo "ERROR: arduino-cli not found (set ARDUINO_CLI, or put it on PATH)." >&2
  echo "  To build by hand, pass for each pod id:" >&2
  echo "    --build-property \"compiler.cpp.extra_flags=-DMESQ_POD_ID=<id> $DMP_FLAG\"" >&2
  echo "    --build-property \"compiler.c.extra_flags=$DMP_FLAG\"" >&2
  exit 127
fi

# ---- provenance: what is about to be built, and with what ----
cli_version=$("$CLI" version 2>/dev/null | head -1 || echo "unknown")
core_version=$("$CLI" core list 2>/dev/null | awk '$1=="esp32:esp32"{print $2}' || true)
icm_version=$("$CLI" lib list 2>/dev/null \
  | awk '/ICM 20948/{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+\.[0-9]+\.[0-9]+$/){print $i; exit}}' || true)
src_commit=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")
# Untracked files count: Arduino compiles every .ino/.c/.cpp in the folder.
if [ "$src_commit" = "unknown" ]; then
  src_state="unknown: not a git checkout"
elif [ -n "$(git status --porcelain --untracked-files=all -- "$SKETCH" tools/build_pods.sh \
              ':(exclude,glob)**/.DS_Store' 2>/dev/null)" ]; then
  src_state="modified: uncommitted changes in the sketch folder or this script"
else
  src_state="clean"
fi
if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
  if [ "$src_state" = "clean" ]; then
    SOURCE_DATE_EPOCH=$(git log -1 --format=%ct 2>/dev/null || true)
  fi
  [ -n "${SOURCE_DATE_EPOCH:-}" ] || SOURCE_DATE_EPOCH=$(date +%s)
fi
export SOURCE_DATE_EPOCH
# The same text the banner prints: GCC renders the stamp in UTC.
fw_stamp=$(LC_ALL=C date -u -r "$SOURCE_DATE_EPOCH" "+%b %e %Y %H:%M:%S" 2>/dev/null \
  || LC_ALL=C date -u -d "@$SOURCE_DATE_EPOCH" "+%b %e %Y %H:%M:%S" 2>/dev/null \
  || echo "unknown")
[ -n "$core_version" ] || core_version="unknown"
[ -n "$icm_version" ]  || icm_version="unknown"
if [ "$core_version" != "$TESTED_CORE" ]; then
  echo "WARNING: ESP32 core $core_version; this firmware was tested with $TESTED_CORE." >&2
fi
if [ "$icm_version" != "$TESTED_ICM_LIB" ]; then
  echo "WARNING: SparkFun ICM-20948 library $icm_version; this firmware was tested with $TESTED_ICM_LIB." >&2
fi
echo "Source $src_commit ($src_state); FW_BUILD stamp $fw_stamp UTC"

IDS=("$@")
if [ ${#IDS[@]} -eq 0 ]; then IDS=($(seq 0 16)); fi

mkdir -p "$OUTROOT"
MANIFEST="$OUTROOT/MANIFEST.txt"
run_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
: > "$MANIFEST"
{
  echo "# Mesquite pod build manifest - $run_utc"
  echo "# source: mesquite-mocap $src_commit ($src_state)"
  echo "# FW_BUILD in every banner: $fw_stamp (UTC; SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH)"
  echo "# toolchain: $cli_version; esp32:esp32 core $core_version; SparkFun ICM-20948 library $icm_version"
  echo "# fqbn: $FQBN"
  echo "# cpp flags: -DMESQ_POD_ID=<id> $DMP_FLAG $EXTRA_FLAGS"
  echo "# c flags: $DMP_FLAG"
  echo "# id  name           sha256(bin)"
} >> "$MANIFEST"

fail=0
first_log=""
for id in "${IDS[@]}"; do
  if [ "$id" -lt 0 ] || [ "$id" -gt 16 ]; then
    echo "SKIP invalid id $id" >&2; fail=1; continue
  fi
  name="${NAMES[$id]}"
  out="$OUTROOT/pod_${id}_${name}"
  echo "==> building id=$id ($name)"
  mkdir -p "$out"
  if "$CLI" compile --no-color \
        --fqbn "$FQBN" \
        --build-property "compiler.cpp.extra_flags=-DMESQ_POD_ID=${id} ${DMP_FLAG} ${EXTRA_FLAGS}" \
        --build-property "compiler.c.extra_flags=${DMP_FLAG}" \
        --output-dir "$out" \
        "$SKETCH" >"$out/build.log" 2>&1; then
    bin=$(ls "$out"/*.ino.bin 2>/dev/null | head -1 || true)
    if [ -n "$bin" ]; then
      sum=$(shasum -a 256 "$bin" | awk '{print $1}')
      printf "%-4s %-14s %s\n" "$id" "$name" "$sum" >> "$MANIFEST"
      echo "    ok  $sum"
      [ -n "$first_log" ] || first_log="$out/build.log"
    else
      echo "    WARNING: no .bin produced" >&2; fail=1
    fi
  else
    echo "    BUILD FAILED - see $out/build.log" >&2
    tail -5 "$out/build.log" >&2
    fail=1
  fi
done

# Every id compiles the same sketch, so the first image's list of libraries
# (versions and folders, as arduino-cli reports them) stands for all of them.
if [ -n "$first_log" ]; then
  echo "# libraries and platform used (arduino-cli, first image):" >> "$MANIFEST"
  awk '/^Used library/{p=1} p && NF {print "#   " $0}' "$first_log" \
    | sed "s#${HOME}#~#g" >> "$MANIFEST"
fi
# A partial rebuild replaces MANIFEST.txt while older images stay on disk, so
# keep every run's manifest: a pod's banner may match any of them.
mkdir -p "$OUTROOT/manifests"
kept="$OUTROOT/manifests/MANIFEST_$(echo "$run_utc" | tr -d ':-').txt"
cp "$MANIFEST" "$kept"

echo
echo "Manifest: $MANIFEST (kept as $kept)"
# Duplicate-binary check: two ids producing an identical image means the
# build flag did not take effect, which is exactly the NODE-05 failure.
# It can fire only because every image of a run carries the same FW_BUILD.
images=$(awk 'NF==3 && $1!="#"' "$MANIFEST" | wc -l | tr -d ' ')
dups=$(awk 'NF==3 && $1!="#"{print $3}' "$MANIFEST" | sort | uniq -d | wc -l | tr -d ' ')
if [ "$dups" != "0" ]; then
  echo "ERROR: $dups duplicate binaries in manifest -- MESQ_POD_ID did not take effect." >&2
  exit 1
fi
if [ "$images" = "0" ]; then
  echo "No images were built."
else
  echo "No duplicate images: all $images built are distinct."
fi
exit $fail
