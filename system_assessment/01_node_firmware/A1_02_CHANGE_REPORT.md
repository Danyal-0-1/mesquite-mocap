# A1_02 — Change Report

**What actually changed.** Proposals live in `A1_01_SOLUTION_DECISIONS.md`;
nothing proposed-but-not-done appears here.

Release candidate: branch `a1-implementation`, commits `32812a5`, `cc66387`,
`3718fa6` on top of `4487d56`. File hashes in `A1_03_VERIFICATION_REPORT.md` §1.

Diagnosis / resolution vocabularies are the master prompt's §1.

---

## Summary

| # | ID | Diagnosis | Resolution |
|---|---|---|---|
| 1 | `NODE-01` cross-core torn quaternion | `CONFIRMED` | `CODE_FIXED` + host-verified |
| 2 | `NODE-02` `ms_lo` stamps transmit | `CONFIRMED` | `CODE_FIXED` |
| 3 | `SENS-03` degenerate quaternion | `CONFIRMED` | `CODE_FIXED` + host-verified |
| 4 | `NODE-04` latching init failure | `CONFIRMED` | `CODE_FIXED`, `HARDWARE_VALIDATION_PENDING` |
| 5 | `NODE-03` transmit cadence | `CONFIRMED` | `CODE_FIXED` + host-verified |
| 6 | `NET-03` fleet phase lockstep | `CONFIRMED` (code) | `CODE_FIXED`, `HARDWARE_VALIDATION_PENDING` |
| 7 | `NODE-06` dead Euler computation | `CONFIRMED` | `CODE_FIXED` |
| 8 | `HUB-03` reset on any byte | `CONFIRMED` | `CODE_FIXED` + host-verified |
| 9 | `HUB-02` two writers, one stream | `CONFIRMED` | `CODE_FIXED` + host-verified |
| 10 | `HUB-04` `cleanupClients` never called | `CONFIRMED` | `CODE_FIXED` |
| 11 | `HUB-05`/`SYNC-03` silent loss | `CONFIRMED` | `CODE_FIXED` + host-verified |
| 12 | `HUB-07` unchecked `add_peer` | `CONFIRMED` | `CODE_FIXED` |
| 13 | `WEB-02`/`WEB-PARSER` latching parser | `CONFIRMED` | `VERIFIED` (host) |
| 14 | `SYNC-08` held frames indistinguishable | `CONFIRMED` | `CODE_FIXED` |
| 15 | `BVH-01`/`SYNC-07` export timing | `CONFIRMED` | `VERIFIED` (host) |
| 16 | **P3-01** hub `MESQ_INSTR` never compiled | `CONFIRMED` (new) | `CODE_FIXED` |
| 17 | **P3-02** I12 guard truthiness bug | `CONFIRMED` (new) | `VERIFIED` (host) |
| 18 | **P3-03** parser duplicated in fixture | `CONFIRMED` (new) | `CODE_FIXED` |
| 19 | `PROTO-01` implicit struct layout | `CONFIRMED` | `CODE_FIXED` + host-verified |
| 20 | `SENS-02` unused raw streams | `CONFIRMED` (code) | `REQUIREMENT_PENDING` (flagged, default off) |
| 21 | `PWR-01` battery/display on sample task | `CONFIRMED` (code) | `REQUIREMENT_PENDING` (comment fixed only) |
| 22 | P2 quality: `broadcastAddress` misnamed | `CONFIRMED` | `CODE_FIXED` |

`SENS-01` (DMP rate) is `REQUIREMENT_PENDING` — see §23.

---

## 1. `NODE-01` — coherent cross-core sample handoff

**Before.** `Device code/Pod_Watch_Binary/Pod_Watch_Binary.ino` declared
`struct Quat { float x, y, z, w; } quat;` at file scope. `TaskReadIMU` (core 1)
assigned the four fields at the end of the Quat6 branch; `TaskWifi` (core 0) read
them one at a time when building the packet. No mutex, no queue, not even
`volatile` — and `volatile` would not have helped: it stops compiler caching, not
a torn multi-word read across cores.

**After.**
- `Device code/Pod_Watch_Binary/mesq_pod_core.h` — new file. `MesqSample` (the
  record) and `MesqSampleChannel` (the length-one overwrite slot), lines 60–160.
- `Pod_Watch_Binary.ino:249` — `MesqSampleChannel g_sampleCh;`
- `Pod_Watch_Binary.ino:892-1010` (`TaskReadIMU`) — publishes one record.
- `Pod_Watch_Binary.ino:777-890` (`TaskWifi`) — takes one record with a freshness
  flag.
- The old `struct Quat` is **deleted**, not left unused, so the unsynchronised
  path cannot be reintroduced by accident. A comment marks where it was.

**Protocol impact.** None. Wire format untouched.

**Test.** `tools/firmware_tests.cpp` `test_node01()`. Two real threads, 400,000
publishes, producer alternating between two well-separated unit quaternions so a
mix is detectable with certainty. Asserts zero incoherent reads, that the
timestamp travelling with sequence N is the one written for N (a norm check alone
cannot see a cross-generation field), that the sequence never goes backwards, and
that `published = consumed + dropped`.

**Before/after.** Baseline structure, same harness: 8,931 reads, **469 with a
non-unit norm**. Fixed: 141,749 reads, **0 incoherent**, ledger balanced
(99,465 consumed + 300,535 dropped = 400,000 published).

**Residual risk.** The critical section's real cost on Xtensa silicon is
unmeasured. `A1_07` M-NODE01.

---

## 2. `NODE-02` — sample time on the wire

**Before.** `myData.ms_lo = (uint16_t)millis();` in the send branch — the moment of
transmission. The sample's own time never left the pod.

**After.** `Pod_Watch_Binary.ino:~820` passes `smp.sample_ms`, captured in
`TaskReadIMU` at FIFO decode and carried inside the coherent record.

**Protocol impact.** **Layout unchanged**: same 16 bytes, same offsets, same
endianness. One field's *meaning* changed. No correct consumer regresses — the
browser previously misread it as an age (`WEB-04`). Documented at the top of
`mesq_packet.h` and in `js/mesq_parser.js`.

**What it buys.** Two consecutive packets with the same `ms_lo` provably carry the
same sensor sample. That is `SYNC-08` (held-frame detection) for zero wire cost.

**What it does NOT buy — stated so it is not over-claimed.** This is a
`sample_observed_time`, not the DMP's internal sample instant: no interrupt edge
is wired. It wraps every 65.536 s. It shares no epoch with any other pod
(`SYNC-01`). **No cross-device latency may be computed from it.**

**Test.** `test_node02()` — decode time is preserved, a repeat take returns the
same record and reports not-fresh, overwrite keeps the newest, and 54 superseded
samples out of 55 are counted.

---

## 3. `SENS-03` — quaternion reconstruction policy

**Before.** `double q0 = sqrt(1.0 - ((q1*q1)+(q2*q2)+(q3*q3)));` unguarded. A
negative radicand gave NaN; `q_to_i16(NaN)` returns 0; all four components became
0 and the pod transmitted **the zero quaternion**.

**After.** `mesq_pod_core.h:190-240` — `mesqReconstructQuat()`, returning
`MESQ_Q_OK` / `MESQ_Q_REPAIRED` / `MESQ_Q_REJECTED`. Called at
`Pod_Watch_Binary.ino:992`. Rejected samples hold the previous pose and increment
`g_quatRejected`; repairs increment `g_quatRepaired`; both appear in the 1 Hz
instrumentation line.

**Threshold.** `MESQ_RADICAND_EPS = 1e-6`, ~180× the Q30 roundoff bound
(≈5.6 × 10⁻⁹) and ~10⁶× below a real failure.

**Test.** `test_sens03()`. 200,000 uniform random inputs: **every accepted result
is a unit quaternion**. 50,000 inputs inside the roundoff shell: all repaired,
none rejected, all finite and unit-norm. 10× the bound: rejected. The baseline
path is asserted too — unguarded `sqrt` gives NaN and `q_to_i16(NaN) == 0` — so
the defect stays demonstrable next to the fix.

---

## 4. `NODE-04` — bounded initialisation

**Before.** `while (!initialized) { myICM.begin(...); delay(500); }` — unbounded.
`if (!success) { ...; while (1) ; }` — permanent. `setupIMU()` returned `void`.

**After.** `Pod_Watch_Binary.ino:432-540`. `setupIMU()` returns `bool`. Ten
attempts with linear backoff (~11 s). `MesqBootStage` enum recorded at every step.
`mesqShowFault()` paints a red screen naming the failure. `setup()` starts the
tasks either way and logs the stage.

**Why not a reboot loop.** It would re-enter `setup()`, re-run the radio init, and
make a dead pod look intermittent — the exact diagnostic confusion Phase 1 was
trying to resolve.

**Test.** Type-checks in all three pod configurations. **Fault injection needs
hardware** — pulling SDA, an unpowered IMU, and ten cold boots per pod are
`A1_07` M-NODE04. Resolution is `CODE_FIXED`, **not** `VERIFIED`.

---

## 5–6. `NODE-03` / `NET-03` — deadline and fleet phase

**Before.** `if (millis() > (prev_ms + (1000 / fps)))` with `fps = 32`, then
`prev_ms = millis()` after the send.

**After.** `mesq_pod_core.h:250-290` (`MesqDeadline`), used at
`Pod_Watch_Binary.ino:777-790`. Period `1000000/32 = 31250 µs` exact; advance by
whole periods; catch-up bounded at 4 periods then resync with a counter; first
deadline seeded at `id × period / 17`.

**Test.** `test_node03()` asserts the baseline arithmetic (`1000/32 == 31`, strict
`>` first opens at 32 ms), then: exactly 32 sends per simulated second; 320–321
over 10 s under jitter with **0 resyncs**, i.e. no accumulated drift; a 5 s stall
yields **one** send, not a burst; 17 pods get 17 strictly increasing deadlines
~1838 µs apart, all inside one period; and 65 sends across the 32-bit microsecond
wrap with none lost.

**Behaviour change, flagged.** Offered load rises ~2.4% (31.25 → 32.0 attempts/s)
because the truncation is gone. Argued in `A1_01` §5. The rate was not raised
beyond the declared `fps = 32`.

**`NET-03` caveat.** The phase arithmetic is tested; the **RF benefit is not**.
`A1_07` M-NET03.

---

## 7. `NODE-06` — dead Euler computation

**Before.** Two `atan2`, one `asin`, plus the intermediate terms, computed on
every DMP sample into local variables `roll`, `pitch`, `yaw` that nothing read.

**After.** Removed. Also removed: `quatI/quatJ/quatK/quatReal` and `ax/ay/az`,
each confirmed by reference count to have exactly one occurrence — their own
declaration.

**Test.** No behavioural test; verified by reference counting and by the sketch
type-check. The saving is not quantified — no hardware.

---

## 8. `HUB-03` — framed host command

**Before.** `Dongle_Binary.ino` `loop()`: `if (Serial.available() > 0) {
Serial.readString(); sendReset(); }`.

**After.**
- `Device code/Dongle_Binary/mesq_hub_core.h:60-150` — `MesqCmdParser`,
  `mesqCmdFeed()`, `mesqCmdBuild()`.
- `Dongle_Binary.ino:568-610` — `hostCmdTask`.
- `Dongle_Binary.ino:696` — `loop()` now only sleeps.
- `js/mesq_parser.js` — `encodeCommand()`; `js/webserialnative.js` —
  `window.mesqRebootFleet()`; `js/custom_icm.js:1163,1239` — call sites updated
  from `window.sWrite("reboot")`.

**Compatibility.** Against a hub still running Phase 1 firmware, the **first byte**
of the new frame triggers the old reset — same outcome. The browser can therefore
be deployed before the hubs are reflashed. A Phase 3 hub ignores the old
`"reboot"` text entirely, so hubs must be flashed before that text is relied on
from anywhere else.

**Test.** `test_hubcmd()` — 26 bytes of noise, an ordinary JSON line, **100,000
random bytes** and **5,000 realistic JSON lines**: zero triggers. A valid frame
fires exactly once including byte-at-a-time; a corrupted checksum is rejected and
counted; an over-long declared length is refused at the length byte and the parser
recovers immediately; an unknown well-formed command is surfaced, not guessed.
Cross-language: JS and C produce byte-identical `aa55fc010001`.

---

## 9. `HUB-02` — single-owner USB stream

**Before.** `OnDataRecv` called `Serial.write(incomingData, 16)` from the WiFi
task; `handleWebSocketMessage` called `Serial.println(json)` from AsyncTCP; and
`onEvent` printed connect/disconnect text into the same live stream.

**After.**
- `mesq_hub_core.h:160-220` — `MesqUsbQueue` (128 × 96 B ring).
- `Dongle_Binary.ino:424` — receive callback copies 16 bytes and returns.
- `Dongle_Binary.ino:~470` — WebSocket handler enqueues its line whole.
- `Dongle_Binary.ino:547-566` — `usbWriterTask`, priority 2, **the only caller of
  `Serial.write()` for stream data**.
- `hubEmitStatus()` (`Dongle_Binary.ino:127`) — hub speech becomes framed `0xFE`
  records instead of raw text. The WS connect/disconnect prints now use it.

**Side benefit.** The callback no longer references the framework's borrowed RX
buffer after returning, and no longer blocks on I/O inside a WiFi callback.

**Test.** `test_hub02()` — two real producer threads (20,000 pod frames, 5,000
JSON lines) against one drain. Walks the drained bytes and asserts **no record is
split or interleaved**, `accepted + dropped = offered`, `pushed == popped`, and
`highWater ≤ 128`. The test deliberately over-drives the queue so the drop path is
exercised.

**Residual risk.** Real USB CDC throughput is untested. `A1_07` M-HUB02.

---

## 10–12. `HUB-04`, `HUB-05`/`SYNC-03`, `HUB-07`

- **`HUB-04`** — `ws.cleanupClients()` now called once per second in
  `podTimeoutTask` (`Dongle_Binary.ino:282`). One line; `AsyncWebSocket` retains
  disconnected client objects until it is called.
- **`HUB-05`/`SYNC-03`** — `MesqNodeStats` per bone id, **always on**, updated in
  the receive callback. Two framed status records per second carry per-node
  `received`/`lost`, queue depth and high-water, drops, peer failures and command
  counters. Phase 2 had this behind `MESQ_INSTR`; a measurement nobody flashes is
  not a measurement.
- **`HUB-07`** — `peerMacsInit[id]` is set **only** when `esp_now_add_peer()`
  returns `ESP_OK`; failures increment `g_peerAddFail`. Before, the flag was set
  first and the return ignored, so a failed add permanently hid that pod from
  `sendReset()`.

**Test.** `test_hubstats()` — 100 consecutive packets give no gaps; a 5-packet hole
is 1 gap / 5 lost; a repeat is a duplicate; an older counter is reordering; the
65535→0 wrap costs **0 lost**; and a pod reboot is **1 resync, not ~65,000 lost**.
That last case did not exist before and would have poisoned every loss statistic
in any session where a pod restarted.

---

## 13. `WEB-02` / `WEB-PARSER` — bounded parser, one implementation

**Before.** The state machine was inline in `js/webserialnative.js`, gated on
`_jsonLine.length === 0`, with an unbounded string accumulator. Phase 2's T6(b)
proved a permanent latch when payload bytes avoid `0x0A`.

**After.** `js/mesq_parser.js` — new, 385 lines, loaded by **both** the browser
(`index.html:347`) and `tools/parser_tests.js`. Bounded at 512 B per JSON line,
4096 B of buffer, 2000 ms stall. Deterministic resync on `0xAA 0x55` inside a JSON
line. Bone id validated before consuming 16 bytes. Tail trim uses `slice`, so the
backing `ArrayBuffer` is released.

**Before/after, same fault bytes, in one output** (`tools/replay_harness.js` T10):

| Fault | Phase 2 baseline | Phase 3 |
|---|---|---|
| Unterminated JSON + 50 frames, no `0x0A` in payload | **0 of 50 decoded**, accumulator grew to 823 B and rising | **50 of 50**, pending buffer 0 B |
| Binary frame spliced into a JSON line | **both records destroyed** | pod frame decoded, JSON line discarded and counted |

**Test.** `tools/parser_tests.js`, 63 assertions: golden vectors, exhaustive split
points at every byte boundary, byte-at-a-time, seeded fuzz over 200 chunkings,
3,000 bytes of seeded noise then recovery, 25 KB of junk under the buffer bound,
disconnect in every parser state, counter wrap, and the NaN policy.

---

## 14. `SYNC-08` — held frames are now visible

`js/webserialnative.js` classifies each pod frame using the new `ms` semantics and
maintains `window._podFresh[bone] = { fresh, held, lastMs }`; each delivered object
carries `held: true|false`. Surfaced in `_mocapDebug()`.

**Not yet done:** the recorder does not yet propagate `held` into the BVH, so
measured and held frames are still not distinguished *in the export*. Recorded in
`A1_07` as OPEN-BVH-HELD.

---

## 15, 17. `BVH-01` / `SYNC-07` and the guard bug

**Before.** Phase 2 added a measured-frame-time branch to `generateBVH()`, but
nothing ever set `window.mesqFrameTiming`, so every export used the asserted
`1/30` — ~6.7% progressive dilation over ~32 Hz data, the reason cross-correlation
against ground truth never locked.

**After.**
- `index.html:378-420` — the recorder populates `mesqFrameTiming` from
  `performance.now()` (monotonic, immune to wall-clock steps) plus ISO anchors,
  and sets the authoritative frame count from `recordedMotionData.length`.
- `js/bvh_converter.js:~165` — the guard was `_ft.startMs && _ft.endMs && ...`,
  a **truthiness test on a numeric timestamp**: `startMs: 0` is falsy, so the
  measured branch was skipped. Now `typeof v === 'number' && isFinite(v)`. Same
  defect class as `PHN-01`.

**Test.** `tools/export_tests.js`, 18 assertions: 600 frames spanning 18.75 s
export `Frame Time: 0.03125` labelled `measured`; the fallback still works and is
labelled `ASSUMED_1_30_UNTRUSTWORTHY`; the 6.67% dilation is quantified and shown
to be zero once wired; four degenerate timing inputs all fall back rather than
emitting a zero or negative frame time; and the file's structure is checked.

---

## 16, 18, 19. New Phase 3 findings

- **P3-01** — the hub's `-DMESQ_INSTR=1` build had **never compiled**:
  `mesq_emitStatus()` used `SYNC0`/`SYNC1` above their `#define`. Fixed by moving
  the wire-format block above the instrumentation block
  (`Dongle_Binary.ino:64-77`). Found by `tools/check_sketches.sh`.
- **P3-02** — the I12 truthiness guard, §15 above.
- **P3-03** — `tools/replay_harness.js` tested a hand-copied duplicate of the
  parser, so a browser-parser fix was covered by nothing. Resolved by the shared
  module; the harness is now explicitly the **baseline** reproduction and carries
  a Phase 3 comparison section.
- **`PROTO-01`** — `mesq_packet.h` serialises field by field rather than relying on
  struct layout, with a `static_assert` on size. JS and C golden vectors are
  asserted byte-identical (`aa550257ff3f01c0ff1fff5f3412cdab`). Writing that test
  immediately found a **1-LSB mismatch**: the JS encoder used `Math.round` while
  the firmware's C cast truncates toward zero. The JS side now truncates.

---

## 20–23. Deliberately not changed

- **`SENS-02`** — behind `-DMESQ_DISABLE_UNUSED_DMP_STREAMS=1`, **default off**.
  Performance change, no measurement. `A1_07` M-SENS02.
- **`PWR-01`/`PWR-02`** — only the wrong comment fixed ("every minute" over a
  3-second operation). The work stays on the sample task pending measurement.
- **`SENS-01`** — DMP rate. `REQUIREMENT_PENDING`: the 60 Hz question is
  undecided, and `Example10_DMP_FastMultipleSensors` compatibility with the pinned
  library is `[unverified]`. **No register was invented.**
- **Magnetometer** — not enabled. `A1_01` §11.
- **Wire format** — not versioned. `A1_01` §6.
- **`broadcastAddress` → `hubAddress`** — renamed; it is a unicast MAC and the old
  name cost review time on every read of the radio path.

---

## Files changed

**Pod firmware**
- `Device code/Pod_Watch_Binary/Pod_Watch_Binary.ino` — modified
- `Device code/Pod_Watch_Binary/mesq_pod_core.h` — new
- `Device code/Pod_Watch_Binary/mesq_packet.h` — new

**Hub firmware**
- `Device code/Dongle_Binary/Dongle_Binary.ino` — modified
- `Device code/Dongle_Binary/mesq_hub_core.h` — new

**Browser**
- `js/mesq_parser.js` — new
- `js/webserialnative.js` — parser core replaced by the shared module
- `js/bvh_converter.js` — guard fix
- `js/custom_icm.js` — two reboot call sites
- `index.html` — parser script tag, I12 recorder wiring

**Tests and tooling**
- `tools/firmware_tests.cpp` — new, 76 assertions
- `tools/parser_tests.js` — new, 63 assertions
- `tools/export_tests.js` — new, 18 assertions
- `tools/check_sketches.sh` — new
- `tools/arduino_stubs/mesq_stubs.h`, `tools/arduino_stubs/preprocess.py` — new
- `tools/run_all_tests.sh` — new
- `tools/replay_harness.js` — reframed as baseline, T10 comparison added

**Documentation** — this directory, plus `system_assessment/02_hub/`.

## Rollback

`git checkout 4487d56 -- "Device code" js index.html` restores every source file.
The three Phase 3 commits are independent in dependency order: reverting `3718fa6`
alone undoes the hub changes and leaves the pod and browser work intact.
