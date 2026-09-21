# A1_07 — Open Items and Hardware Runbook

Only genuinely unresolved or hardware-dependent items. No "test more" entries:
every row has an exact command, a pass/fail rule, and who can run it.

---

## Blockers, in order

### B1 — Neither sketch has been built for the target
**Why open:** no `arduino-cli` or PlatformIO on this machine. Everything
firmware-side is type-checked against stubs, which does not prove the real
libraries agree, that it links, or that it fits.

**This blocks every other hardware item.** Do it first.

**Exact next action:**
```bash
arduino-cli core install esp32:esp32
arduino-cli lib install "SparkFun 9DoF IMU Breakout - ICM 20948 - Arduino Library"
# TTGO_TWatch_Library must be installed manually from
# https://github.com/Xinyuan-LilyGO/TTGO_TWatch_Library

# Pod, one image per id (tools/build_pods.sh does all 17 with a SHA-256 manifest)
tools/build_pods.sh 3

# Hub
arduino-cli compile --fqbn esp32:esp32:lilygo_t_display_s3 \
  "Device code/Dongle_Binary" --verbose

# Then repeat both with -DMESQ_INSTR=1 in build.extra_flags
```
**Pass:** all four compile. Record binary size, free flash, and the full verbose
log. **Fail:** capture the first error verbatim; likely causes are a library
version whose signatures differ from `tools/arduino_stubs/mesq_stubs.h`.
**Who:** anyone with the toolchain. **Return:** the verbose logs plus sizes.

### B2 — No pod or hub attached
Blocks M-NODE01 … M-RF. **Next action:** attach one pod and one hub by USB;
confirm `ls /dev/ttyACM* /dev/ttyUSB*` lists them.

### B3 — `U1`, the ESP-NOW PHY rate, is unknown
Nothing in either sketch configures it. Phase 1's airtime arithmetic assumes
1 Mbps and **dominates every capacity claim**. Needs a sniffer or a fleet reflash
with an explicit rate. Blocks the whole of `A1_04` §5.

### B4 — Requirements the owner must decide
`A1_00` §2. Specifically: is 60 fresh orientations/s hard? What is the latency
budget? What loss and longest-outage are acceptable? Without these, several design
decisions stay `REQUIREMENT_PENDING` — the code is written either way.

---

## Single-node measurements

### M-NODE01 — critical-section cost and handoff health
**Needs:** B1, one pod. **Build:** `-DMESQ_INSTR=1`.
**Run:** boot, read the 1 Hz `[INSTR]` line for 60 s static, then 60 s moving.
**Record:** `normBad`, `drop`, `age_ms(mean/max)`, `quat6/s`, `instr_us/s`.
**Pass:** `normBad == 0` in every second. `drop` ≈ `quat6/s − 32` (the intended
decimation, not loss). `age_ms` mean below one send period (31 ms).
**Fail:** any `normBad > 0` means the coherent channel regressed — stop and
report, this is the `NODE-01` invariant.

### M-SENS01 — the actual DMP rate (resolves `SENS-01`)
**Needs:** B1, one pod. **Run:** `-DMESQ_INSTR=1`, 60 s static and 60 s moving.
**Record:** `quat6/s` — this is **distinct FIFO outputs**, not loop iterations.
Also `read_us(min/mean/max)` and `fifoMore`.
**Pass:** none declared. This is the measurement that turns Phase 1's ~55 Hz from
a code comment into a number, and it decides whether the 60 Hz question is even
live. **Do this before any rate-architecture decision.**

### M-SENS02 — the unused raw streams A/B
**Needs:** M-SENS01 as baseline.
**Run:** same procedure with `-DMESQ_DISABLE_UNUSED_DMP_STREAMS=1`.
**Compare:** `read_us` mean and max, `quat6/s`, `fifoMore`.
**Pass:** `quat6/s` unchanged or higher **and** `read_us` mean materially lower →
adopt as default. Any drop in `quat6/s` → do not adopt.

### M-NODE03 — send cadence on real silicon
**Needs:** B1, one pod, `-DMESQ_INSTR=1`.
**Record:** the `send(n= … b=…)` histogram and `late_us`, `resync` for 60 s.
**Pass:** `n` = 32 ± 1 per second; the 30–39 ms bucket dominant; `resync == 0`
during steady operation. **Fail:** non-zero `resync` in steady state means
something blocks `TaskWifi` for >125 ms — find it before going to a fleet.

### M-NODE04 — initialisation fault injection
**Needs:** B1, one pod, and the ability to interrupt SDA (a jumper or tape over
the pad). **Do not desolder anything.**
**Run, three cases:**
1. IMU reachable → expect `INIT_RESULT : IMU_OK DMP_OK`, stage `RUNNING`.
2. SDA interrupted at boot → expect ~10 `IMU_ATTEMPT` lines over ~11 s, then
   `INIT_RESULT : IMU_FAIL`, a **red screen reading "IMU NOT FOUND"**, and the
   pod still responsive.
3. Then **10 cold boots** with the IMU reachable; record time to first valid
   sample each time.
**Pass:** no case hangs; case 2 never reboots in a loop; case 3 reaches `RUNNING`
10/10. **Fail:** any hang means `NODE-04` is not fixed.

### M-PWR01 — battery and display cost
**Needs:** B1, one pod, `-DMESQ_INSTR=1`.
**Run:** 5 min with the screen on, 5 min after it auto-sleeps.
**Record:** `read_us` max around the 3 s battery tick; `quat6/s`; `fifoMore`.
**Pass criterion to decide the fix:** if `read_us` max spikes >5 ms on the tick,
move `handleBattDisplay()` off the sample task. If not, leave it and close
`PWR-01` as `ACCEPTED_RISK` with the number.

---

## Hub measurements

### M-HUB02 — USB queue under a real fleet
**Needs:** B1, hub + as many pods as available.
**Run:** capture 10 min; watch the 1 Hz `Q depth=… high=… dropped=…` status line
in `window._hubStatus`.
**Pass:** `dropped == 0` and `high` well below 128. **Fail:** any `dropped > 0`
means real USB CDC throughput is below the assumption in `A1_01` §8 — raise
`MESQ_USB_QUEUE_LEN` or reduce offered load, and record the number.

### M-HUB03 — the reset path, on hardware
**Needs:** B1, hub + 2 pods.
**Run:**
```bash
# 1. noise must NOT reset the fleet
printf 'hello world\r\n' > /dev/ttyACM0
head -c 4096 /dev/urandom  > /dev/ttyACM0
# 2. the real command MUST reset it
printf '\xAA\x55\xFC\x01\x00\x01' > /dev/ttyACM0
```
**Pass:** steps 1 leave both pods streaming uninterrupted; step 2 reboots both and
the hub emits `CMD reset_fleet accepted (#1)`.
**Fail:** any reset in step 1 means the old path survives somewhere.

### M-HUB05 — per-node integrity against known loss
**Needs:** B1, hub + ≥4 pods.
**Run:** stream 5 min, then power one pod off for 10 s and back on.
**Pass:** that pod shows `POD xx LOST`, then on return a **resync**, and `lost`
does **not** jump by tens of thousands. This is the wrap/reboot case from
`test_hubstats` confirmed on real traffic.

---

## Fleet and RF

### M-FLEET — scaling
1, 4, 8, 12, 15, 17 pods, identical layout and motion, ≥60 s each.
**Record per node:** fresh rate (distinct `ms_lo`), `lost`, gaps, duplicates,
reordering, longest outage, RSSI, queue high-water, browser drops by reason.
**Report distributions and per-node values, never only fleet means.**
**Thresholds:** declare them from the application requirement (B4) **before**
running. Do not invent them after seeing results.

### M-SOAK — 30-minute full-fleet capture
Realistic body placement, documented interference. **Pass:** no duplicate ids, no
invalid emitted quaternions, no silent init failures, no parser deadlock, no
unexplained reset; recovery from any outage within the declared deadline without
a fleet power cycle.

### M-RF — the channel comparison
The seven-step protocol in `A1_04` §5. **Step 5 (three hubs, same channel) is the
control and is the one usually skipped** — without it you cannot tell RF
partitioning from USB/parser parallelism.

---

## Non-hardware open items

| ID | Item | Next action |
|---|---|---|
| **OPEN-BVH-HELD** | Held frames are detected in the browser but not propagated into the BVH, so measured and held frames are still conflated in the export | Carry `held` through `updateMotionData()` into a per-frame flag; emit a `; MESQ_HELD_FRAMES n` provenance line |
| **OPEN-PREFLIGHT** | No operator view of expected id/MAC, firmware, battery, last seen, fresh rate, gaps | Build a panel over `window._podRx`, `_podFresh`, `_hubStatus` — all the data now exists |
| **OPEN-RF-COUNTRY** | `esp_wifi_set_country()` is never called | Set US explicitly in both sketches and log it in the banner |
| **OPEN-INSTR-COST** | Rule 2 unsatisfied: the instrumentation's own cost has never been read off a device | Part of M-NODE01: record `instr_us/s` |
| **OPEN-U2** | Arduino core / IDF version still unresolved | Read the boot banner once a pod boots (B1 → B2) |
| **OPEN-I2C** | GPIO21/22 are **code-selected**; the physical wiring, pull-ups and what else shares the bus are unverified | Continuity test or schematic. **Do not remap these pins before this is done** |
| **OPEN-REVIEW** | Everything is `SELF_REVIEWED` | A second reader on `mesq_pod_core.h` and `mesq_hub_core.h`, focused on the ESP32 assumptions |
| **OPEN-PHN05** | Root Euler order `XYZ` puts yaw in the middle slot; Phase 2 measured up to 23.9% of frames within 10° of gimbal lock | Change to `YXZ` and re-export; needs a regression against existing captures |

---

## How to return results

For each measurement: the **exact build flags and firmware hash**, the raw log
file, the pod ids involved, the physical layout, and the environment (room,
distance, known 2.4 GHz activity). A number without its build identity cannot be
compared to another number.
