# A1_01 — Solution Decisions

Why each change is the change it is, and what would justify revisiting it. This
is the **solution** report; what actually changed is `A1_02_CHANGE_REPORT.md`.

Format: context → evidence → requirements → alternatives → comparison → selected
→ why → why not the others → risks → rollback → revisit condition.

---

## 1. `NODE-01` — the cross-core quaternion

**Context.** `TaskReadIMU` on core 1 wrote four floats; `TaskWifi` on core 0 read
them. No synchronisation of any kind.

**Evidence.** `[fact-code]` Phase 1 `struct Quat { float x,y,z,w; } quat;` written
field-by-field at the end of the Quat6 branch, read field-by-field when building
the packet. `volatile` was not even present — and would not have helped: on a
dual-core ESP32 `volatile` prevents compiler caching, not a torn multi-word read.

**Requirement.** A transmitted pose must be *one* orientation. A mix of two is not
a rotation; its norm is not 1 and every downstream composition inherits the error.

**Alternatives.**

| Option | Verdict |
|---|---|
| **A. `portMUX` critical section around a 24-byte copy** | **SELECTED** |
| B. Seqlock with acquire/release atomics | rejected |
| C. FreeRTOS queue, depth N | rejected |
| D. Length-one overwrite queue (`xQueueOverwrite`) | rejected, narrowly |
| E. Leave it; add a norm counter only | rejected |

**Comparison.** B is lock-free and would be marginally cheaper, but it depends on
memory-ordering guarantees I cannot verify against the exact Xtensa toolchain
without hardware, and a subtly wrong barrier reintroduces the same tearing while
*looking* correct. C is wrong on its merits: the DMP produces ~55 samples/s and
the radio sends ~32/s, so a depth-N queue hands the transmitter a backlog of
stale orientations after any hiccup — worse than dropping them. D is behaviourally
right and is what `MesqSampleChannel` implements; a raw `xQueueOverwrite` was
rejected only because it carries no freshness flag and no drop counter, both of
which are needed here. E was rejected because a norm check is *necessary but not
sufficient* — two nearby orientations can mix into something whose norm is still
within tolerance.

**Selected.** `MesqSampleChannel` in `mesq_pod_core.h`: a length-one slot copied
under a short critical section, carrying quaternion + sample time + sequence +
validity as one record, plus `fresh` and `dropped`.

**Why it dominates.** The critical section is ~24 bytes and cannot tear. Nothing
is called inside it — no I²C, no radio, no `Serial`. The record shape is what
makes `NODE-02` and `SYNC-08` free rather than separate work. And the ledger
`published = consumed + dropped` is checkable, which is how the test caught an
inverted counter in my own first draft.

**Risks.** A critical section masks interrupts on the calling core. At ~24 bytes
this is sub-microsecond; it has not been measured on real silicon.

**Rollback.** Delete the channel, restore the four floats from git. One file.

**Revisit if.** Bench measurement shows the critical section costs anything
measurable at 55 Hz, or a toolchain audit establishes the atomics story well
enough to justify B.

---

## 2. `NODE-02` — timestamp and sequence semantics

**Context.** `ms_lo` carried `millis()` at transmit. The sample's own time was
never on the wire.

**Evidence.** `[fact-code]` Phase 1 `myData.ms_lo = (uint16_t)millis();` inside
the send branch. `[fact-data]` Phase 1 `SYNC-02`: the field stamps transmission.

**Requirement.** A consumer must be able to tell a fresh measurement from a
repeated one. The master prompt also requires the 16-byte frame be preserved
unless a versioned migration is approved.

**Alternatives.**

| Option | Verdict |
|---|---|
| **A. Redefine `ms_lo` as sample-decode time. No layout change.** | **SELECTED** |
| B. Add `sample_seq` and `boot_id` fields → new wire version | deferred, drafted §6 |
| C. Change `count` to the sample sequence | **rejected — actively harmful** |
| D. Leave it | rejected |

**Comparison.** C deserves a specific warning because it is the obvious move and
it is wrong: the DMP produces ~55 samples/s and the radio sends ~32/s, so a
sample counter on the wire would appear to gap ~23 times a second and would
destroy the `SYNC-03` gap detection that Phase 1 ranked as the single cheapest
useful change. Gaps in `count` must keep meaning *radio loss*.

**Selected.** A. `ms_lo` is the pod clock at FIFO decode. Two consecutive packets
carrying the same `ms_lo` provably carry the same sensor sample.

**Why.** Zero wire cost, zero compatibility cost, and it yields held-frame
detection (`SYNC-08`) for free. The field was useless before — the browser already
misread it as an age (`WEB-04`) — so no correct consumer regresses.

**Limits, stated plainly.** This is a `sample_observed_time`, **not** the DMP's
internal sample instant: no interrupt edge is wired, so the true instant is
unverified and must not be claimed. It still wraps every 65.536 s and still shares
no epoch with any other pod (`SYNC-01`). It is a **freshness marker, not a
timebase**, and no cross-device latency may be computed from it.

**Revisit if.** The owner approves a wire version bump, or an INT pin is wired and
a true acquisition instant becomes available.

---

## 3. `SENS-03` — invalid quaternion policy

**Context.** `q0 = sqrt(1 - (q1²+q2²+q3²))` with no guard.

**Evidence.** `[fact-code]` unguarded `sqrt` in the Quat6 branch;
`[fact-code]` `q_to_i16(NaN)` returns 0, so all four components became 0 and the
pod transmitted **the zero quaternion** — not a rotation, no inverse, silently
corrupting every composition it reaches.

**Requirement.** Never emit an invalid rotation. Distinguish fixed-point roundoff
from a genuinely bad sample.

**Alternatives.** (i) clamp everything to zero and continue; (ii) reject
everything negative; (iii) **threshold: clamp true roundoff, reject the rest**;
(iv) normalise unconditionally.

**Comparison.** (i) turns a bad sample into a plausible-looking wrong pose, which
is the worst outcome — undetectable downstream. (ii) discards legitimate samples:
Q30 quantisation alone can push the radicand to about −5.6 × 10⁻⁹. (iv) hides the
distinction entirely.

**Selected.** (iii), with `MESQ_RADICAND_EPS = 1e-6`.

**The arithmetic behind the threshold.** Each Q30 component carries at most 2⁻³⁰
of quantisation error, so the sum of three squares can overshoot 1 by at most
about 3 × 2 × 1 × 2⁻³⁰ ≈ 5.6 × 10⁻⁹. The threshold sits ~180× above that bound
and ~10⁶× below a real failure (a garbage sample gives a radicand of order −1).
The two cases cannot be confused. Verified in `tools/firmware_tests.cpp`: 50,000
inputs inside the shell are all repaired, 10× the bound rejects, and across
200,000 random inputs **every accepted result is a unit quaternion**.

**On rejection.** A rejected sample holds the previous orientation and the next
packet is marked held. That is honest. Inventing a plausible rotation is not.

**Revisit if.** Bench data shows `qRej` non-trivially above zero — that would mean
a real sensor or bus problem, not a policy problem.

---

## 4. `NODE-04` — initialisation recovery

**Evidence.** `[fact-code]` `while (!initialized) { ... }` — unbounded; and
`while (1) ;` on DMP failure — permanent. A pod in either state displayed its bone
name, updated its battery bar, and transmitted nothing.

**Alternatives.** (i) keep; (ii) bounded retry then `ESP.restart()`; (iii)
**bounded retry, explicit error state, slow background retry**; (iv) bounded retry
then deep sleep.

**Comparison.** (ii) was rejected specifically: a reboot loop re-enters `setup()`,
re-runs the radio init, and makes a *dead* pod look *intermittent* — which is
precisely the diagnostic confusion Phase 1 was trying to resolve. (iv) hides the
pod entirely.

**Selected.** (iii). Ten attempts, linear backoff (~11 s total), a boot stage
recorded at every step, **the fault painted on the watch in red**, and a slow
retry afterwards. `setupIMU()` returns `bool`; the tasks start either way.

**Why the screen matters.** 17 identical black watches on a body. A pod that
cannot reach its IMU must not look like a pod with a radio problem. This is the
cheapest possible operator-facing diagnostic and it needs no tooling.

---

## 5. `NODE-03` / `NET-03` — transmit cadence and fleet phase

**Evidence.** `[fact-code]` `if (millis() > (prev_ms + (1000 / fps)))` with
`fps = 32`. Three defects in two lines: `1000/32` truncates to 31; strict `>`
first opens at 32 ms; and `prev_ms = millis()` re-anchors to the *actual* send
time, so lateness accumulates permanently instead of cancelling.

**Selected.** A fixed-grid deadline in **microseconds** (`31250` exact), advanced
by whole periods, with bounded catch-up (max 4 periods, then resync) and a
deterministic per-node phase offset `id × period / 17`.

**Honest note on scope.** Correcting the truncation raises the ceiling from
~31.25 to 32.0 attempts/s — about +2.4% offered load. That is a behaviour change
made without a measurement, and it is made deliberately: the code declared 32 fps
and delivered less, which is a correctness defect in the arithmetic, not a
performance tuning decision. The rate was **not** raised beyond the declared 32.

**Why bounded catch-up.** After a 5 s stall, firing four packets back to back
dumps a burst into contended airtime for no benefit. One send, and count the
resync.

**Why the phase offset.** A fleet-wide reset leaves 17 pods transmitting in
lockstep, all contending for the same slot (`NET-03`). The offset spreads them
across one period, costs nothing, and needs no coordination. **Unvalidated on
hardware** — the arithmetic is tested, the RF benefit is not.

---

## 6. Protocol versioning — deliberately NOT done

The 16-byte frame is **unchanged**: same length, same offsets, same endianness.
Only `ms_lo`'s meaning changed, and its previous meaning had no correct consumer.

A version bump would be the right home for `sample_seq`, a boot/session id, and a
real timebase. It is **not implemented** because it requires the owner's approval
and a migration that covers mixed-fleet behaviour, rollout order, rollback and
golden fixtures. The sketch of that proposal:

> Reserve bone id `0xFD` for a `pose_v2` record. Pods announce their version in a
> boot status frame. The hub forwards both kinds unchanged. The browser decodes
> both. Old pods and new pods coexist in one capture; the browser reports per-node
> protocol version in the preflight view. Rollback is a reflash of the pods only.

Do not implement this before there is a measured need for the extra fields.

---

## 7. `HUB-03` — the reset command

**Evidence.** `[fact-code]` `if (Serial.available() > 0) { Serial.readString();
sendReset(); }`. **Any** byte reset all 17 pods.

**Alternatives.** (i) keep; (ii) a magic string; (iii) **a framed, checksummed
binary command**; (iv) a second UART.

**Comparison.** (ii) is better than (i) but still fires on a prefix collision and
gives no integrity check on a fragmented write. (iv) needs a second cable to every
deployment.

**Selected.** (iii): `[0xAA][0x55][0xFC][cmd][len][payload][xor]`. `0xFC` is
outside the bone range and distinct from `0xFE` (hub→host) and `0xFF` (hub→pod).

**What the checksum does and does not do.** It catches *accidental* corruption —
line noise, a half-written frame. It is **not authentication**. Anyone who can
open the port can send a valid frame. That is acceptable for a USB cable in a lab
and is stated rather than implied. A CRC would not change this.

**Verified.** 100,000 random bytes and 5,000 realistic JSON lines: zero triggers.
A valid frame fires exactly once, including byte-at-a-time. A corrupted checksum
is rejected and counted.

---

## 8. `HUB-02` — two writers on one stream

**Evidence.** `[fact-code]` `OnDataRecv` called `Serial.write(16 bytes)` from the
WiFi task while `handleWebSocketMessage` called `Serial.println(json)` from the
AsyncTCP task. `[fact-measured]` Phase 2 F2: one interleave destroys **both**
records.

**Alternatives.** (i) a mutex around every `Serial.write`; (ii) **a bounded queue
with a single writer task**; (iii) frame the JSON so collisions are at least
detectable; (iv) move the phone to a second interface.

**Comparison.** (i) is disqualified outright: it would hold a lock across
`Serial.write()` *inside the ESP-NOW receive callback*, which is exactly the
blocking-in-a-callback the framework forbids. (iii) treats the symptom. (iv)
changes the deployment.

**Selected.** (ii). `MesqUsbQueue`, 128 slots × 96 bytes. Producers copy a whole
record and return; `usbWriterTask` is the only caller of `Serial.write()`.

**Why this is the right shape, not just a fix.** Copying in the callback also
satisfies the rule that borrowed RX data must be copied into owned storage before
the callback returns — Phase 1 was passing the framework's buffer straight to a
blocking write. And backpressure becomes a **counter** (`dropped`, `highWater`)
instead of corruption.

**Sizing.** 17 pods × 32 Hz ≈ 544 records/s; 128 slots ≈ 235 ms of slack. Deep
enough to ride out a USB stall, shallow enough to bound added latency.

**Drop policy.** Drop the **newest** on overflow, not the oldest. The oldest is
closer to being written; dropping the newest keeps the stream self-consistent and
bounds latency instead of letting it grow.

**Unvalidated.** The queue is proven correct under two host threads. Whether
`highWater` ever approaches 128 with a real 17-pod fleet on real USB is exactly
what `A1_07` M-HUB02 measures.

---

## 9. Browser parser architecture — `WEB-02` / `WEB-PARSER`

**Evidence.** `[fact-code]` the JSON branch was gated on `_jsonLine.length === 0`,
so once a line opened, every byte including binary frames went into a string
accumulator until a `0x0A`. `[fact-measured]` Phase 2 F1: it self-clears in ~16
packets because `0x0A` occurs in binary payloads — **but** Phase 2's own T6(b)
proved it latches **permanently** when payload bytes happen to avoid `0x0A`, with
`_jsonLine` growing without bound.

**Selected.** A single bounded state machine in `js/mesq_parser.js`, with three
abandon paths and one deterministic resync:

- **max JSON line** 512 B, **max buffer** 4096 B, **stall timeout** 2000 ms;
- **the resync**: valid JSON here is 7-bit ASCII, so the bytes `0xAA 0x55`
  **cannot** occur inside a well-formed JSON line. Finding them there is *proof*
  of a hub-side interleave, not a guess. The text line is abandoned and the binary
  frame survives.

That last point is why this is a correctness fix rather than a heuristic. Phase 1
`WEB-07`/T8 established the ASCII property; this exploits it.

**Also fixed.** The old parser consumed a full 16 bytes on a false sync with an
invalid bone id; it now validates the id first and skips one byte. And the tail
trim uses `slice`, not `subarray`, so the backing `ArrayBuffer` is released.

**The other half of the decision: one implementation.** Phase 2's fixture held a
hand-copied duplicate of the parser marked "kept in sync manually", so a fix in
the browser was covered by no test at all. `js/mesq_parser.js` is now loaded by
both the browser and `tools/parser_tests.js`. This was the single highest-leverage
structural change in the phase.

---

## 10. Radio: one channel or several

Fully argued in `A1_04_RADIO_CHANNEL_STUDY.md`. Summary: **stay on one fixed
channel (Option A)**, because a multi-channel deployment cannot be selected from
modelling alone and no fleet measurement exists. The comparison protocol and the
code hooks are written; the decision is `HARDWARE_VALIDATION_PENDING`.

---

## 11. Heading and the magnetometer — explicitly NOT changed

Six-axis Quat6 fusion has **no Earth-referenced yaw**. Phase 1's 71–99° session-to-
session error is an arbitrary heading *origin*, not gyro drift, and Phase 2
confirmed the sign flips between sessions — the signature of an unreferenced
frame.

Enabling the magnetometer is **not** a one-line change. It needs hard- and
soft-iron calibration per chip, disturbance testing in the actual capture volume
(17 powered ESP32s, a steel-frame building, laptops), and a decision about what
happens when the field is disturbed mid-capture. It requires the owner's explicit
approval and is out of scope here. `KIN-01`/ZUPT remains the Phase 2-identified
alternative that needs no magnetometer.

---

## 12. `SENS-02` — measured before changed

The two raw DMP streams are enabled and never read `[fact-code]`. Disabling them
shortens every FIFO read. That is a **performance** change and Gate 5 requires a
confirmed material effect first. The change is written and behind
`-DMESQ_DISABLE_UNUSED_DMP_STREAMS=1`, **defaulting to current behaviour**, with
the A/B protocol in `A1_07` (M-SENS02). It is deliberately not on.

The same reasoning applies to `PWR-01`: the battery read and TFT redraw sit on the
sample task every 3 s, the cost is unmeasured, so only the wrong comment (it said
"every minute") was corrected.

---

## 13. New library creation

No new third-party library. Four project-local headers/modules were created, each
against the §9 test: a named unmet requirement, no existing option that meets it,
a small stable API, and deterministic tests.

| Module | Requirement it meets |
|---|---|
| `mesq_pod_core.h` | coherent handoff, quaternion policy, deadline — none testable off-device before |
| `mesq_packet.h` | explicit serialisation instead of relying on struct layout |
| `mesq_hub_core.h` | command framer, USB queue, integrity counters |
| `js/mesq_parser.js` | one parser for browser and tests, instead of two copies |

A replacement ICM-20948 or fusion library was **not** written and would need far
stronger evidence than exists.
