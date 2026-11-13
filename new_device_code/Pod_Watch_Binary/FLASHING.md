# Flashing a pod — quick reference

Build one T-Watch, know which node it became. **Verified working on this machine.**

---

## TL;DR

```bash
cd <repo root>
tools/setup_arduino.sh pod        # once, ~3 GB download
tools/build_pod.sh 3              # compile only
tools/build_pod.sh 3 /dev/ttyUSB0 # compile + upload
```

Only the id changes between pods. Table in §3.

---

## 1. Why there are two build environments

**The pod and the hub need different ESP32 core versions, and arduino-cli keeps
one core version per data directory.** They cannot share.

| Target | Core | Why |
|---|---|---|
| **pod** — T-Watch 2019 | **2.0.17** | `TTGO_TWatch_Library` vendors its own `TFT_eSPI` and `lv_lib_png`. They use direct GPIO register access and lodepng signatures that IDF 5.x changed. The pod **does not build** on core 3.x — it is a whole graphics stack, not a few call sites. |
| **hub** — T-Display-S3 | **3.x** | `Dongle_Binary.ino` targets 3.x/IDF 5.x, and its async libraries are the mathieucarbou forks, which are the ones maintained for 3.x. |

`tools/setup_arduino.sh` builds both under `~/.mesquite-arduino/`. **Neither touches
`~/Arduino` or `~/.arduino15`**, so your Arduino IDE is unaffected.

> arduino-cli does **not** expand `~` in a config file — a literal `~` makes it
> create a directory actually *named* `~` in the working directory. That is why
> `tools/arduino_env/mkconfig.sh` generates the config with absolute paths at run
> time instead of checking a YAML into git.

---

## 2. One-time setup

```bash
tools/setup_arduino.sh pod     # or: hub, or: both
```

Installs core 2.0.17, the SparkFun ICM-20948 library, `WebSockets`
(Links2004/Markus Sattler), and clones `TTGO_TWatch_Library` — which is **not** in
the Library Manager and cannot be installed with `lib install`.

arduino-cli itself needs no install: the Arduino IDE AppImage bundles it at
`~/Downloads/squashfs-root/resources/app/lib/backend/resources/arduino-cli`
(v1.6.0 here). The scripts find it automatically.

---

## 3. Pod ID table

`MESQ_POD_ID` is the **only** difference between pod images.

| ID | Watch screen | Browser bone | Segment |
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

† In the firmware, but the browser does not currently drive those bones.

**Screen colour** is a fast check that the right image landed:

| Colour | IDs |
|---|---|
| white | 0, 1, 2 |
| blue-grey | 3, 4, 5, 15 (left arm) |
| orange | 6, 7, 8, 16 (right arm) |
| purple | 9, 10, 11 (left leg) |
| teal | 12, 13, 14 (right leg) |

---

## 4. Build and upload

```bash
tools/build_pod.sh 3                   # compile only
tools/build_pod.sh 3 /dev/ttyUSB0      # compile + upload
```

Find the port with `acli board list`. **The T-Watch 2019 appears as `/dev/ttyUSB0`**,
not `ttyACM` — it uses a CP210x/CH340 USB-UART bridge.

### Instrumented build

Adds the 1 Hz `[INSTR]` line (Quat6 rate, FIFO timing, send histogram, quaternion
validity, sample age):

```bash
CFG=$(tools/arduino_env/mkconfig.sh pod)
acli --config-file "$CFG" compile --fqbn esp32:esp32:twatch \
  --build-property "compiler.cpp.extra_flags=-DMESQ_POD_ID=3 -DMESQ_INSTR=1" \
  new_device_code/Pod_Watch_Binary
```

Measured cost of the instrumentation: **+1,864 bytes of flash** (939,153 → 941,017).

### ⚠ `compiler.cpp.extra_flags`, never `build.extra_flags`

`build.extra_flags` **replaces** the board's own defines rather than appending, so it
silently wipes `ARDUINO_USB_CDC_ON_BOOT` and friends. Library platform detection then
fails in confusing ways — `WebSockets` falls through to its W5100 branch and dies on
a missing `Ethernet.h`. `compiler.cpp.extra_flags` is empty by default and appends
cleanly.

---

## 5. Confirm which node you made

**1. Watch screen** — bone name in its chain colour, readable across the room.

**2. Boot banner:**

```bash
acli monitor -p /dev/ttyUSB0 -c baudrate=115200
```

```
===== MESQUITE POD BOOT =====
FW_BUILD      : <date> <time>
POD_ID        : 3            ← the one that matters
TICK_RATE_HZ  : 1000
INIT_RESULT   : IMU_OK DMP_OK
```

`INIT_RESULT : IMU_FAIL` or a **red SENSOR FAULT screen** means the IMU or DMP did
not come up. It no longer hangs silently.

**3. Browser** — `window._podRx` shows per-bone counts. A missing key means that id
never arrived.

---

## 6. All 17

```bash
for i in $(seq 0 16); do tools/build_pod.sh $i || break; done
```

Or `tools/build_pods.sh` for images plus a SHA-256 manifest and a duplicate-image
check. Two pods sharing an id is **silent at runtime** and looks exactly like "a node
didn't connect."

---

## 7. Why the id is a build flag

The build **fails** if `MESQ_POD_ID` is missing or outside `0..16`:

```
#error "MESQ_POD_ID is not defined. Build with -DMESQ_POD_ID=<0..16>"
```

Deliberate. Identity used to be chosen by uncommenting one of 17 lines; flashing a
17-watch fleet that way eventually produces a duplicate, which costs a session. A
build error costs seconds.

---

## 8. Troubleshooting

| Error | Cause | Fix |
|---|---|---|
| `LilyGoWatch.h: No such file` | T-Watch library missing | `tools/setup_arduino.sh pod` |
| `MESQ_POD_ID is not defined` | no id supplied | `tools/build_pod.sh <n>` |
| `Ethernet.h: No such file` | used `build.extra_flags` | use `compiler.cpp.extra_flags` — §4 |
| `ledcSetup not declared` / `'GPIO' was not declared` | building the pod on **core 3.x** | use the pod environment (core 2.0.17) |
| `main file missing from sketch` | ran with `.` from the wrong directory | `cd` to the sketch, or pass its full path |
| `mbedtls_md5_starts_ret` / `pxCurrentTCB` | **hub** built against the wrong async forks | `tools/setup_arduino.sh hub` installs mathieucarbou's |
| Port not listed | watch asleep, or a charge-only cable | wake it; `ls /dev/ttyUSB*` |
| Upload times out | boot mode | hold BOOT as upload starts |

---

## 9. Verified

On this machine, core 2.0.17, arduino-cli 1.6.0:

| Build | Result |
|---|---|
| `tools/build_pod.sh 3` | **939,153 bytes, 14% flash** |
| `tools/build_pod.sh 14` | 939,153 bytes |
| id 3 + `MESQ_INSTR=1` | 941,017 bytes (+1,864) |
| no id | fails with the `#error`, as designed |

Compile only — **not yet flashed to a watch.**
