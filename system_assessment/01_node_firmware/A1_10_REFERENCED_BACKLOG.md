# A1_10 — Referenced Backlog

Real findings from other assessment sections that are **out of scope for A1** and
were deliberately not addressed. Listed so that scope control does not become
silent omission.

None of these blocks the A1 work. Several depend on it.

---

## Estimation and accuracy

| ID | Source | Why out of scope here | Depends on | Owning workstream |
|---|---|---|---|---|
| `EST-01` | P1 A5 | Heading is unobservable in 6-axis fusion. Not a node-firmware defect — an architecture property | M4 static session | Estimation |
| `EST-02` | P1 A5 / P2 D4 | Per-frame optimal yaw removes only 24–32% of root-relative error. Weakened, not refuted | `INT-04` | Estimation |
| `EST-04` | P1 A5 | 15 chips treated as identical; no per-sensor calibration exists | per-chip bench | Estimation |
| `KIN-01` | P1 A6 / P2 D3 | **ZUPT is feasible** — stance runs up to 3.3 s in every capture. The one accuracy fix needing no magnetometer | optical ground truth | Kinematics |
| `KIN-03` `KIN-05` `KIN-06` | P1 A6 | Hierarchy hand-unrolled; no joint limits; skeleton proportions differ (pelvis 55.5 vs 95.9) | — | Kinematics |
| `KIN-02` `KIN-04` | P1 A6 | Negative results / dead code. Retained for the record | — | — |

## Phone, SLAM, and the WebXR path

| ID | Source | Why out of scope | Owning workstream |
|---|---|---|---|
| `PHN-01` | P1 A7 | Position guard is a truthiness test — a zero coordinate drops the frame. **Same defect class as the two fixed here** (`P3-02`, the parser stall sentinel). Phase 2 instrumented it and deliberately did not change behaviour pending Rule 1 | Phone |
| `PHN-02` | P1 A7 | Partly covered by `HUB-02`, which is fixed. Re-evaluate after the hub is reflashed | Phone |
| `PHN-03` | P1 A7 | **Refuted** by Phase 2 D2 for existing captures. Retained as a resolved negative | — |
| `PHN-04` | P1 A7 | WebXR world frame and IMU heading frame never reconciled | Phone + Estimation |
| `PHN-05` | P1 A7 / P2 D1 | Root Euler `XYZ` puts yaw in the middle slot; up to **23.9%** of frames within 10° of gimbal lock (reference `YXZ`: 0.0%). Tracked as `OPEN-PHN05` in `A1_07` because the fix is a one-line export change, but the regression belongs to the export workstream | Export |

## Browser performance

| ID | Source | Why out of scope | Owning workstream |
|---|---|---|---|
| `WEB-01` | P1 A4 | Per-packet `moment.js` + `innerHTML` + jQuery, ~480/s on the main thread. A real performance defect, but it needs a DevTools profile on live traffic to size | Browser |
| `WEB-05` | P1 A4 | `_rxBuf` reallocated and copied per read. **Partly addressed** — the tail trim now uses `slice` so the backing buffer is released — but the per-read concatenation remains | Browser |
| `WEB-06` | P1 A4 | Kalman smoothing path commented out; no host-side filtering is active | Browser |
| `WEB-07` | P1 A4 | Rotation order `XYZ` vs reference `YXZ` — correctly declared, but a portability trap. Related to `PHN-05` | Export |

## Synchronisation

| ID | Source | Why out of scope | Owning workstream |
|---|---|---|---|
| `SYNC-01` | P1 A8 | 17 unrelated clock domains. Phase 3 made freshness detectable but did **not** create a shared timebase. That needs a protocol change (`D-03`) | Sync |
| `SYNC-05` | P1 A8 | Arrival-stamping converts jitter into 3–6° of joint error | Sync |
| `SYNC-06` | P1 A8 | The concrete sync scheme. Blocked on `D-03` | Sync |
| `SYNC-09` | P1 A8 | **Negative result:** crystal drift ~12 ms / 10 min, second-order | — |
| `SYNC-10` | P1 A8 | Decoupled logging viable (528 KB per 10-min session); needs flash partition enumeration on a device | Sync |

## Hub, non-A1

| ID | Source | Why out of scope | Owning workstream |
|---|---|---|---|
| `HUB-01` | P1 A2 | **Negative result:** `max_connection` cannot gate pods. The four-node theory is refuted | — |
| `HUB-06` | P1 A2 | Two of three FreeRTOS tasks are empty no-ops. Documented in place rather than removed — deleting task handles other code references is wider than this phase warrants | Hub |
| `HUB-08` | P1 A2 | **Negative result:** serial bandwidth 18% utilised — not a constraint | — |

## Network, blocked on `U1`

`NET-01`, `NET-02`, `NET-04` (batching), `NET-05` (RSSI — the hook exists under
`MESQ_INSTR`), `NET-06` (the hub's softAP beacons on its own receive channel).
All blocked on the PHY rate. Covered by `A1_04` and `A1_07` B3.

## Evaluation and benchmarking

| ID | Source | Why out of scope | Owning workstream |
|---|---|---|---|
| `P2-B10-02` | P2 B10 | **The benchmark maps no arm joints** — 14 of 22, all torso and legs. It is silent about the fastest-moving half of the body, which is also the half most exposed to timing jitter. This is a serious evaluation gap | Evaluation |
| `P2-B10-01` / `INT-04` | P2 | A ±1 s alignment change swings the per-frame yaw estimate across 1,176°. No further quantitative work on existing captures | Evaluation |
| `P2-B8-02` | P2 B8 | Foot vertical excursion implausibly small (1–4 units) | Kinematics |
| `KIN-06` retraction | P2 B11 | Absolute positional error in BVH units needs recomputation; not convertible to centimetres as it stands | Evaluation |

## Standing constraint from Phase 2

**Existing captures cannot be repaired.** They carry no arrival timestamps, so the
true frame interval is unrecoverable. The I12 fix — now actually wired and tested
— works **prospectively only**. Any timing claim about an existing capture must be
withdrawn or recaptured. See `system_assessment_2/RETRACTIONS.md`.
