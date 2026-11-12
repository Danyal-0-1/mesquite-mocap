# Flashing a pod — arduino-cli quick reference

Everything needed to put firmware on one T-Watch and know which node it became.

---

## TL;DR

```bash
# once per terminal
alias acli=~/Downloads/squashfs-root/resources/app/lib/backend/resources/arduino-cli

# find the watch
acli board list

# build + upload pod id 3 (LEFT UP ARM)
acli compile --fqbn esp32:esp32:twatch \
  --build-property "build.extra_flags=-DMESQ_POD_ID=3" \
  --upload -p /dev/ttyACM0 .
```

Change `-DMESQ_POD_ID=<n>` per watch. **Nothing else changes between pods.**

---

## 1. arduino-cli — you already have it

The Arduino IDE AppImage bundles it. No separate install needed:

```
~/Downloads/squashfs-root/resources/app/lib/backend/resources/arduino-cli
```

Verified on this machine: **v1.6.0**, esp32 core **3.3.12**. Make it permanent so you
stop typing the path:

```bash
echo "alias acli=~/Downloads/squashfs-root/resources/app/lib/backend/resources/arduino-cli" >> ~/.bashrc
source ~/.bashrc
```

> If you ever move or delete the extracted AppImage, the alias breaks. A standalone
> install instead: `curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh`

---

## 2. One-time setup

```bash
acli core update-index
acli core install esp32:esp32           # 3.3.12 confirmed working
acli lib install "SparkFun 9DoF IMU Breakout - ICM 20948 - Arduino Library"
```

### ⚠ Do this first — the T-Watch library is NOT in the Library Manager

`Pod_Watch_Binary.ino` includes `<LilyGoWatch.h>` on **line 2**, and that library is
not installed and **cannot** be installed with `acli lib install`.

Until you clone it, *every* build stops on the same error regardless of what else you
do — including builds with a perfectly good `MESQ_POD_ID`:

```
Pod_Watch_Binary.ino:2:10: fatal error: LilyGoWatch.h: No such file or directory
```

This is the current state of this machine. **Nothing else in this guide will work
until this step is done.**

```bash
git clone --depth 1 https://github.com/Xinyuan-LilyGO/TTGO_TWatch_Library \
  ~/Arduino/libraries/TTGO_TWatch_Library
```

Check it took:

```bash
ls ~/Arduino/libraries/ | grep -i watch
```

---

## 3. Pod ID table

`MESQ_POD_ID` is the **only** thing that distinguishes one pod image from another.

| ID | Watch screen | Browser bone | Body segment |
|---:|---|---|---|
| 0 | HEAD | `Head` | head |
| 1 | SPINE | `Spine` | upper back |
| 2 | HIPS | `HipsAlt` | pelvis — *the hip pod* |
| 3 | LEFT / UP ARM | `LeftArm` | left upper arm |
| 4 | LEFT / FOREARM | `LeftForeArm` | left forearm |
| 5 | LEFT / HAND | `LeftHand` | left hand |
| 6 | RIGHT / UP ARM | `RightArm` | right upper arm |
| 7 | RIGHT / FOREARM | `RightForeArm` | right forearm |
| 8 | RIGHT / HAND | `RightHand` | right hand |
| 9 | LEFT / UP LEG | `LeftUpLeg` | left thigh |
| 10 | LEFT / LOW LEG | `LeftLeg` | left shin |
| 11 | LEFT / FOOT | `LeftFoot` | left foot |
| 12 | RIGHT / UP LEG | `RightUpLeg` | right thigh |
| 13 | RIGHT / LOW LEG | `RightLeg` | right shin |
| 14 | RIGHT / FOOT | `RightFoot` | right foot |
| 15 | LEFT / SHOULDER | `LeftShoulder` | left shoulder † |
| 16 | RIGHT / SHOULDER | `RightShoulder` | right shoulder † |

† Ids 15 and 16 exist in the firmware but the browser does **not** currently drive
those bones. Flashing them is harmless; they just won't move the rig.

**Screen colour is a fast visual check** that the right image went on:

| Colour | IDs |
|---|---|
| white | 0, 1, 2 (head, spine, hips) |
| blue-grey | 3, 4, 5, 15 (left arm chain) |
| orange | 6, 7, 8, 16 (right arm chain) |
| purple | 9, 10, 11 (left leg) |
| teal | 12, 13, 14 (right leg) |

---

## 4. Build and upload

```bash
acli board list                      # find the port, e.g. /dev/ttyACM0
```

**Compile only** (fast check before touching hardware):

```bash
acli compile --fqbn esp32:esp32:twatch \
  --build-property "build.extra_flags=-DMESQ_POD_ID=3" .
```

**Compile and upload**:

```bash
acli compile --fqbn esp32:esp32:twatch \
  --build-property "build.extra_flags=-DMESQ_POD_ID=3" \
  --upload -p /dev/ttyACM0 .
```

Board defaults are correct as shipped — PSRAM enabled, default partition scheme.
No `--board-options` needed.

### Instrumented build

Adds the 1 Hz `[INSTR]` diagnostic line (Quat6 rate, FIFO timing, send histogram,
quaternion validity, sample age):

```bash
acli compile --fqbn esp32:esp32:twatch \
  --build-property "build.extra_flags=-DMESQ_POD_ID=3 -DMESQ_INSTR=1" \
  --upload -p /dev/ttyACM0 .
```

---

## 5. Confirm which node you just made

Three independent checks — use at least the first two.

**1. The watch screen** — bone name in its chain colour. Readable across the room.

**2. The boot banner** —

```bash
acli monitor -p /dev/ttyACM0 -c baudrate=115200
```

```
===== MESQUITE POD BOOT =====
FW_BUILD      : <date> <time>
POD_ID        : 3            ← this is the one that matters
IDF_VERSION   : ...
TICK_RATE_HZ  : 1000
INIT_RESULT   : IMU_OK DMP_OK
```

`INIT_RESULT : IMU_FAIL` or a **red SENSOR FAULT screen** means the IMU or DMP did
not come up — that pod is not usable. It no longer hangs silently.

**3. In the browser** — `window._podRx` shows per-bone packet counts. A missing key
means that id never arrived.

---

## 6. All 17 at once

```bash
cd <repo root>
tools/build_pods.sh            # all 17 images + SHA-256 manifest + duplicate check
tools/build_pods.sh 3          # just id 3
```

The manifest matters: two pods with the same id is **silent at runtime** and looks
exactly like "a node didn't connect."

---

## 7. Why the id is a build flag

The build **fails** if `MESQ_POD_ID` is undefined or outside `0..16`:

```
#error "MESQ_POD_ID is not defined. Build with -DMESQ_POD_ID=<0..16>"
```

That is deliberate. Identity used to be chosen by uncommenting one of 17 lines;
flashing a 17-watch fleet that way eventually produces a duplicate, which costs a
session. A build error costs seconds.

---

## 8. Troubleshooting

| Error | Cause | Fix |
|---|---|---|
| `LilyGoWatch.h: No such file` | T-Watch library missing | §2 — clone it. **This fails at line 2, so it masks every other error until fixed** |
| `MESQ_POD_ID is not defined` | no `--build-property` | add `-DMESQ_POD_ID=<n>`. You will only reach this error once the T-Watch library is present |
| `mbedtls_md5_starts_ret was not declared` | mbedtls 3.x removed `_ret` functions | wrong `ESPAsyncWebServer` fork — see below |
| `undefined reference to pxCurrentTCB` | SMP FreeRTOS removed the single-core symbol | same root cause |
| Port not listed | watch asleep, or a charge-only cable | wake it, try another cable, check `ls /dev/ttyACM*` |
| Upload times out | boot mode | hold BOOT while upload starts |

### The async-library forks

The **hub** sketch needs `ESPAsyncWebServer` and `AsyncTCP` **by mathieucarbou** —
those are the ones maintained for ESP32 core 3.x. Other forks (lacamera, dvarrel)
predate it and fail with the mbedtls and `pxCurrentTCB` errors above.

IDE → Library Manager → search each → install the **mathieucarbou** entry.

This does not affect the pod, which uses neither library.
