# A1_04 — 2.4 GHz Channel Architecture Study

**Conclusion up front: stay on one fixed channel. Resolution
`HARDWARE_VALIDATION_PENDING`.** A multi-channel deployment must not be selected
from modelling alone, and no fleet measurement exists. What follows is the
constraint analysis, the design, the comparison protocol, and what would change
the answer.

---

## 1. Constraints from the framework and the official documentation

Verified against Espressif's published API documentation. **Each must be
re-verified against the exact installed Arduino-ESP32 / IDF version before it is
relied on for a deployment decision** — that version is itself unresolved (`U2`,
still open because no pod has been booted).

Sources, accessed 2026-09-20:
- ESP-NOW API — <https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_now.html>
- Wi-Fi API — <https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_wifi.html>
- ESP-NOW example — <https://github.com/espressif/esp-idf/tree/master/examples/wifi/espnow>
- ESP32-S3 datasheet — <https://documentation.espressif.com/esp32-s3_datasheet_en.pdf>

1. **One radio, one channel, one instant.** An ESP32 Wi-Fi radio is tuned to a
   single channel at any moment. `[fact-doc]`
2. **Sender and intended receiver must share the current channel.** Off-channel
   APIs, where a version offers them, are *sequential* operations, not
   simultaneous reception. `[fact-doc]`
3. **A single fixed-channel hub cannot receive several channels at once.**
   Follows from 1 and 2. `[inference]`
4. **Channel hopping creates deaf intervals.** Continuous sensor traffic is lost
   during them unless a rigorously synchronised design compensates. `[inference]`
5. **Infrastructure association constrains the channel.** The hub runs
   `WIFI_AP_STA` with a softAP for the phone, so the AP's channel and the ESP-NOW
   channel are the same radio. `[fact-code]` `Dongle_Binary.ino` pins both to
   `ESPNOW_WIFI_CHANNEL`.
6. **Most numbered channels overlap.** At ~20/22 MHz occupancy in the US,
   **1, 6, 11** are the standard non-overlapping set. 1–11 are **not** eleven
   independent lanes. `[fact-doc]`
7. **More interfaces, peers, tasks or cores do not create more receivers.**
   `[fact-doc]`
8. **Scanning retunes the radio** and creates blind intervals. Survey with a
   separate receiver or between captures, then lock and log. `[fact-doc]`

**Consequence.** Any genuine multi-channel architecture needs **more physical
receivers**, one per channel. That is Option C. Everything else is either one
channel, or one channel at a time.

## 2. Current configuration

`[fact-code]` Both sketches `#define ESPNOW_WIFI_CHANNEL 1` and call
`esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE)`, `esp_wifi_set_ps(WIFI_PS_NONE)`,
`esp_wifi_set_max_tx_power(80)`. The hub's softAP is created on the same channel.
Pods unicast to one hardcoded hub MAC.

**Channel 1 is also where the hub's own softAP beacons** (`NET-06`). The hub is
therefore contributing to congestion on the channel it is trying to receive on.
That is a real concern and it is **unmeasured**.

**`U1` — the PHY rate is still unknown.** Nothing in either sketch calls
`esp_wifi_config_espnow_rate()` or equivalent, so the rate is whatever the
framework defaults to. Phase 1's airtime arithmetic (57% at 32 Hz, 106% at 60 Hz)
**assumes 1 Mbps**. If the default is higher, those numbers are far too
pessimistic. This single unknown dominates every capacity claim in this document
and cannot be resolved without a sniffer or a fleet reflash.

**No regulatory country is set in code.** For the Arizona deployment
`esp_wifi_set_country()` should be set explicitly and logged. Recorded in `A1_07`
as OPEN-RF-COUNTRY.

## 3. Options

### Option A — one fixed channel, one hub  ← **SELECTED**
Simplest for compatibility and synchronisation. One USB device, one clock domain
at the host, no merge logic, no per-group provisioning. This is the current
architecture.

### Option B — one automatically selected channel per session
Survey before capture, provision pods and hub, then stay fixed. Adds startup
complexity, a wrong-channel recovery path, and a reproducibility question (two
sessions on different channels are not directly comparable). Worth revisiting
**after** A is measured across 1/6/11.

### Option C — several independent fixed-channel gateways
The only credible way to receive on several channels simultaneously. Partition
17 pods across 2–3 S3 hubs, each permanently on one of 1/6/11, merged at the host
by gateway id + pod id + host receive timestamp.

Costs: more USB devices, a host-side merge with explicit clock alignment, more
provisioning to get wrong, and higher hardware cost. Each gateway keeps its **own
real MAC** — cloning MACs would be a correctness disaster.

### Option D — one hub that channel-hops
**High risk.** Deaf intervals are structural (constraint 4). For a continuous
low-latency stream this needs a dwell schedule, pod-side synchronisation,
resynchronisation after drift, and a bounded worst-case latency — all before a
prototype is worth building. **Recommended only as a negative control.**

### Option E — a different transport or radio
Out of scope without a measured gap. No new hardware should be bought on the
strength of this document.

## 4. Why A is selected today

1. **No measurement exists.** Options C and D can only be justified by a measured
   deficiency in A, and A has never been measured with 17 pods.
2. **`U1` is unresolved.** Every congestion argument for multi-channel rests on an
   airtime figure that assumes an unverified PHY rate.
3. **The known defects were not RF.** Phase 1 attributed the intermittency (S3/S4)
   to three *latching* failures — a pod hanging on init, a hub resetting on any
   byte, a parser latching on a truncated line. All three are now fixed in source.
   Until the system is re-measured with those gone, attributing anything to radio
   contention is unsupported.
4. **C multiplies operational failure modes** — per-group channel provisioning,
   per-gateway identity, host merge, clock alignment — in exchange for an unproven
   benefit.

**This is a decision to measure, not a decision that one channel is sufficient.**

## 5. The comparison protocol

Run in this order; each step is only worth running if the previous one left a gap.
Hold packet format, offered load, PHY, TX power, channel width, encryption, build
identity, distance, motion, hub placement, antenna orientation and battery state
fixed unless that factor is under test. Randomise trial order, repeat trials, and
rotate physical pods between groups.

| # | Experiment | Answers |
|---|---|---|
| 1 | **Baseline scaling** 1, 4, 8, 12, 15, 17 pods, one hub, ch 1 | where, if anywhere, delivery degrades |
| 2 | **Channel-only crossover** full fleet on 1, then 6, then 11, same hub, one at a time | site interference. **Does NOT prove simultaneous multi-channel capacity** |
| 3 | **PHY crossover** default vs explicitly supported rates | resolves `U1`, the dominant unknown |
| 4 | **Two-hub split** ~9/8 pods on two separated channels | does partitioning help at all |
| 5 | **Three-hub processing control** ~6/6/5 across three gateways, **all on the same channel** | isolates hub/USB/parser parallelism from RF |
| 6 | **Three-hub RF split** ~6/6/5 across three separated channels | improvement *beyond* step 5 is the evidence RF partitioning contributed |
| 7 | **Hopping negative control** one hub, logged dwell schedule | quantifies deaf intervals. Not a deployment candidate |

**Step 5 is the one usually skipped and it is the one that matters.** Without it,
any improvement from three gateways could be three USB pipes and three parsers
rather than three channels, and the conclusion would be wrong.

Run each step first with **clearly labelled synthetic traffic** at controlled load
to isolate radio capacity, then repeat with integrated fresh IMU samples.

## 6. Metrics per trial

Per node, not just fleet means, with distributions:

- distinct fresh samples/s (`ms_lo` changes, now that held frames are detectable)
- delivery, gaps, duplicates, reordering, resyncs (hub `MesqNodeStats`, always on)
- longest outage and time to recover
- fairness across 15–17 pods
- airtime / channel occupancy
- RSSI per node (hub `MESQ_INSTR` I2) — body shadowing, orientation, range
- USB queue depth, high-water and drops (`MesqUsbQueue`, always reported)
- browser parse rate, drops by reason, `_podFresh` held/fresh split
- current draw and capture duration
- startup and preflight complexity, operator error rate

**On latency.** Report **same-clock durations** only — sample-to-send age on the
pod, host inter-arrival jitter, gaps, outages. Cross-device p50/p95/p99 latency may
be reported **only after** a clock-alignment method has measured offset, drift and
uncertainty, and that uncertainty is small relative to the claim. `ms_lo` is a
freshness marker, not a timebase; 17 independently booted `millis()` clocks share
no epoch (`SYNC-01`).

## 7. Regulatory

Arizona, United States → FCC. Channels **1–11** only; **12–14 must not be used**.
Use **1, 6, 11** for any separated-channel comparison. Do not silently enable
HT40. Set and log `esp_wifi_set_country()` explicitly — it is currently unset.

Software acceptance of a TX power value is **not** proof of compliance.
`esp_wifi_set_max_tx_power(80)` is a conducted-power request; compliance is about
**EIRP** and depends on the certified module and antenna. Preserve the certified
module/antenna configuration. Re-evaluate the whole plan for any other country.

## 8. Deployment and preflight procedure (Option A)

1. Confirm pod and hub firmware hashes match the manifest from
   `tools/build_pods.sh`.
2. Confirm every pod's `MESQ_POD_ID` is unique — the build system enforces this,
   the hub's per-node counters confirm it at runtime.
3. Boot the hub, read the banner: channel, packet length, IDF version, tick rate.
4. Boot pods; confirm each reaches `RUNNING` (not `FAIL_IMU` / `FAIL_DMP` — a
   failure is now red on the watch).
5. Connect the browser; confirm 17 distinct bone ids in `window._podRx`.
6. Watch one 1 Hz hub status line: all `rx` counters climbing, `lost` near zero,
   queue `dropped` zero.
7. Survey the band with a **separate** receiver, never by scanning on the hub —
   scanning retunes the radio and creates blind intervals.
8. Record channel, country, TX power, PHY rate (once `U1` is resolved), build
   hashes and the RF environment in the session manifest.

## 9. What would change the conclusion

Option C becomes worth adopting **only if**, after the fixes in this phase are on
hardware:

- step 1 shows delivery degrading materially between 8 and 17 pods, **and**
- step 3 shows the PHY rate cannot be raised to relieve it, **and**
- step 2 shows the degradation is not site interference fixable by choosing a
  quieter channel, **and**
- step 6 beats step 5 by a margin that is both statistically and practically
  meaningful, **and**
- the added synchronisation, provisioning and operational complexity is acceptable
  to the operator.

If any one of those fails, one optimised shared channel is the better system.
