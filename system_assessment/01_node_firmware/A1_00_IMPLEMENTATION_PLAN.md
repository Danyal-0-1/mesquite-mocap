# A1_00 — Implementation Plan and Execution Mode

**Phase 3.** Written 2026-09-20. Branch `a1-implementation`, forked from `4487d56`.

> **Scope note, read first.** This phase does **not** repeat Phase 2
> (`system_assessment_2/`). Phase 2's delivered work — the per-node build system,
> the provenance banners, the `MESQ_INSTR` instrumentation, the replay fixture, the
> I12 export branch, the browser I5/I6 counters — was inspected, found present in
> the tree, and **kept**. Section 3 below is the explicit delta. Two Phase 2 items
> turned out to be broken, not merely unrun; those are called out rather than
> quietly rewritten.

---

## 1. Execution mode

**`REPOSITORY_ONLY`.**

| Resource | State | Consequence |
|---|---|---|
| Pods (T-Watch 2019) | **none attached** | no sensor, timing, boot, power or radio measurement |
| Hub (T-Display-S3) | **none attached** | no USB, no ESP-NOW, no fleet test |
| `arduino-cli` / PlatformIO | **not installed** | **no target build of either sketch** |
| RF monitor, power meter, optical ground truth | none | radio and accuracy claims cannot be validated |
| `git` | **working** (Phase 2 was blocked on an Xcode licence; this machine is Linux) | branch-per-wave discipline is available again |
| `g++ 13`, `node 24.18`, `python3` | available | host tests, threaded race tests, type-checking |

The absence of hardware limits resolution status. It does not stop source
inspection, host tests, instrumentation design, or runbook authoring, and it did
not: 184 host assertions now run, and two suites found real defects.

## 2. Requirements table

The master prompt requires these to be recorded **before** judging performance.
Most are still undecided by the owner, and that is stated rather than guessed.

| Requirement | Value | Status |
|---|---|---|
| Target pod count | 15–17 | `[fact-doc]` |
| Hard distinct fresh-orientation rate | **UNDECIDED** — is 60 Hz a scientific requirement or a proxy for visible choppiness? | `REQUIREMENT_PENDING` |
| End-to-end latency definition and budget | **UNDECIDED** | `REQUIREMENT_PENDING` |
| Per-node loss / longest-outage limit | **UNDECIDED** | `REQUIREMENT_PENDING` |
| Automatic recovery deadline | **UNDECIDED** | `REQUIREMENT_PENDING` |
| Capture / soak duration | ~10 min typical, 30 min target | `[inference]` from Phase 1 |
| Heading accuracy requirement | **UNDECIDED**; 6-axis fusion cannot supply absolute heading at all | `REQUIREMENT_PENDING` |
| Regulatory region | Arizona, United States → FCC, channels 1–11 | `[fact-doc]` |

Only the *dependent design decisions* are marked `REQUIREMENT_PENDING`. Every
correctness, memory-safety, boundedness and recovery defect was fixed regardless,
which is what the master prompt's Gate 5 permits.

## 3. What Phase 2 already did — and was NOT redone

Verified present in the working tree before any edit.

| Phase 2 item | Found | Phase 3 action |
|---|---|---|
| `NODE-05` per-node identity via `-DMESQ_POD_ID`, `#error` if undefined | present, `Pod_Watch_Binary.ino:51-56` | **kept untouched** |
| `tools/build_pods.sh` (17 images + SHA-256 manifest) | present | **kept untouched** |
| Provenance banners on pod and hub | present | kept; extended with boot stage |
| `MESQ_INSTR` pod instrumentation (I4, I8, I9, I11, N1, N2, N3) | present | kept; retargeted at the new counters |
| `MESQ_INSTR` hub instrumentation (I1, I2, I3, I7, I11, H1, H2, H3) | present | kept; **had never compiled** — see below |
| Browser I5/I6 counters, `js/mesq_instr.js` | present | kept, rewired to the shared parser |
| `tools/replay_harness.js` | present, 23/23 | kept as the **baseline** reproduction; a Phase 3 comparison section added |
| I12 measured frame time in `bvh_converter.js` | present | kept; **the missing wiring supplied** and a guard bug fixed |
| Radio channel lock / PS off / TX power on both sides | present | kept untouched |
| Framed `0xFE` hub status marker and its browser branch | present | kept; promoted out of the instrumentation-only build |

**Two Phase 2 items were broken, not merely unrun.** Phase 2's own README records
that nothing was ever compiled and no gate opened, which is exactly how both
survived:

1. **The hub's `MESQ_INSTR=1` build never compiled.** `mesq_emitStatus()`
   referenced `SYNC0`/`SYNC1` above their `#define`. A macro is textual, so the
   instrumentation build failed outright. Found by `tools/check_sketches.sh`.
2. **The I12 export fix could never fire.** Nothing populated
   `window.mesqFrameTiming`, and its guard was a truthiness test on a numeric
   timestamp, so even a correctly populated `startMs: 0` fell back to the
   asserted 1/30. Found by `tools/export_tests.js`.

Neither is a criticism of Phase 2's analysis, which was sound. They are the
predictable cost of code that no test and no compiler ever saw — which is why
this phase's first deliverable was a test harness, not a fix.

## 4. System and dependency map

```
body motion
  → ICM-20948 DMP, Quat6 / Game Rotation Vector, 6-axis, magnetometer OFF
  → I²C GPIO21/22 @ 400 kHz  [pins are code-selected; wiring UNVERIFIED]
  → TaskReadIMU (core 1): FIFO drain, Q30 → double, validity policy
  → MesqSampleChannel  ← THE cross-core boundary
  → TaskWifi (core 0): deadline, pack, ESP-NOW unicast to one hub MAC, ch 1
  → hub ESP-NOW recv callback: validate, count, COPY into MesqUsbQueue
  → usbWriterTask: the ONLY Serial.write() for stream data
  → USB CDC
  → js/mesq_parser.js: bounded three-record state machine
  → custom_icm.js: calibration, skeleton, render
  → bvh_converter.js: measured frame time, BVH
```

The phone/WebXR path joins at the hub's WebSocket and is multiplexed into the
same USB stream as newline-terminated JSON. That multiplexing is the origin of
`HUB-02` and `WEB-02`.

## 5. Ordered gates and their outcome

| Gate | Outcome |
|---|---|
| −1 declare mode and requirements | **PASS** — `REPOSITORY_ONLY`, §1–2 above |
| 0 preserve and inventory | **PASS** — branch, baseline hashes, Phase 2 delta |
| 1 reproduce the current system | **PARTIAL** — host suites reproduce; **no target build possible** |
| 2 master issue and dependency matrix | **PASS** — `A1_06_TRACEABILITY_MATRIX.md` |
| 3 instrument without changing behaviour | **PASS** — Phase 2's flag kept; new counters are always-on and framed |
| 4 baseline measurements | **BLOCKED** — no hardware. Runbooks in `A1_07` |
| 5 compare and choose solutions | **PASS** — `A1_01_SOLUTION_DECISIONS.md` |
| 6 implement in dependency order | **PASS** — three commits, tests after each |
| 7 independent review and regression | **PASS (`SELF_REVIEWED`)** — no second agent was used |
| 8 release-candidate freeze | **PASS** — hashes in `A1_03` |

## 6. Agent roles

**No subagents were used.** One implementer did discovery, implementation and a
separate adversarial review pass. Every review finding is therefore labelled
**`SELF_REVIEWED`** and carries the weaker confidence that implies. This is a
declared limitation, not an omission.

## 7. Decision points that need the owner

1. **Is 60 fresh orientations/s a hard requirement?** Everything about DMP rate,
   airtime and fleet scaling hangs on this.
2. **May the magnetometer be enabled** as a separate calibrated, disturbance-tested
   architecture decision? It is the only path to absolute heading; it is not a
   one-line change and has not been made.
3. **Is a wire-format version bump acceptable?** Phase 3 deliberately preserved the
   16-byte frame. A migration proposal is drafted in `A1_01` §6 but not implemented.
4. **May `SENS-02` be A/B'd on a real pod?** The change is written and behind a
   flag, defaulting to current behaviour.

See `A1_07_OPEN_ITEMS_AND_HARDWARE_RUNBOOK.md` for what each needs.
