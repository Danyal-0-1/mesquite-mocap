# A1_09 — Wall Checklist

Print this. Work top to bottom: **Learn → Confirm → Instrument → Measure → Decide
→ Implement → Validate → Write**. The dependency order is real — measuring before
confirming the build, or deciding before measuring, produces numbers you cannot
defend.

IDs are shared with `A1_06_TRACEABILITY_MATRIX.md`. The sortable version is
`A1_09_WALL_CHECKLIST.csv`.

**Never tick a box because code was edited.** Use the diagnosis and resolution
vocabularies from the master prompt §1. `CODE_FIXED` is not `VERIFIED`.

**The gate everything else waits behind is `C-01`** — neither sketch has been
built for the target. Until that passes, every firmware row below is a claim about
source, not about a device.


## Learn

### ☐ `L-01` — Retype mesqReconstructQuat   ·   **P1** · node

> The zero quaternion reached the wire; you must be able to explain the threshold

| | |
|---|---|
| **Prerequisite** | none |
| **Evidence needed** | A1_05 §3 |
| **Falsifying test** | an accepted result with norm != 1 |
| **Code target** | `mesq_pod_core.h` |
| **Before → After** | NaN -> (0,0,0,0) → every accepted result unit-norm |
| **Risk** | none |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `CODE_FIXED` |
| **Owner** | Daynal |
| **Paper impact** | Methods: quaternion validity policy |
| **Artifact** | A1_05_RETYPE_THESE.md #1 |
| **Notes** | desktop only |

### ☐ `L-02` — Retype mesq_pack + q_to_i16   ·   **P1** · protocol

> Byte order and truncation are paper-relevant and were 1 LSB wrong in JS

| | |
|---|---|
| **Prerequisite** | L-01 |
| **Evidence needed** | A1_05 §6 |
| **Falsifying test** | golden hex differs between C and JS |
| **Code target** | `mesq_packet.h` |
| **Before → After** | implicit struct layout → explicit, byte-identical both languages |
| **Risk** | none |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `CODE_FIXED` |
| **Owner** | Daynal |
| **Paper impact** | Methods: wire format |
| **Artifact** | A1_05_RETYPE_THESE.md #2 |
| **Notes** | desktop only |

### ☐ `L-03` — Retype MesqDeadline   ·   **P1** · node

> Integer time and wrap arithmetic; three defects in two lines

| | |
|---|---|
| **Prerequisite** | L-02 |
| **Evidence needed** | A1_05 §4 |
| **Falsifying test** | fire count drifts over 10 s |
| **Code target** | `mesq_pod_core.h` |
| **Before → After** | <=31.25/s, drifting → 32/s exactly, no drift |
| **Risk** | +2.4% offered load |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `CODE_FIXED` |
| **Owner** | Daynal |
| **Paper impact** | Methods: transmit cadence |
| **Artifact** | A1_05_RETYPE_THESE.md #3 |
| **Notes** | desktop only |

### ☐ `L-04` — Retype MesqSampleChannel   ·   **P0** · node

> The hardest idea in the system: a pose is one record, not four numbers

| | |
|---|---|
| **Prerequisite** | L-03 |
| **Evidence needed** | A1_05 §5 |
| **Falsifying test** | a read spanning two generations |
| **Code target** | `mesq_pod_core.h` |
| **Before → After** | 469/8931 torn → 0/141749 torn |
| **Risk** | critical-section cost unmeasured |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `CODE_FIXED` |
| **Owner** | Daynal |
| **Paper impact** | Methods: cross-core handoff |
| **Artifact** | A1_05_RETYPE_THESE.md #4 |
| **Notes** | desktop only |

### ☐ `L-05` — Retype the parser JSON branch   ·   **P1** · browser

> Bounded recovery and the ASCII invariant

| | |
|---|---|
| **Prerequisite** | L-04 |
| **Evidence needed** | A1_05 §7 |
| **Falsifying test** | an unterminated line swallowing a frame |
| **Code target** | `js/mesq_parser.js` |
| **Before → After** | 0/50 frames → 50/50 frames |
| **Risk** | none |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `VERIFIED` |
| **Owner** | Daynal |
| **Paper impact** | Methods: stream parsing |
| **Artifact** | A1_05_RETYPE_THESE.md #5 |
| **Notes** | desktop only |

## Confirm

### ☐ `C-01` — Build both sketches with arduino-cli   ·   **P0** · build

> Nothing firmware-side has been compiled for the target

| | |
|---|---|
| **Prerequisite** | toolchain install |
| **Evidence needed** | A1_03 §4 |
| **Falsifying test** | a compile error against the real libraries |
| **Code target** | `both .ino` |
| **Before → After** | type-check only → real build + sizes |
| **Risk** | library signatures may differ from stubs |
| **Diagnosis** | `UNTESTED` |
| **Resolution** | `BLOCKED` |
| **Owner** | Daynal |
| **Paper impact** | blocks every firmware claim |
| **Artifact** | A1_07 B1 |
| **Notes** | THE blocker |

### ☐ `C-02` — Read the pod boot banner   ·   **P0** · node

> Resolves U2: core/IDF version and tick rate

| | |
|---|---|
| **Prerequisite** | C-01 |
| **Evidence needed** | A1_00 §3 |
| **Falsifying test** | banner absent or tick rate != 1000 |
| **Code target** | `Pod_Watch_Binary.ino` |
| **Before → After** | U2 unresolved → U2 resolved |
| **Risk** | none |
| **Diagnosis** | `UNTESTED` |
| **Resolution** | `BLOCKED` |
| **Owner** | Daynal |
| **Paper impact** | three Phase 1 claims depend on tick rate |
| **Artifact** | A1_07 OPEN-U2 |


### ☐ `C-03` — Verify I2C wiring GPIO21/22   ·   **P1** · node

> Pins are code-selected; wiring is unverified

| | |
|---|---|
| **Prerequisite** | hardware |
| **Evidence needed** | A1_07 OPEN-I2C |
| **Falsifying test** | continuity test disagrees |
| **Code target** | `setupIMU()` |
| **Before → After** | [unverified] → [fact-measured] |
| **Risk** | do NOT remap before this |
| **Diagnosis** | `UNTESTED` |
| **Resolution** | `BLOCKED` |
| **Owner** | Daynal |
| **Paper impact** | Methods: hardware description |
| **Artifact** | A1_07 OPEN-I2C |


### ☐ `C-04` — Establish the ESP-NOW PHY rate (U1)   ·   **P0** · radio

> Dominates every airtime and capacity claim

| | |
|---|---|
| **Prerequisite** | C-01 + sniffer |
| **Evidence needed** | A1_04 §2 |
| **Falsifying test** | measured rate != 1 Mbps assumption |
| **Code target** | `both .ino` |
| **Before → After** | assumed 1 Mbps → measured |
| **Risk** | Phase 1 airtime figures may be far too pessimistic |
| **Diagnosis** | `UNTESTED` |
| **Resolution** | `BLOCKED` |
| **Owner** | Daynal |
| **Paper impact** | Results: airtime |
| **Artifact** | A1_07 B3 |


### ☐ `C-05` — Set and log esp_wifi_set_country()   ·   **P2** · radio

> Regulatory region is never declared in code

| | |
|---|---|
| **Prerequisite** | C-01 |
| **Evidence needed** | A1_04 §7 |
| **Falsifying test** | country unset in the banner |
| **Code target** | `both .ino` |
| **Before → After** | unset → US, logged |
| **Risk** | compliance is EIRP, not a software value |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `NOT_STARTED` |
| **Owner** | Daynal |
| **Paper impact** | Methods: regulatory |
| **Artifact** | A1_07 OPEN-RF-COUNTRY |


## Instrument

### ☐ `I-01` — Measure instrumentation self-cost   ·   **P1** · node

> Rule 2: unsatisfied since Phase 2

| | |
|---|---|
| **Prerequisite** | C-01 |
| **Evidence needed** | A1_03 §7 |
| **Falsifying test** | instr_us/s is a material fraction of the period |
| **Code target** | `MESQ_INSTR` |
| **Before → After** | never read → a number |
| **Risk** | results untrustworthy until done |
| **Diagnosis** | `UNTESTED` |
| **Resolution** | `BLOCKED` |
| **Owner** | Daynal |
| **Paper impact** | gates every I-series result |
| **Artifact** | A1_07 OPEN-INSTR-COST |


### ☐ `I-02` — Build the operator preflight view   ·   **P2** · browser

> All the data exists; nothing displays it

| | |
|---|---|
| **Prerequisite** | none |
| **Evidence needed** | A1_07 OPEN-PREFLIGHT |
| **Falsifying test** | an operator cannot see a missing pod before capture |
| **Code target** | `index.html` |
| **Before → After** | no view → id/MAC/fw/batt/fresh/gaps |
| **Risk** | none |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `NOT_STARTED` |
| **Owner** | Daynal |
| **Paper impact** | operational, not paper |
| **Artifact** | A1_07 OPEN-PREFLIGHT |
| **Notes** | no hardware needed |

## Measure

### ☐ `M-01` — M-SENS01 distinct Quat6/s   ·   **P0** · node

> Turns Phase 1's ~55 Hz comment into a number; decides the 60 Hz question

| | |
|---|---|
| **Prerequisite** | C-01,I-01 |
| **Evidence needed** | A1_07 M-SENS01 |
| **Falsifying test** | quat6/s materially != 55 |
| **Code target** | `setupIMU()` |
| **Before → After** | code comment → [fact-measured] |
| **Risk** | if 60 Hz is hard, nothing here reaches it |
| **Diagnosis** | `INCONCLUSIVE` |
| **Resolution** | `REQUIREMENT_PENDING` |
| **Owner** | Daynal |
| **Paper impact** | Results: sensor rate |
| **Artifact** | A1_07 M-SENS01 |
| **Notes** | do before any rate decision |

### ☐ `M-02` — M-NODE01 handoff health   ·   **P0** · node

> normBad must stay 0 on real silicon

| | |
|---|---|
| **Prerequisite** | C-01,I-01 |
| **Evidence needed** | A1_07 M-NODE01 |
| **Falsifying test** | any normBad > 0 |
| **Code target** | `MesqSampleChannel` |
| **Before → After** | 469 torn (host baseline) → expect 0 |
| **Risk** | regression would be serious |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `HARDWARE_VALIDATION_PENDING` |
| **Owner** | Daynal |
| **Paper impact** | Methods: concurrency |
| **Artifact** | A1_07 M-NODE01 |


### ☐ `M-03` — M-NODE04 fault injection + 10 cold boots   ·   **P1** · node

> The init hang is fixed in source only

| | |
|---|---|
| **Prerequisite** | C-01 |
| **Evidence needed** | A1_07 M-NODE04 |
| **Falsifying test** | any hang or reboot loop |
| **Code target** | `setupIMU()` |
| **Before → After** | hangs forever → bounded + red screen |
| **Risk** |  |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `HARDWARE_VALIDATION_PENDING` |
| **Owner** | Daynal |
| **Paper impact** | Results: reliability |
| **Artifact** | A1_07 M-NODE04 |


### ☐ `M-04` — M-SENS02 raw-stream A/B   ·   **P2** · node

> Performance change, not yet justified

| | |
|---|---|
| **Prerequisite** | M-01 |
| **Evidence needed** | A1_07 M-SENS02 |
| **Falsifying test** | quat6/s drops with streams off |
| **Code target** | `MESQ_DISABLE_UNUSED_DMP_STREAMS` |
| **Before → After** | default off → decide from data |
| **Risk** |  |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `REQUIREMENT_PENDING` |
| **Owner** | Daynal |
| **Paper impact** | Results: FIFO cost |
| **Artifact** | A1_07 M-SENS02 |


### ☐ `M-05` — M-PWR01 battery/display cost   ·   **P2** · node

> Comment was 20x wrong; cost unmeasured

| | |
|---|---|
| **Prerequisite** | C-01 |
| **Evidence needed** | A1_07 M-PWR01 |
| **Falsifying test** | read_us spikes > 5 ms on the tick |
| **Code target** | `handleBattDisplay()` |
| **Before → After** | unmeasured → a number |
| **Risk** |  |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `REQUIREMENT_PENDING` |
| **Owner** | Daynal |
| **Paper impact** | Results: power |
| **Artifact** | A1_07 M-PWR01 |


### ☐ `M-06` — M-HUB02 USB queue under load   ·   **P1** · hub

> Queue depth is a calculation, not a measurement

| | |
|---|---|
| **Prerequisite** | C-01 |
| **Evidence needed** | A1_07 M-HUB02 |
| **Falsifying test** | dropped > 0 |
| **Code target** | `MesqUsbQueue` |
| **Before → After** | no queue at all → expect dropped 0 |
| **Risk** | real CDC throughput unknown |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `HARDWARE_VALIDATION_PENDING` |
| **Owner** | Daynal |
| **Paper impact** | Results: hub throughput |
| **Artifact** | A1_07 M-HUB02 |


### ☐ `M-07` — M-HUB03 reset path on hardware   ·   **P0** · hub

> Noise must not reset the fleet

| | |
|---|---|
| **Prerequisite** | C-01 |
| **Evidence needed** | A1_07 M-HUB03 |
| **Falsifying test** | any reset from noise |
| **Code target** | `hostCmdTask` |
| **Before → After** | any byte reset 17 pods → 0 from 4 KB urandom |
| **Risk** |  |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `HARDWARE_VALIDATION_PENDING` |
| **Owner** | Daynal |
| **Paper impact** | Results: reliability |
| **Artifact** | A1_07 M-HUB03 |


### ☐ `M-08` — M-HUB05 integrity vs known loss   ·   **P1** · hub

> Reboot must read as resync, not 65000 lost

| | |
|---|---|
| **Prerequisite** | C-01 |
| **Evidence needed** | A1_07 M-HUB05 |
| **Falsifying test** | lost jumps by tens of thousands |
| **Code target** | `MesqNodeStats` |
| **Before → After** | loss invisible → classified |
| **Risk** |  |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `HARDWARE_VALIDATION_PENDING` |
| **Owner** | Daynal |
| **Paper impact** | Results: loss |
| **Artifact** | A1_07 M-HUB05 |


### ☐ `M-09` — M-FLEET scaling 1/4/8/12/15/17   ·   **P1** · radio

> No fleet measurement has ever been taken

| | |
|---|---|
| **Prerequisite** | C-01,C-04 |
| **Evidence needed** | A1_07 M-FLEET |
| **Falsifying test** | delivery degrades between 8 and 17 |
| **Code target** | `whole chain` |
| **Before → After** | none → per-node distributions |
| **Risk** | declare thresholds BEFORE running |
| **Diagnosis** | `UNTESTED` |
| **Resolution** | `BLOCKED` |
| **Owner** | Daynal |
| **Paper impact** | Results: scaling |
| **Artifact** | A1_07 M-FLEET |


### ☐ `M-10` — M-SOAK 30-minute full fleet   ·   **P2** · radio

> Long-run stability is unknown

| | |
|---|---|
| **Prerequisite** | M-09 |
| **Evidence needed** | A1_07 M-SOAK |
| **Falsifying test** | any unexplained reset or deadlock |
| **Code target** | `whole chain` |
| **Before → After** | none → a soak result |
| **Risk** |  |
| **Diagnosis** | `UNTESTED` |
| **Resolution** | `BLOCKED` |
| **Owner** | Daynal |
| **Paper impact** | Results: endurance |
| **Artifact** | A1_07 M-SOAK |


## Decide

### ☐ `D-01` — Is 60 fresh orientations/s hard?   ·   **P0** · owner

> Everything about rate, airtime and scaling depends on it

| | |
|---|---|
| **Prerequisite** | M-01 |
| **Evidence needed** | A1_00 §2 |
| **Falsifying test** | none - this is a requirement, not a fact |
| **Code target** | `n/a` |
| **Before → After** | undecided → decided |
| **Risk** | several decisions stay pending |
| **Diagnosis** | `NOT_APPLICABLE` |
| **Resolution** | `REQUIREMENT_PENDING` |
| **Owner** | owner |
| **Paper impact** | frames the whole contribution |
| **Artifact** | A1_00 §7 |


### ☐ `D-02` — Approve or refuse the magnetometer   ·   **P1** · owner

> Only path to absolute heading; not a one-line change

| | |
|---|---|
| **Prerequisite** | D-01 |
| **Evidence needed** | A1_01 §11 |
| **Falsifying test** | none |
| **Code target** | `setupIMU()` |
| **Before → After** | disabled → decided |
| **Risk** | needs per-chip calibration + disturbance testing |
| **Diagnosis** | `NOT_APPLICABLE` |
| **Resolution** | `REQUIREMENT_PENDING` |
| **Owner** | owner |
| **Paper impact** | Limitations: heading |
| **Artifact** | A1_01 §11 |


### ☐ `D-03` — Approve or refuse a wire-format v2   ·   **P2** · owner

> Needed for sample_seq, boot id, a real timebase

| | |
|---|---|
| **Prerequisite** | D-01 |
| **Evidence needed** | A1_01 §6 |
| **Falsifying test** | none |
| **Code target** | `mesq_packet.h` |
| **Before → After** | v1 preserved → decided |
| **Risk** | invalidates old captures if mishandled |
| **Diagnosis** | `NOT_APPLICABLE` |
| **Resolution** | `REQUIREMENT_PENDING` |
| **Owner** | owner |
| **Paper impact** | Methods: protocol |
| **Artifact** | A1_01 §6 |


### ☐ `D-04` — One channel or several   ·   **P2** · radio

> Cannot be decided from modelling alone

| | |
|---|---|
| **Prerequisite** | M-09 |
| **Evidence needed** | A1_04 |
| **Falsifying test** | step 6 beats step 5 materially |
| **Code target** | `both .ino` |
| **Before → After** | one channel → decided from data |
| **Risk** | step 5 control is the one usually skipped |
| **Diagnosis** | `UNTESTED` |
| **Resolution** | `HARDWARE_VALIDATION_PENDING` |
| **Owner** | Daynal |
| **Paper impact** | Results: architecture |
| **Artifact** | A1_04 §5 |


## Implement

### ☐ `P-01` — Propagate held frames into the BVH   ·   **P1** · browser

> Detected in the browser, still conflated in the export

| | |
|---|---|
| **Prerequisite** | none |
| **Evidence needed** | A1_07 OPEN-BVH-HELD |
| **Falsifying test** | export cannot distinguish measured from held |
| **Code target** | `bvh_converter.js` |
| **Before → After** | conflated → flagged |
| **Risk** |  |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `NOT_STARTED` |
| **Owner** | Daynal |
| **Paper impact** | Limitations: held frames |
| **Artifact** | A1_07 OPEN-BVH-HELD |
| **Notes** | no hardware needed |

### ☐ `P-02` — Change root Euler order XYZ -> YXZ   ·   **P2** · browser

> Phase 2 measured up to 23.9% of frames near gimbal lock

| | |
|---|---|
| **Prerequisite** | none |
| **Evidence needed** | A1_07 OPEN-PHN05 |
| **Falsifying test** | near-singular frame fraction stays high |
| **Code target** | `bvh_converter.js` |
| **Before → After** | 23.9% near singular → expect ~0% |
| **Risk** | needs regression vs existing captures |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `NOT_STARTED` |
| **Owner** | Daynal |
| **Paper impact** | Results: export quality |
| **Artifact** | A1_07 OPEN-PHN05 |
| **Notes** | no hardware needed |

## Validate

### ☐ `V-01` — Independent review of the ESP32 assumptions   ·   **P1** · process

> Everything is SELF_REVIEWED

| | |
|---|---|
| **Prerequisite** | none |
| **Evidence needed** | A1_03 §10 |
| **Falsifying test** | a reviewer disputes the critical-section or priority choices |
| **Code target** | `both core headers` |
| **Before → After** | self-reviewed only → reviewed |
| **Risk** |  |
| **Diagnosis** | `NOT_APPLICABLE` |
| **Resolution** | `ACCEPTED_RISK` |
| **Owner** | Daynal + reviewer |
| **Paper impact** | confidence in Methods |
| **Artifact** | A1_07 OPEN-REVIEW |


## Write

### ☐ `W-01` — Scope every communication claim   ·   **P1** · paper

> A claim without node count, duration, PHY and thresholds is not a claim

| | |
|---|---|
| **Prerequisite** | M-09,M-10 |
| **Evidence needed** | A1_03 §12 |
| **Falsifying test** | a reviewer asks under what conditions |
| **Code target** | `paper` |
| **Before → After** | unscoped → scoped |
| **Risk** |  |
| **Diagnosis** | `NOT_APPLICABLE` |
| **Resolution** | `NOT_STARTED` |
| **Owner** | Daynal |
| **Paper impact** | Results section |
| **Artifact** | A1_03 §12 |


### ☐ `W-02` — State heading as an origin, not drift   ·   **P0** · paper

> Calling it drift is wrong and a reviewer will catch it

| | |
|---|---|
| **Prerequisite** | none |
| **Evidence needed** | A1_05 §3 |
| **Falsifying test** | none - this is already established |
| **Code target** | `paper` |
| **Before → After** | ambiguous → precise |
| **Risk** |  |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `NOT_STARTED` |
| **Owner** | Daynal |
| **Paper impact** | Limitations |
| **Artifact** | RETRACTIONS.md |
| **Notes** | Phase 2 confirmed the sign flip |

### ☐ `W-03` — Re-export benchmarks with measured frame time   ·   **P0** · paper

> Existing captures carry no arrival timestamps and cannot be repaired

| | |
|---|---|
| **Prerequisite** | P-01 |
| **Evidence needed** | A1_03 §6 |
| **Falsifying test** | exported duration != capture duration |
| **Code target** | `bvh_converter.js` |
| **Before → After** | 6.67% dilation → 0% |
| **Risk** | old captures are unrepairable - recapture |
| **Diagnosis** | `CONFIRMED` |
| **Resolution** | `VERIFIED` |
| **Owner** | Daynal |
| **Paper impact** | Results: all timing |
| **Artifact** | A1_02 §15 |
| **Notes** | I12 fires now |

---

## Counts

| Lane | Rows |
|---|---|
| Learn | 5 |
| Confirm | 5 |
| Instrument | 2 |
| Measure | 10 |
| Decide | 4 |
| Implement | 2 |
| Validate | 1 |
| Write | 3 |
| **Total** | **32** |

| Resolution | Rows |
|---|---|
| `ACCEPTED_RISK` | 1 |
| `BLOCKED` | 7 |
| `CODE_FIXED` | 4 |
| `HARDWARE_VALIDATION_PENDING` | 6 |
| `NOT_STARTED` | 6 |
| `REQUIREMENT_PENDING` | 6 |
| `VERIFIED` | 2 |
