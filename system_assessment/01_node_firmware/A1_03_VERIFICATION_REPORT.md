# A1_03 — Verification Report

**Read this first: no hardware was tested.** No pod, no hub, no radio, no USB
cable, no battery, no ground truth. Everything below is a host-side result. Every
claim about fleet behaviour, timing on silicon, power, range or accuracy remains
**unmeasured**.

---

## 1. Release-candidate freeze

Branch `a1-implementation` @ `3718fa6`, base `4487d56`.

```
2a250598ebf7e27e7404a56d36d835ecdd4af97021280960308e28b7a39d22d5  Device code/Pod_Watch_Binary/Pod_Watch_Binary.ino
ed21f9fb31d2d9e944ef8ca5063a21c563224fe047c3fe9177648e06907ed920  Device code/Pod_Watch_Binary/mesq_pod_core.h
5f4ac65c2100da5ce1db31dac50663d1b893c4e9dca07fc04056709baf4e73f2  Device code/Pod_Watch_Binary/mesq_packet.h
a154af5bf0b1f564a5dccf85148c70e4e09d9d8eb49f11e241b138283153105e  Device code/Dongle_Binary/Dongle_Binary.ino
ef14443d3af73d562c8f95e3128f02aacac9ae7b66b6f8472f64bb45b96ea442  Device code/Dongle_Binary/mesq_hub_core.h
af829a6d784e28cd347bcddd5b82613228eba42c144657abbf187e86730ad0c2  js/mesq_parser.js
5093b6e2ca44316f1de6bc6644a3a4686f712b6b7d82a8fa56f27d75d3afbb44  js/webserialnative.js
b8a4a4094e2f00ce18f5f3ace026b3d93b4da0578e6f434dce03c3c3658d1cc3  js/bvh_converter.js
```

Any later edit invalidates the freeze for that component and requires its suites
to be rerun and these hashes regenerated.

## 2. Environment

| | |
|---|---|
| OS | Linux 7.0.0-31-generic |
| Compiler | `g++` 13, `-std=c++17 -O2 -pthread` |
| Node | v24.18.0 |
| Python | 3 |
| `arduino-cli` / PlatformIO | **absent** |
| Serial devices | **none** |

## 3. Commands

```bash
tools/run_all_tests.sh          # everything below
tools/check_sketches.sh         # sketch type-check only
node tools/parser_tests.js
node tools/export_tests.js
node tools/replay_harness.js
g++ -std=c++17 -O2 -pthread -DMESQ_HOST_TEST \
    -I"Device code/Pod_Watch_Binary" -I"Device code/Dongle_Binary" \
    tools/firmware_tests.cpp -o /tmp/mesq_fw_tests && /tmp/mesq_fw_tests
```

## 4. Build results

| Target | Result | Meaning |
|---|---|---|
| Pod sketch, `-DMESQ_POD_ID=3` | **type-check OK** | not a target build |
| Pod, `+ -DMESQ_INSTR=1` | **type-check OK** | not a target build |
| Pod, `+ -DMESQ_DISABLE_UNUSED_DMP_STREAMS=1` | **type-check OK** | not a target build |
| Hub sketch | **type-check OK** | not a target build |
| Hub, `-DMESQ_INSTR=1` | **type-check OK** | **was broken before this phase** |
| `tools/firmware_tests.cpp` | **builds and runs** | real target-shared headers |

**What the type-check is.** `tools/check_sketches.sh` reproduces the Arduino
builder's auto-prototyping (via real `cpp` output, so `#if` branches resolve),
substitutes `tools/arduino_stubs/mesq_stubs.h` for the libraries, and asks `g++`
to compile the sketch body. It proves the sketch is valid C++ and that every call
matches the declared signature.

**What it is not.** It does **not** prove the real libraries agree at their pinned
versions, that the image links, that it fits in flash, or that it runs. **A real
`arduino-cli` build has not happened and is the first open item in `A1_07`.**

It nonetheless paid for itself immediately: it found that the hub's
`-DMESQ_INSTR=1` build had never compiled at all (P3-01).

## 5. Automated test matrix

| Suite | Assertions | Result |
|---|---|---|
| Sketch type-check, 5 configurations | 5 | **5 OK** |
| `tools/firmware_tests.cpp` | 76 | **76 pass, 0 fail** |
| `tools/parser_tests.js` | 63 | **63 pass, 0 fail** |
| `tools/export_tests.js` | 18 | **18 pass, 0 fail** |
| `tools/replay_harness.js` (baseline + comparison) | 27 | **27 pass, 0 fail** |
| **Total** | **189** | **0 failures** |

### Baseline failures preserved

Phase 2's `tools/replay_harness.js` passed 23/23 before this phase and passes
27/27 now (4 added by the Phase 3 comparison section). **No pre-existing failure
was inherited, and none was introduced.** There is no other test suite in the
repository; `package.json` declares no test script.

## 6. Key before/after results

### `NODE-01` — torn cross-core reads

| | Reads | Non-unit norm |
|---|---|---|
| Phase 1 structure, same harness | 8,931 | **469** |
| `MesqSampleChannel` | 141,749 | **0** |

Plus: sequence/timestamp/quaternion always from one generation, sequence never
decreasing, and `99,465 consumed + 300,535 dropped = 400,000 published`.

The drop of ~75% is **not loss** — it is the intended ~55 Hz → ~32 Hz decimation,
now counted instead of assumed.

### `WEB-02` — the latching parser

| Fault | Baseline | Phase 3 |
|---|---|---|
| Unterminated JSON + 50 `0x0A`-free frames | **0 decoded**, accumulator 823 B and growing | **50 decoded**, pending 0 B |
| Binary spliced into a JSON line | **both records destroyed** | pod frame kept, JSON discarded and counted |
| 3,000 B seeded noise | n/a | recovers, buffer within 4096 B |
| 25 KB junk | n/a | bounded, next frame decodes |

### `HUB-03` — the reset path

| Input | Phase 1 | Phase 3 |
|---|---|---|
| 1 arbitrary byte | **fleet reset** | ignored |
| 100,000 random bytes | **~100,000 resets** | **0** |
| 5,000 realistic JSON lines | **5,000 resets** | **0** |
| A valid framed command | n/a | fires exactly once |
| Corrupted checksum | n/a | rejected and counted |

### `NODE-03` — cadence

| | Phase 1 | Phase 3 |
|---|---|---|
| Gate period | `1000/32` = **31 ms** (integer), first open at 32 ms | **31250 µs** exact |
| Sends per simulated second | ≤ 31.25 | **32** |
| 10 s under jitter | drifts (re-anchored each send) | **320–321, 0 resyncs** |
| After a 5 s stall | n/a | **1 send**, not a burst |
| Across the 32-bit µs wrap | untested | **65 sends, none lost** |
| 17 pods after a fleet reset | in lockstep | spread ~1838 µs apart |

### `BVH-01` — export timing

| | Frame Time | Label |
|---|---|---|
| Phase 1 / Phase 2 as shipped | 0.03333 (asserted) | none |
| Phase 3, 600 frames over 18.75 s | **0.03125 (measured)** | `measured` |
| Phase 3, no timing recorded | 0.03333 | `ASSUMED_1_30_UNTRUSTWORTHY` |

Quantified: asserting 1/30 over 32 Hz data stretches the timeline **6.67%**.

### `HUB-05` — integrity counters

| Event | Phase 1 | Phase 3 |
|---|---|---|
| 5-packet hole | invisible | 1 gap, 5 lost |
| Duplicate | invisible | 1 duplicate |
| Reordering | invisible | 1 reordered |
| Counter wrap 65535→0 | invisible | **0 lost** |
| Pod reboot | invisible | **1 resync, 0 lost** |

The reboot case is the important one: without it a single pod restart would have
been recorded as ~65,000 lost packets and poisoned every loss figure in the
session.

## 7. Instrumentation overhead

**Not measured.** The master prompt's Rule 2 requires the instrument's own cost be
quantified before its results are trusted. Phase 2 recorded this as unsatisfied
(`P2-B1-02`) and it remains unsatisfied: the pod accumulates `mesq_instrCostUs`
and prints it, but nothing has ever read that number off a device.

The always-on hub counters are new and also unmeasured. The arithmetic: 2 framed
records/s ≈ 500 B/s against a stream already carrying ~8.7 KB/s of pose data
(17 × 32 × 16), so ~5% more bytes. That is `[inference]`, not a measurement.

## 8. Fault injection performed

All host-side:

| Fault | Where |
|---|---|
| Unterminated JSON line, `0x0A`-free payloads | `parser_tests.js` T4 |
| Binary frame spliced mid-JSON | T5, `replay_harness.js` T7/T10 |
| Oversized JSON line | T6 |
| Mid-line stall | T7 |
| False sync, invalid bone id | T8 |
| Sync bytes inside a valid payload | T9 |
| Truncated / oversized / malformed / random | T11 |
| Seeded random chunking, 200 trials | T12 |
| 25 KB unparseable junk | T13 |
| Disconnect in all 5 parser states | T14 |
| 100,000 random bytes at the hub command parser | `firmware_tests.cpp` |
| Corrupted checksum, over-long length, unknown command | `firmware_tests.cpp` |
| Two-thread write contention with forced overflow | `firmware_tests.cpp` |
| Degenerate export timing (4 shapes) | `export_tests.js` E4 |

**Not performed — needs hardware:** IMU absent, DMP init failure, cold boots, USB
unplug mid-frame, pod reboot mid-capture, radio outage, duplicate ID insertion,
browser reload against a live stream.

## 9. Diagnosis outcomes

**Confirmed (16):** `NODE-01` `NODE-02` `NODE-03` `NODE-04` `NODE-06` `SENS-03`
`NET-03` `HUB-02` `HUB-03` `HUB-04` `HUB-05` `HUB-07` `WEB-02` `SYNC-03` `SYNC-08`
`PROTO-01` — plus three new: **P3-01** (hub instrumentation never compiled),
**P3-02** (I12 guard truthiness), **P3-03** (parser duplicated in the fixture).

**Confirmed in code, effect unquantified (3):** `SENS-02`, `PWR-01`, `PWR-02` —
the defect exists; whether it causes a field symptom is unmeasured.

**Inconclusive (1):** `SENS-01`. The ~55 Hz figure applies to the stock SparkFun
configuration. Whether `Example10_DMP_FastMultipleSensors` works with the pinned
library is `[unverified]`. **No register was invented.**

**Falsified (0).** No Phase 1 or Phase 2 finding in A1 scope was falsified this
phase. Phase 2 falsified `PHN-03` and weakened `WEB-02`'s severity; both stand.

**Blocked (all hardware items).** `A1_07`.

## 10. Independent review

**`SELF_REVIEWED`.** No second agent was used. A separate adversarial pass covered
concurrency and lock scope, codec and compatibility, wrap arithmetic, error
recovery, radio assumptions, parser state transitions and memory bounds,
quaternion conventions, and unsupported success claims.

Three findings from that pass were fixed and are worth recording because they were
found *by the tests*, not by reading:

1. **`MesqSampleChannel::publish()` had its drop condition inverted.** Caught by
   the `published = consumed + dropped` ledger assertion, which is exactly why
   that assertion exists.
2. **The JS encoder used `Math.round` where the firmware's C cast truncates** — a
   1-LSB divergence that would have made JS-generated golden vectors wrong as
   firmware references. Caught by the cross-language golden vector.
3. **The parser's stall sentinel used `0`**, which is a legal timestamp, so the
   stall timeout never fired. Caught by the stall test.

**Not reviewed independently, and it matters:** the ESP32-specific assumptions —
`portENTER_CRITICAL` cost, task priority interaction with the WiFi stack, USB CDC
throughput, and whether `Serial.write()` from a dedicated task behaves as assumed.

## 11. Residual risks

1. **Neither sketch has been compiled for the target.** Everything firmware-side
   is type-checked only.
2. **Nothing has run on hardware.** All firmware resolutions are `CODE_FIXED`, not
   `VERIFIED`.
3. **Instrumentation cost is still unmeasured** (Rule 2 unsatisfied).
4. **The USB queue's depth is a calculation, not a measurement.** If real CDC
   throughput is lower than assumed, `dropped` will be non-zero — visible, at
   least, which it was not before.
5. **The `NET-03` phase offset is arithmetically tested, RF-untested.**
6. **The ~2.4% offered-load increase from the deadline fix is unmeasured** against
   a real 17-pod airtime budget.
7. **`SENS-01` is unresolved.** If 60 Hz is a hard requirement, nothing here
   reaches it.
8. **Heading is unchanged and unfixable in this architecture.** Six-axis fusion has
   no absolute yaw.
9. **`SELF_REVIEWED` only.**
10. **Held frames are detected but not yet propagated into the BVH export.**

## 12. What may and may not be claimed

**May be claimed:**
- Specific code defects existed, with the mechanism identified and a reproduction.
- Those defects are fixed in source, with host tests that fail if reintroduced.
- The parser, codec, command framer, queue, deadline and quaternion policy behave
  as specified **under host test**.
- The JS and C encoders are byte-identical.

**May NOT be claimed:**
- That the firmware builds, flashes or runs.
- Any rate, latency, loss, range, power or accuracy figure.
- That symptoms S1–S6 are resolved. Fixing a *mechanism* is not the same as
  proving it *caused* the field symptom, and no field measurement exists.
- Anything about the fleet. Nothing was tested above one simulated node.
