# Node Firmware — Solved Issues

Phase 3 outcome for `01-node_firmware`, at a glance. Full detail in
[`A1_02_CHANGE_REPORT.md`](A1_02_CHANGE_REPORT.md); evidence in
[`A1_03_VERIFICATION_REPORT.md`](A1_03_VERIFICATION_REPORT.md).

> **`CODE_FIXED` is not `VERIFIED`.** No pod has been built for the target or run.
> Every firmware row below is a claim about source backed by host tests, not a
> claim about a device.

## Fixed in source, host-verified

| ID | Title | Evidence |
|---|---|---|
| `NODE-01` | Cross-core quaternion shared with no synchronisation → torn reads | 469/8,931 torn on the baseline structure → **0/141,749**, ledger balanced, 2 real threads |
| `NODE-02` | `ms_lo` stamped transmit, not sample | now the FIFO-decode time; held frames detectable; **no wire-format change** |
| `NODE-03` | `1000/32` truncated to 31 ms, strict `>`, drift re-anchored each send | **32 sends/s exactly**; 320–321 over 10 s under jitter, 0 resyncs; 65 across the µs wrap |
| `NODE-04` | Pod hung forever on IMU or DMP init failure | bounded 10 attempts, boot stages, **red fault screen**, slow retry — no hang, no reboot loop |
| `NODE-06` | Dead Euler computation on every sample | removed, with 7 other dead declarations |
| `SENS-03` | Unguarded `sqrt` → NaN → **the zero quaternion on the wire** | 200k random + 50k roundoff-shell inputs: every accepted result is a unit quaternion |
| `NET-03` | Fleet transmitted in lockstep after a reset | deterministic per-node phase; 17 deadlines ~1838 µs apart inside one period |
| `PROTO-01` | Wire layout implicit in struct layout | explicit serialisation; **C and JS golden vectors byte-identical** |

## Fixed and fully verified (browser/export — a real build exists)

| ID | Title | Evidence |
|---|---|---|
| `WEB-02` / `WEB-PARSER` | Unterminated JSON latched the parser permanently | baseline **0 of 50** frames → **50 of 50**; buffer bounded; 63 assertions |
| `BVH-01` / `SYNC-07` | Export asserted 1/30 over ~32 Hz data → 6.67% progressive dilation | measured `0.03125` with a `measured` provenance label; 18 assertions |
| `SYNC-08` | Held poses indistinguishable from measurements | detected per node in the browser (**not yet in the BVH** — `A1_07` OPEN-BVH-HELD) |

## New defects found this phase

| ID | Title | Found by |
|---|---|---|
| **P3-01** | The hub's `-DMESQ_INSTR=1` build had **never compiled** — `SYNC0`/`SYNC1` used above their `#define` | `tools/check_sketches.sh` |
| **P3-02** | The Phase 2 I12 export fix could never fire: nothing set `mesqFrameTiming`, and its guard was a truthiness test on a numeric timestamp | `tools/export_tests.js` |
| **P3-03** | The test fixture held a hand-copied duplicate of the parser, so a browser-parser fix was covered by no test | reading, confirmed by the shared-module refactor |

Also found *by the new tests, in my own new code*, and fixed before commit: an
inverted drop counter in `MesqSampleChannel` (caught by the `published = consumed
+ dropped` ledger), a 1-LSB mismatch between the JS `Math.round` encoder and the
firmware's truncating C cast (caught by the cross-language golden vector), and a
zero-sentinel in the parser's stall timeout (caught by the stall test).

## Deliberately not changed

| ID | Why |
|---|---|
| `SENS-01` DMP rate | `REQUIREMENT_PENDING`. The 60 Hz question is undecided and the fast-init path is `[unverified]` against the pinned library. **No register was invented.** |
| `SENS-02` unused raw streams | Performance change with no measurement. Behind `-DMESQ_DISABLE_UNUSED_DMP_STREAMS=1`, **default off** |
| `PWR-01` / `PWR-02` | Cost unmeasured. Only the wrong comment ("every minute" over a 3-second operation) was corrected |
| Magnetometer | Not a one-line change. Needs per-chip calibration, disturbance testing, and the owner's approval |
| Wire format v2 | Deliberately preserved v1. Migration drafted in `A1_01` §6, not implemented |

## Carried forward from Phase 2, untouched

`NODE-05` (per-node build identity), `tools/build_pods.sh`, the provenance
banners, the `MESQ_INSTR` instrumentation, and the radio channel/PS/TX-power
hardening were all delivered by Phase 2, verified present, and **not redone**.

## What to do next

1. **`arduino-cli` build both sketches** — `A1_07` B1. Everything else waits on it.
2. Read the pod boot banner → resolves `U2`.
3. `M-SENS01` — the distinct Quat6 rate. Decides whether 60 Hz is even a live
   question.
4. `M-NODE01` — confirm `normBad` stays 0 on real silicon.
5. `M-NODE04` — pull SDA, confirm the red screen, then 10 cold boots.

## Hub

Hub findings are in [`../02_hub/HUB_SOLVED.md`](../02_hub/HUB_SOLVED.md).
