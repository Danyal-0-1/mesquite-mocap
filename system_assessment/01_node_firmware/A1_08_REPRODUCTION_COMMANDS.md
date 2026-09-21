# A1_08 — Reproduction Commands

Every command runs from the repository root. No secrets are required.

## Everything at once

```bash
tools/run_all_tests.sh
```
Runs all five host suites. Expected: **189 assertions, 0 failures**, and a closing
line stating that no hardware was exercised.

## Individually

```bash
# 1. Sketch type-check, 5 configurations (g++ + stubs; NOT a target build)
tools/check_sketches.sh

# 2. Firmware core tests (host build, real threads) - 76 assertions
g++ -std=c++17 -O2 -pthread -DMESQ_HOST_TEST \
    -I"Device code/Pod_Watch_Binary" -I"Device code/Dongle_Binary" \
    tools/firmware_tests.cpp -o /tmp/mesq_fw_tests
/tmp/mesq_fw_tests

# 3. Parser acceptance - 63 assertions, against the parser the browser loads
node tools/parser_tests.js

# 4. BVH export - 18 assertions
node tools/export_tests.js

# 5. Phase 2 baseline harness + Phase 3 comparison - 27 assertions
node tools/replay_harness.js
```

## Instrumentation builds

```bash
# Pod, instrumented
arduino-cli compile --fqbn esp32:esp32:twatch \
  --build-property "build.extra_flags=-DMESQ_POD_ID=3 -DMESQ_INSTR=1" \
  "Device code/Pod_Watch_Binary"

# Pod, SENS-02 A/B arm
arduino-cli compile --fqbn esp32:esp32:twatch \
  --build-property "build.extra_flags=-DMESQ_POD_ID=3 -DMESQ_INSTR=1 -DMESQ_DISABLE_UNUSED_DMP_STREAMS=1" \
  "Device code/Pod_Watch_Binary"

# Hub, instrumented
arduino-cli compile --fqbn esp32:esp32:lilygo_t_display_s3 \
  --build-property "build.extra_flags=-DMESQ_INSTR=1" \
  "Device code/Dongle_Binary"

# All 17 pod images with a SHA-256 manifest and a duplicate-image check
tools/build_pods.sh
```
Note: the hub's per-node integrity counters and framed status are **always on** and
need no flag. `MESQ_INSTR` adds RSSI, write-duration and drop-reason detail.

## Dependencies

```bash
node --version      # 24.18.0 used here
g++ --version       # 13 used here; needs -std=c++17
python3 --version   # for tools/arduino_stubs/preprocess.py
```
No npm install is required for the tests: `tools/parser_tests.js`,
`tools/export_tests.js` and `tools/replay_harness.js` use only Node built-ins.

For firmware:
```bash
arduino-cli core install esp32:esp32
arduino-cli lib install "SparkFun 9DoF IMU Breakout - ICM 20948 - Arduino Library"
# TTGO_TWatch_Library: install manually from
# https://github.com/Xinyuan-LilyGO/TTGO_TWatch_Library
```

## Hardware capture

See `A1_07_OPEN_ITEMS_AND_HARDWARE_RUNBOOK.md`. Minimum loop:

```bash
# 1. flash one pod, watch the boot banner (resolves U2)
arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200

# 2. flash the hub, confirm its banner
arduino-cli monitor -p /dev/ttyACM1 -c baudrate=921600

# 3. serve the browser app and connect
node server.js        # or: python3 -m http.server 8000

# 4. in DevTools
_mocapDebug()          # bytes, frames, per-bone, freshness, parser drops
window._hubStatus      # the hub's framed status lines
window._podFresh       # fresh vs held per bone
window._parserDrops    # recovery events by reason
```

## Artifacts

| What | Where |
|---|---|
| Pod images + SHA-256 manifest | `tools/build_pods.sh` output dir |
| Host test binary | `/tmp/mesq_fw_tests` |
| Reports | `system_assessment/01_node_firmware/`, `system_assessment/02_hub/` |
| Exported captures | browser download, `MMcap_bvh_<timestamp>.bvh` |

## Rollback

```bash
git checkout 4487d56 -- "Device code" js index.html
```
The three Phase 3 commits are ordered by dependency: reverting `3718fa6` alone
undoes the hub work and leaves the pod and browser changes in place.
