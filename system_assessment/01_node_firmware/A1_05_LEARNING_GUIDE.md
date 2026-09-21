# A1_05 — Learning Guide

Generated from the frozen release candidate `3718fa6` (hashes in `A1_03` §1).

**Companion: [`A1_05_RETYPE_THESE.md`](A1_05_RETYPE_THESE.md)** — the code you
should type out by hand, in order, with exercises and an answer key. This file is
the explanation; that one is the practice.

---

## Contents

1. The system story, motion to browser
2. Code atlas
3. Quaternions
4. Time, freshness, and what a timestamp actually means
5. Two cores, one pose
6. The wire
7. Parser state machines
8. Glossary

---

## 1. The system story

```
your arm moves
  → the ICM-20948's gyroscope senses rotation RATE; its accelerometer senses
    the direction of gravity (plus your arm's own acceleration, which is the
    problem the fusion has to solve)
  → the DMP, a small processor inside the sensor, fuses them into an
    ORIENTATION and pushes it into a FIFO as three Q30 fixed-point numbers
  → TaskReadIMU (ESP32 core 1) reads the FIFO over I²C, reconstructs the
    fourth quaternion component, checks it is valid, and PUBLISHES it as one
    coherent record
  → TaskWifi (ESP32 core 0) wakes on a 31250 µs deadline, TAKES the newest
    record, packs 16 bytes, and unicasts them over ESP-NOW
  → the hub's radio callback validates, counts, and COPIES the 16 bytes into a
    queue, then returns immediately
  → usbWriterTask — the only writer — drains the queue to USB CDC
  → the browser's parser turns the byte stream back into records
  → the skeleton code applies calibration and drives the 3D model
  → the recorder samples the skeleton and writes a BVH file
```

Three ideas run through the whole chain and are worth holding onto:

- **A pose is one thing, not four numbers.** Every bug in §5 comes from forgetting
  this.
- **A timestamp means nothing until you say which clock.** Every bug in §4 comes
  from forgetting this.
- **A byte stream has no record boundaries.** You have to impose them, and you
  have to be able to recover when they are violated. That is §7.

## 2. Code atlas

| Entry | File | Symbol | Core / task | Runs | Blocking | Failure path |
|---|---|---|---|---|---|---|
| Boot | `Pod_Watch_Binary.ino` | `setup()` | core 1 (Arduino) | once | yes | bounded; paints a fault |
| IMU init | `Pod_Watch_Binary.ino:432` | `setupIMU()` | core 1 | once + retry | yes, bounded | returns `false`, stage recorded |
| Sample loop | `Pod_Watch_Binary.ino:892` | `TaskReadIMU` | **core 1** | ~55 Hz | 10 ms when FIFO empty | rejects sample, holds pose |
| Quaternion policy | `mesq_pod_core.h:190` | `mesqReconstructQuat` | core 1 | per sample | no | `MESQ_Q_REJECTED` |
| Publish | `mesq_pod_core.h:95` | `MesqSampleChannel::publish` | core 1 | per valid sample | critical section, ~24 B | counts a drop |
| Take | `mesq_pod_core.h:120` | `MesqSampleChannel::take` | **core 0** | ~32 Hz | critical section | returns not-fresh |
| Deadline | `mesq_pod_core.h:265` | `mesqDeadlineDue` | core 0 | every tick | no | bounded catch-up |
| Send | `Pod_Watch_Binary.ino:777` | `TaskWifi` | **core 0** | 32 Hz | `esp_now_send` | send callback counts failures |
| Serialise | `mesq_packet.h:80` | `mesq_pack` | core 0 | per send | no | saturates, never wraps |
| Hub receive | `Dongle_Binary.ino:~400` | `OnDataRecv` | WiFi task | per packet | **must not block** | validates then drops |
| Hub enqueue | `mesq_hub_core.h:185` | `mesqUsbPush` | any producer | per record | critical section | counts a drop |
| Hub write | `Dongle_Binary.ino:547` | `usbWriterTask` | core 1, prio 2 | continuous | `Serial.write` | queue backs up, visibly |
| Hub command | `Dongle_Binary.ino:568` | `hostCmdTask` | core 1 | 100 Hz poll | no | fails closed |
| Browser parse | `js/mesq_parser.js:215` | `feed` | main thread | per read | no | three bounded abandon paths |
| Export | `js/bvh_converter.js` | `generateBVH` | main thread | on stop | no | labelled fallback |

**Read this ordering once and it explains most of the design:** the only place
core 1 and core 0 touch is `MesqSampleChannel`. The only place two hub tasks touch
is `MesqUsbQueue`. Everything else is single-owner. That is not an accident — it
is the whole strategy.

## 3. Quaternions

### Orientation is not position
A quaternion says **which way something is facing**. It says nothing about where
it is. Mesquite's pods measure orientation only; the root *position* comes from
the phone's SLAM. This is why Phase 1 could report good root-relative pose (15°)
and poor global pose (42°) at the same time — the limbs are right relative to each
other, the whole body is pointing the wrong way.

### The form
A unit quaternion is four numbers `q = (w, x, y, z)` with

    w² + x² + y² + z² = 1

`(x, y, z)` is a direction (the axis) scaled by `sin(θ/2)`; `w` is `cos(θ/2)`,
where `θ` is the rotation angle about that axis.

**Project convention:** this codebase writes `(w, x, y, z)`. On the wire the order
is `qx, qy, qz, qw` (see `mesq_packet.h`). Mixing these up is the single most
common way to produce a plausible-looking wrong rotation, which is why the packing
is explicit and tested rather than a `memcpy`.

### Why Euler angles are not used here
Three angles (roll/pitch/yaw) are easy to read and have **gimbal lock**: at a
particular pitch, two of the three axes align and one degree of freedom
disappears. Phase 2 measured this in Mesquite's own export: with `XYZ` order, up
to **23.9%** of frames sat within 10° of the singularity, because `XYZ` puts yaw in
the middle slot. Rokoko's `YXZ` scored 0.0%. That is `PHN-05`, still open.

### `q` and `-q` are the same rotation
Negating all four components gives the same orientation (θ → θ + 2π about the
negated axis). A comparison that ignores this reports 180° of error for two
identical poses. Always compare `min(|q₁ − q₂|, |q₁ + q₂|)`, or use the dot
product's absolute value.

### Q30 fixed point
The DMP has no floating-point unit, so it sends integers scaled by 2³⁰:

    q = Q / 1073741824.0

A `int32_t` of 1073741824 means exactly 1.0. The resolution is 2⁻³⁰ ≈ 9.3 × 10⁻¹⁰
per component.

### Reconstructing `w` — and the bug that lived here
The DMP sends only `x, y, z`. `w` comes from the unit-norm constraint:

    w = √(1 − (x² + y² + z²))

The radicand can go **negative**. Two completely different reasons:

**Roundoff.** Each component carries ≤ 2⁻³⁰ of quantisation error, so the sum of
three squares can overshoot 1 by at most about

    3 × 2 × 1 × 2⁻³⁰ ≈ 5.6 × 10⁻⁹

Clamping to zero costs nothing — the true `w` was already within √(5.6e-9) ≈
7.5 × 10⁻⁵ of zero.

**A bad sample.** Garbage from the FIFO gives components well outside the unit
sphere and a radicand of order −1. There is no pose to recover.

Phase 1 did neither. It called `sqrt()` on the raw value. A negative radicand gave
`NaN`; `q_to_i16(NaN)` returned `0`; all four components became `0`; and the pod
transmitted `(0,0,0,0)` — **not a rotation**. It has no inverse, and every
composition downstream inherits the corruption silently.

`MESQ_RADICAND_EPS = 1e-6` sits ~180× above the roundoff bound and ~10⁶× below a
real failure. The two cases cannot be confused.

### int16 quantisation on the wire
Each component is stored as `int16 = float × 32767`, a step of 1/32767 ≈ 3 × 10⁻⁵.
Phase 1 `EST-03` measured the resulting angular error at **0.006°** — negligible
next to a 42° heading error. **Do not revisit this.** It is a good example of a
plausible suspect that measurement exonerated.

### Why six-axis fusion cannot know which way you are facing
Gyroscope + accelerometer = "6-axis". The accelerometer sees **gravity**, which
fixes two of three rotational degrees of freedom: roll and pitch have an absolute
reference. Rotation *about* the gravity vector — yaw, heading — has no reference
at all in that pair. Only a magnetometer (Earth's field) or an external reference
supplies it.

So the heading at boot is **arbitrary**, and it stays wherever it started plus
whatever the gyro integrates. Phase 1's 71–99° session-to-session error is an
arbitrary **origin**, not drift, and Phase 2 confirmed the sign flips between
sessions — exactly the signature of an unreferenced frame. Calling it "drift" in a
paper would be wrong and a reviewer would catch it.

## 4. Time, freshness, and what a timestamp means

### The clock domains
There are **17 independent `millis()` clocks** plus the host's. Each starts when
that pod boots. They share **no epoch**. Subtracting one pod's `millis()` from
another's is meaningless (`SYNC-01`).

### What `ms_lo` is now
Phase 1 wrote `millis()` at transmit. Phase 3 writes the pod clock at **FIFO
decode**. The layout did not change; the meaning did.

Two packets from one pod carrying the **same** `ms_lo` carry the **same sensor
sample**. The pod had nothing new to send, so the pose is **held**, not measured.
That distinction is free and it matters for the paper: a held frame is not
evidence.

### What it is NOT
- **Not** the DMP's internal sample instant. No interrupt line is wired, so the
  true instant is unverified. It is a `sample_observed_time`.
- **Not** a timebase. It wraps every 65.536 s and shares no epoch with anything.
- **Not** usable for cross-device latency. Ever, without a validated clock
  alignment with a stated uncertainty.

### The seven rates
Keep them separate; conflating them is how "60 Hz" became ambiguous:

| Rate | Where | Approx |
|---|---|---|
| DMP output | inside the sensor | ~55 Hz (stock config) |
| FIFO drain | `TaskReadIMU` | as fast as I²C allows |
| Fresh publication | `MesqSampleChannel` | = DMP output |
| Send attempts | `TaskWifi` deadline | 32 Hz exactly |
| Hub receive | hub callback | ≤ send, radio loss |
| Browser parse | `feed()` | ≤ hub receive |
| Render / export | `requestAnimationFrame`, recorder | independent |

~23 samples/s are **deliberately discarded** between publication and send. That is
the design, and `MesqSampleChannel::dropped()` now counts them so the discard is
measured instead of assumed.

### Wrap-safe arithmetic
`count` is `uint16` and wraps at 65535. The distance between two counters is

    forward = (uint16_t)(b - a)

Unsigned arithmetic wraps correctly by definition, so 65535 → 0 gives 1, not
−65535. Getting this wrong is how a single pod reboot becomes "65,000 packets
lost" and poisons a whole session's statistics. `mesqStatsOnPacket()` handles the
wrap, distinguishes duplicate / reorder / gap, and classifies a large forward jump
as a **reboot resync** rather than loss.

## 5. Two cores, one pose

The ESP32 here is dual-core. `TaskReadIMU` is pinned to core 1; `TaskWifi` to
core 0. They run **genuinely simultaneously**, on different hardware.

### The bug
Phase 1 shared this:

```c
struct Quat { float x, y, z, w; } quat;
```

Core 1 wrote `quat.w`, `quat.x`, `quat.y`, `quat.z` in sequence. Core 0 read them
in sequence. Nothing stopped a read landing between two writes, returning half of
one orientation and half of another. That is a **torn read**. Its norm is not 1,
so it is not a rotation at all.

### Why `volatile` would not have fixed it
`volatile` tells the compiler not to cache a variable in a register. It says
nothing about *atomicity* and nothing about *ordering between cores*. Four
separate 32-bit stores remain four separate stores. This is worth internalising —
it is the most common misconception in embedded concurrency.

### The fix
Stop treating the pose as four numbers. Make it one record:

```c
struct MesqSample {
    float    w, x, y, z;
    uint32_t sample_ms;
    uint32_t sample_seq;
    uint8_t  valid;
};
```

and copy the whole record under a **critical section** — an SMP spinlock that also
masks interrupts on the calling core. 24 bytes, sub-microsecond, cannot tear.

**The rule that goes with it: nothing is called inside a critical section.** No
I²C, no radio, no `Serial`. Holding one across I/O is how an embedded system
deadlocks or misses deadlines. The section here contains only `memcpy`-equivalent
assignments.

### Why a length-one slot, not a queue
The radio always wants the **newest** pose. A depth-N queue would hand the
transmitter a backlog of stale orientations after any hiccup. Overwriting is the
correct policy — and `dropped` counts each overwrite, so the ledger

    published = consumed + dropped

can be checked. That assertion caught an inverted condition in the first draft of
this very channel, which is a good argument for writing the ledger test before
trusting the code.

### Why not a seqlock
A seqlock is lock-free and would be slightly cheaper. It depends on acquire/release
memory ordering that I could not verify against this exact Xtensa toolchain
without hardware — and a subtly wrong barrier reintroduces the tearing while
looking correct. The cheap, obviously-correct option won.

## 6. The wire

```
 off  size  field    meaning
 0    1     sync0    0xAA
 1    1     sync1    0x55
 2    1     id       bone 0..16
 3    1     batt     0..100 %
 4    2     qx       int16 LE = float * 32767, TRUNCATED toward zero
 6    2     qy
 8    2     qz
 10   2     qw
 12   2     count    uint16 PACKET sequence, wraps
 14   2     ms_lo    uint16 pod millis() at SAMPLE DECODE
```

Reserved ids, all outside 0..16 so no record kind can be mistaken for another:

| Marker | Direction | Meaning |
|---|---|---|
| `0xFC` | host → hub | framed command |
| `0xFE` | hub → host | framed status |
| `0xFF` | hub → pod | control (reboot) |

**Endianness.** Little-endian, because that is what the ESP32 is and what
`DataView.getInt16(off, true)` reads. The `true` is the little-endian flag; omit it
and you silently get big-endian.

**Why `count` is the packet counter and not the sample counter.** The DMP produces
~55/s and the radio sends ~32/s. A sample counter would appear to gap ~23 times a
second and would destroy gap detection. Gaps in `count` mean **radio loss**, which
is what you want to measure.

**Why serialisation is explicit.** `mesq_pack()` writes byte 0, byte 1, byte 2…
rather than `memcpy`ing the struct. The field order is then a property of a
function you can read and test, not of whatever the compiler did with padding and
alignment. The golden vector

    aa550257ff3f01c0ff1fff5f3412cdab

is asserted **byte-identical** in both the C and JavaScript suites. Writing that
test immediately found a 1-LSB divergence: JS used `Math.round`, the firmware's C
cast truncates toward zero. Small, but exactly the kind of thing that makes
"why is my quaternion slightly off" take a day.

## 7. Parser state machines

A serial stream is bytes. There are no records until you make some.

### The three record kinds
```
[0xAA][0x55][id 0..16][13 more bytes]      16-byte pose
[0xAA][0x55][0xFE][len][payload...]        hub status
'{' ... '}' '\n'                            phone JSON line
```

### The failure that was there
```js
if (b === SYNC0 && _jsonLine.length === 0) { ...binary... }
if (b === 0x7B || _jsonLine.length > 0)     { ...accumulate until '\n'... }
```

Once a `{` opened a line, **every** subsequent byte went into a string until a
`0x0A` arrived — including whole binary frames. If the payload bytes happened to
avoid `0x0A`, it never arrived, and the parser was dead until the page reloaded.

Phase 2 measured that it usually self-clears in ~16 packets because `0x0A` is
common in binary payloads — but also proved it latches **permanently** when it
does not. "Usually recovers" is not a property you can build on.

### The fix, and why it is deterministic rather than heuristic
Valid JSON here is **7-bit ASCII**. The bytes `0xAA` and `0x55` **cannot** occur
inside a well-formed JSON line. So finding `0xAA 0x55` while scanning a JSON line
is *proof* of a hub-side interleave — not a guess. The text line is abandoned and
the binary frame survives.

That single observation is what turns a heuristic into a correctness argument. It
is worth remembering as a technique: look for an invariant in your data that makes
the ambiguous case impossible.

Plus three hard bounds, because a parser that cannot make progress must shed bytes
rather than accumulate them: max JSON line 512 B, max buffer 4096 B, stall timeout
2000 ms. Each abandon path advances past the offending byte so the next iteration
rescans for a sync.

### One implementation
`js/mesq_parser.js` is loaded by the browser **and** by `tools/parser_tests.js`.
Phase 2 had a hand-copied duplicate in the fixture marked "kept in sync manually",
which meant a fix in the browser was covered by no test at all. If you take one
structural lesson from this phase, take that one.

## 8. Glossary

| Term | Meaning | Where in Mesquite |
|---|---|---|
| **bitmask** | integer whose bits are independent flags | `data.header & DMP_header_bitmap_Quat6` |
| **callback** | function the framework calls into your code | `OnDataRecv`, `OnDataSent` |
| **critical section** | region where preemption/interrupts are masked | `mesqEnter`/`mesqExit` |
| **DLPF** | digital low-pass filter inside the IMU | not configured here |
| **DMP** | Digital Motion Processor, fusion engine inside the ICM-20948 | `myICM.enableDMP()` |
| **ESP-NOW** | Espressif connectionless link-layer protocol | pod → hub |
| **FIFO** | first-in-first-out buffer holding DMP output | `readDMPdataFromFIFO` |
| **freshness** | whether a packet carries a new sample | `ms_lo` equality |
| **FreeRTOS** | the RTOS under Arduino-ESP32 | `xTaskCreatePinnedToCore` |
| **I²C** | two-wire bus (SDA/SCL) | GPIO21/22 @ 400 kHz |
| **latency** | duration between two events — **name both clocks** | see §4 |
| **MAC** | 6-byte radio address | `hubAddress` |
| **MCU** | microcontroller | ESP32 (pod), ESP32-S3 (hub) |
| **ODR** | output data rate; the register holds a **divider** | `setDMPODRrate(…, 0)` |
| **peer** | registered ESP-NOW counterpart | `esp_now_add_peer` |
| **PHY** | physical layer / modulation rate | **`U1`, unresolved** |
| **Q30** | fixed point, 2³⁰ = 1.0 | `mesqQ30ToDouble` |
| **quaternion** | 4-number rotation | §3 |
| **queue** | bounded FIFO between tasks | `MesqUsbQueue` |
| **sequence** | monotonic counter for detecting loss | `count`, `sample_seq` |
| **task** | independently scheduled thread | `TaskReadIMU`, `TaskWifi` |
| **tick** | FreeRTOS scheduler quantum | `configTICK_RATE_HZ` |
| **timestamp** | a time **in a named clock domain** | `sample_ms` |
| **wrap** | counter rolling past its maximum | `(uint16_t)(b - a)` |

---

## What you must be able to explain to a reviewer

1. Why six-axis fusion cannot supply absolute heading, and why the 71–99° figure
   is an origin rather than drift.
2. Why `volatile` does not fix a cross-core data race.
3. Why the radio sends 32 Hz while the DMP produces ~55 Hz, and why that is a
   choice rather than loss.
4. What `ms_lo` means, in which clock, and what you may not compute from it.
5. Why `count` is a packet counter and not a sample counter.
6. Why a negative radicand is sometimes roundoff and sometimes a bad sample, and
   where you drew the line.
7. Why `0xAA 0x55` inside a JSON line is proof rather than a guess.
8. Why the export's frame time must be measured, and what the 6.67% error did to
   the benchmark's correlation.
9. What was fixed in **source** versus what has been **verified on hardware** —
   and that for the firmware, the honest answer is "none of it yet".
