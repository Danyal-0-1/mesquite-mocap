# Mesquite MoCap — Phase 1 System Assessment

Diagnosis only. **Zero code was changed** outside this directory.

---

> ### Phase 3 update (2026-09-20) — implementation has now happened
>
> The sentence above still describes **Phase 1**, which was diagnosis only.
> Phase 3 implemented and tested the A1 findings plus the hub dependencies needed
> to verify them, on branch `a1-implementation`. It did **not** redo
> `system_assessment_2/`; the Phase 2 delta is stated explicitly in
> [`01_node_firmware/A1_00_IMPLEMENTATION_PLAN.md`](01_node_firmware/A1_00_IMPLEMENTATION_PLAN.md) §3.
>
> - **Node:** [`01_node_firmware/SOLVED.md`](01_node_firmware/SOLVED.md) and the
>   `A1_00`–`A1_10` reports beside it
> - **Hub:** [`02_hub/HUB_SOLVED.md`](02_hub/HUB_SOLVED.md)
> - **Learn and retype:** [`01_node_firmware/A1_05_RETYPE_THESE.md`](01_node_firmware/A1_05_RETYPE_THESE.md)
>
> **No firmware was built for the target and nothing ran on hardware.** 189 host
> assertions pass across five suites (`tools/run_all_tests.sh`). `CODE_FIXED` is
> not `VERIFIED`.
>
> Three Phase 1 conclusions below need a footnote now: the "three latching
> failures" are fixed **in source** (Phase 2 had already reduced them to two);
> the DMP ~55 Hz figure remains a code comment, not a measurement; and the
> accuracy conclusion is unchanged — heading is still unreferenced.

---

## Read in this order

1. **[FINDINGS.md](FINDINGS.md)** — master ledger, ranked by (impact × confidence) ÷ cost
2. **[09_integration/BOTTLENECK_VERDICT.md](09_integration/BOTTLENECK_VERDICT.md)** — which ceiling binds, and why the answer is not one of the three candidates offered
3. **[09_integration/CONTRADICTIONS.md](09_integration/CONTRADICTIONS.md)** — eight rulings, including four refuted premises
4. **[INSTRUMENTATION.md](INSTRUMENTATION.md)** — I1–I12, and the re-scoped experiment table
5. **[OPEN_QUESTIONS.md](OPEN_QUESTIONS.md)** — everything unresolved, and what this pass already answered

Supporting: **[09_integration/RATES_TABLE.md](09_integration/RATES_TABLE.md)** (the seven rates),
**[09_integration/EVALUATION_DESIGN.md](09_integration/EVALUATION_DESIGN.md)**,
**[_BRIEFING.md](_BRIEFING.md)** (verified ground truth used throughout).

## Four premises in the master prompt are refuted by code

| Premise | Reality |
|---|---|
| §4.1 `max_connection` caps the fleet at 4 | Pods use **ESP-NOW** and never associate. No AP cap applies. **E1 retired.** |
| §2.2 the DMP may be sitting unused | **Already enabled** — GRV + FIFO. But raw accel/gyro are also enabled and never read. |
| §C2 no sequence numbers, no node timestamps | **Both on the wire.** `count` is never gap-checked; `ms_lo` stamps transmit, not sample. |
| §A6 sensor-to-segment calibration probably ignored | **Implemented** — solved from a T-pose, and the error data exonerates it. |

Two structural corrections to the §3 architecture diagram: node → hub is **ESP-NOW unicast** to a
hardcoded MAC, and hub → browser is **USB CDC serial**, not a WebSocket. The WebSocket carries the
*phone*, and its text is written into the same serial stream as the binary IMU packets.

## Three headline conclusions

1. **60 Hz is unreachable without changing acquisition.** The DMP tops out near 55 Hz and the
   transmit task is capped near 32 Hz. No network or browser work moves this.
2. **The intermittency (S3/S4) is not a throughput problem.** Three independent *latching* failures —
   a pod that hangs forever on init, a hub that reboots the fleet on any serial byte, and a browser
   parser that stops permanently on a truncated JSON line. Each needs a power cycle or reload.
3. **The accuracy problem is whole-body, not per-limb.** Global joint RMSE is flat at 42–54° across
   every joint *including the root*; root-relative pose RMSE is only 15–16°. **Do not replace the
   fusion filter.**

## Start here

`SYNC-03` (~10 lines, browser only, no wire-format change) and `HUB-05`/I1 (~20 lines at the hub).
Together they turn every symptom in the log from an anecdote into a number, and they discriminate
uniform airtime sag from node-specific collapse from queue-filling cliffs from parser stalls.
**Everything else should wait behind them.**

## Agent reports

| Agent | Report | Headline |
|---|---|---|
| A1 | [01_node_firmware](01_node_firmware/REPORT.md) | DMP enabled but capped at ~55 Hz; quaternion shared across cores unsynchronised; `ms_lo` stamps transmit not sample; pod hangs forever if init fails |
| A2 | [02_hub](02_hub/REPORT.md) | `max_connection` refuted; two task contexts write one unframed serial stream; **any** inbound byte reboots the suit; serial bandwidth eliminated as a constraint |
| A3 | [03_network](03_network/REPORT.md) | Airtime **57%** at 32 Hz / **106%** at 60 Hz *if* PHY rate is 1 Mbps — an unconfigured register decides it; unicast retries explain S2; RSSI free and discarded |
| A4 | [04_webapp](04_webapp/REPORT.md) | Parser is correct (chunk hypothesis disproved); per-packet moment.js + innerHTML ~480/s; unterminated JSON line halts binary parsing permanently; export hardcodes 1/30 s |
| A5 | [05_estimation](05_estimation/REPORT.md) | Fusion is all on the DMP, nothing downstream; yaw error **71–99°** is initialisation, not drift; ranked error budget — quantisation is 0.006° and irrelevant |
| A6 | [06_kinematics](06_kinematics/REPORT.md) | Calibration and composition are both correct; error profile is flat, exonerating per-limb causes; **no kinematic-constraint drift correction of any kind** |
| A7 | [07_phone_slam](07_phone_slam/REPORT.md) | Phone path is **live**, not vestigial; `Hips` is the phone, `HipsAlt` the pod; position guard drops zero coordinates; phone JSON shares the binary stream |
| A8 | [08_sync_integrity](08_sync_integrity/REPORT.md) | 15 unrelated clock domains; jitter → **3–6°** joint error; loss undetectable though `count` exists; concrete sync scheme specified |
| A9 | [09_integration](09_integration/REPORT.md) | Reconciliation, contradictions, rates table, bottleneck verdict, evaluation design |

## Definition of done

- [x] Every agent has a report in the required location and structure
- [x] Every claim tagged, with `file:line` where applicable
- [x] `FINDINGS.md` ranked by (impact × confidence) ÷ cost
- [x] `BOTTLENECK_VERDICT.md` adjudicates the three candidates — and explains why two other ceilings bind first
- [x] Every symptom S1–S6 has at least one candidate explanation
- [x] `CONTRADICTIONS.md` lists every disagreement with a resolving measurement
- [x] `RATES_TABLE.md` documents all seven rates with code locations
- [x] `INSTRUMENTATION.md` specifies I1–I12 concretely enough to implement
- [x] `OPEN_QUESTIONS.md` complete, with 11 seed questions answered or partly answered
- [x] **Zero code changed**

## Caveat

This pass is **code reading and analysis of existing captures**. No instrumentation was run and no
hardware was measured, so every performance claim is arithmetic or inference, tagged as such, and
carries a falsifying measurement. The airtime figures depend on an unmeasured PHY rate; the transmit
rate depends on an unknown tick rate; the DMP ceiling rests on the firmware's own comment. **Run I1
and I5 before acting on any ranking here.**
