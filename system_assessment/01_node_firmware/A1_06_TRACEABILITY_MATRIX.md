# A1_06 — Traceability Matrix

One row per in-scope A1 finding and per interface dependency required to verify
it. Out-of-scope findings are in `A1_10_REFERENCED_BACKLOG.md`, not omitted.

Diagnosis: `UNTESTED` `CONFIRMED` `FALSIFIED` `INCONCLUSIVE` `NOT_APPLICABLE`
Resolution: `NOT_STARTED` `CODE_FIXED` `VERIFIED` `HARDWARE_VALIDATION_PENDING`
`REQUIREMENT_PENDING` `NOT_APPLICABLE` `ACCEPTED_RISK` `BLOCKED`

`VERIFIED` requires all six conditions of the master prompt §1. For firmware,
condition 3 (the target builds) **cannot be satisfied** without `arduino-cli`, so
no firmware row is `VERIFIED`. Browser and export rows can be and are.

---

## Node firmware

### `SENS-01` — fresh orientation rate vs the 60 Hz question
- **Source:** Phase 1 A1 · **Anchor:** `Pod_Watch_Binary.ino:~505` `setDMPODRrate(DMP_ODR_Reg_Quat6, 0)`
- **Code fact:** ODR divider 0 = maximum for this configuration `[fact-code]`
- **Runtime evidence:** none. Phase 1's ~55 Hz rests on the firmware's own comment `[unverified]`
- **Falsifier:** count distinct Quat6 FIFO outputs per second on a bench (I9 exists, never run)
- **Dependencies:** the owner's answer on whether 60 Hz is hard; `Example10_DMP_FastMultipleSensors` compatibility with the pinned library
- **Candidates:** stock (current) · SparkFun fast-init · raw-sensor host fusion · new hardware
- **Selected:** none. No register invented. Comment corrected to state the ceiling is configuration-specific, not a hardware limit
- **Test:** M-SENS01 (`A1_07`) · **Before/After:** n/a
- **Diagnosis `INCONCLUSIVE` · Resolution `REQUIREMENT_PENDING`**
- **Paper:** cannot claim any rate · **Risk:** if 60 Hz is hard, nothing here reaches it
- **Learn:** `A1_05` §4 (seven rates)

### `SENS-02` — unused raw accel/gyro FIFO streams
- **Anchor:** `Pod_Watch_Binary.ino:~495` · **Code fact:** enabled, `data.Raw_*` never read anywhere `[fact-code]`
- **Falsifier:** A/B FIFO read duration and distinct Quat6 rate with the streams off
- **Selected:** `-DMESQ_DISABLE_UNUSED_DMP_STREAMS`, **default off** (performance change, no measurement)
- **Test:** type-check in both configurations; M-SENS02 for the A/B
- **Diagnosis `CONFIRMED` (code) · Resolution `REQUIREMENT_PENDING`**

### `SENS-03` — invalid quaternion reconstruction
- **Anchor:** `mesq_pod_core.h:190-240`, called `Pod_Watch_Binary.ino:992`
- **Code fact:** unguarded `sqrt`; `q_to_i16(NaN) == 0` → zero quaternion on the wire `[fact-code]`
- **Falsifier:** an accepted result whose norm is not 1
- **Selected:** threshold policy, `EPS = 1e-6`
- **Test:** `firmware_tests.cpp test_sens03` · **Before:** NaN → `(0,0,0,0)` · **After:** 200k random + 50k shell inputs, every accepted result unit-norm `[fact-measured, host]`
- **Diagnosis `CONFIRMED` · Resolution `CODE_FIXED`** (host-verified; no target build)
- **Learn:** `A1_05` §3 · **Retype:** item 1

### `NODE-01` — cross-core torn quaternion
- **Anchor:** `mesq_pod_core.h:60-160`; `Pod_Watch_Binary.ino:249, 777, 892`
- **Code fact:** four floats written on core 1, read on core 0, unsynchronised, not even `volatile` `[fact-code]`
- **Falsifier:** a read whose fields span two generations
- **Selected:** critical-section length-one channel; seqlock and depth-N queue rejected (`A1_01` §1)
- **Test:** `test_node01`, 2 threads × 400k · **Before:** 469/8,931 non-unit `[fact-measured, host]` · **After:** 0/141,749, ledger balanced
- **Diagnosis `CONFIRMED` · Resolution `CODE_FIXED`**
- **Learn:** `A1_05` §5 · **Retype:** item 4

### `NODE-02` / `SYNC-02` — timestamp semantics
- **Anchor:** `mesq_packet.h` header comment; `Pod_Watch_Binary.ino:~820`
- **Code fact:** Phase 1 `ms_lo = millis()` at send `[fact-code]`
- **Selected:** redefine as sample-decode time; **layout unchanged**; `count` deliberately left as the packet counter
- **Test:** `test_node02` · **After:** repeat takes report not-fresh, 54/55 superseded samples counted
- **Diagnosis `CONFIRMED` · Resolution `CODE_FIXED`**
- **Paper:** freshness marker only — **no cross-device latency may be derived**
- **Learn:** `A1_05` §4

### `NODE-03` — transmit cadence
- **Anchor:** `mesq_pod_core.h:250-290`; `Pod_Watch_Binary.ino:777-790`
- **Code fact:** `1000/32 == 31`; strict `>`; `prev_ms` re-anchored to send time `[fact-code]`
- **Test:** `test_node03` · **Before:** ≤31.25/s, drifting · **After:** exactly 32/s; 320–321 over 10 s under jitter with 0 resyncs; 65 across the µs wrap
- **Diagnosis `CONFIRMED` · Resolution `CODE_FIXED`**
- **Risk:** +2.4% offered load, unmeasured against a real airtime budget
- **Retype:** item 3

### `NODE-04` — latching init failure
- **Anchor:** `Pod_Watch_Binary.ino:432-540`
- **Code fact:** `while(!initialized)` and `while(1);` `[fact-code]`
- **Falsifier:** pull SDA and confirm the pod reaches `FAIL_IMU`, paints red, and retries
- **Test:** type-check only. **Fault injection needs hardware** → M-NODE04
- **Diagnosis `CONFIRMED` · Resolution `CODE_FIXED` + `HARDWARE_VALIDATION_PENDING`**

### `NODE-05` — per-node identity
- **Anchor:** `Pod_Watch_Binary.ino:51-56` · **Delivered by Phase 2, untouched here**
- **Gap:** the hub/browser preflight that detects a duplicate or missing id at runtime is now *possible* (always-on per-node counters) but the operator view is not built → `A1_07` OPEN-PREFLIGHT
- **Diagnosis `CONFIRMED` · Resolution `CODE_FIXED` (Phase 2) + `HARDWARE_VALIDATION_PENDING`**

### `NODE-06` — dead Euler computation
- **Code fact:** `roll`/`pitch`/`yaw` local, never read `[fact-code]`
- **Selected:** removed, with `quatI..quatReal` and `ax/ay/az` (reference count 1 each)
- **Diagnosis `CONFIRMED` · Resolution `CODE_FIXED`** · saving unquantified

### `PWR-01` / `PWR-02` — battery, display, touch on the sample task
- **Anchor:** `Pod_Watch_Binary.ino:~950`
- **Code fact:** AXP202 ADC read + TFT redraw every 3 s on `TaskReadIMU`; comment said "every minute" `[fact-code]`
- **Selected:** correct the comment only. Moving the work is a performance change with no measurement
- **Diagnosis `CONFIRMED` (code) · Resolution `REQUIREMENT_PENDING`** → M-PWR01

### `NET-03` — fleet phase lockstep
- **Anchor:** `mesq_pod_core.h:255` `mesqDeadlineInit`
- **Selected:** deterministic offset `id × period / 17`
- **Test:** 17 strictly increasing deadlines ~1838 µs apart, all inside one period
- **Diagnosis `CONFIRMED` (code) · Resolution `CODE_FIXED` + `HARDWARE_VALIDATION_PENDING`** — arithmetic tested, **RF benefit untested**

---

## Hub — interface dependencies required to verify A1

Full detail in [`../02_hub/HUB_SOLVED.md`](../02_hub/HUB_SOLVED.md).

| ID | Anchor | Diagnosis | Resolution | Evidence |
|---|---|---|---|---|
| `HUB-02` two writers, one stream | `mesq_hub_core.h:160-220`; `Dongle_Binary.ino:424, 547` | `CONFIRMED` | `CODE_FIXED` | 2 threads, 25k records: no split/interleave; offered = accepted + dropped |
| `HUB-03` reset on any byte | `mesq_hub_core.h:60-150`; `Dongle_Binary.ino:568, 696` | `CONFIRMED` | `CODE_FIXED` | 100k random bytes + 5k JSON lines → 0 triggers |
| `HUB-04` `cleanupClients` | `Dongle_Binary.ino:282` | `CONFIRMED` | `CODE_FIXED` | 1 line; leak not reproduced off-device |
| `HUB-05`/`SYNC-03` silent loss | `mesq_hub_core.h:230-290` | `CONFIRMED` | `CODE_FIXED` | gap/dup/reorder/wrap/reboot all classified |
| `HUB-07` unchecked `add_peer` | `Dongle_Binary.ino:~455` | `CONFIRMED` | `CODE_FIXED` | flag set only on success |
| `HUB-06` empty tasks | `Dongle_Binary.ino:~530` | `CONFIRMED` | `ACCEPTED_RISK` | documented, not removed |
| **P3-01** instr build never compiled | `Dongle_Binary.ino:64-77` | `CONFIRMED` (new) | `CODE_FIXED` | `check_sketches.sh` |

---

## Browser and export

| ID | Anchor | Diagnosis | Resolution | Evidence |
|---|---|---|---|---|
| `WEB-02`/`WEB-PARSER` | `js/mesq_parser.js:215-330` | `CONFIRMED` | **`VERIFIED`** | 63 assertions; baseline 0/50 → 50/50 |
| `SYNC-03` gap detection | `js/mesq_instr.js` (Phase 2) + hub counters | `CONFIRMED` | `CODE_FIXED` | Phase 2 fixture; hub side now always-on |
| `SYNC-08` held frames | `js/webserialnative.js` `_podFresh` | `CONFIRMED` | `CODE_FIXED` | detected in the browser; **not yet in the BVH** → OPEN-BVH-HELD |
| `BVH-01`/`SYNC-07`/`WEB-03`/`EST-05` | `index.html:378-420`; `js/bvh_converter.js:~165` | `CONFIRMED` | **`VERIFIED`** | 18 assertions; 0.03125 measured vs 0.03333 asserted |
| **P3-02** truthiness guard | `js/bvh_converter.js:~165` | `CONFIRMED` (new) | **`VERIFIED`** | `export_tests.js` E3 |
| **P3-03** duplicated parser | `tools/replay_harness.js` | `CONFIRMED` (new) | `CODE_FIXED` | one module, both consumers |
| `PROTO-01` | `mesq_packet.h`; `js/mesq_parser.js` | `CONFIRMED` | `CODE_FIXED` | JS/C golden vectors byte-identical |
| `WEB-05` buffer realloc | `js/mesq_parser.js` | `CONFIRMED` | `CODE_FIXED` (partial) | `slice` releases the backing buffer; the per-read copy remains → backlog |

---

## Counts

| Diagnosis | n | | Resolution | n |
|---|---|---|---|---|
| `CONFIRMED` | 22 | | `VERIFIED` | 3 |
| `INCONCLUSIVE` | 1 | | `CODE_FIXED` | 15 |
| `FALSIFIED` | 0 | | `REQUIREMENT_PENDING` | 3 |
| `UNTESTED` | 0 | | `HARDWARE_VALIDATION_PENDING` | 4 (overlapping) |
| | | | `ACCEPTED_RISK` | 1 |

**No firmware item is `VERIFIED`**, because neither sketch has been built for the
target. The three `VERIFIED` rows are browser/export, where the host suites
satisfy every condition of §1.
