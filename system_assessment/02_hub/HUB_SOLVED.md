# Hub — Solved Issues

Phase 3 outcome for the findings in [`REPORT.md`](REPORT.md), plus the hub-side
interface work required to verify the node changes in
[`../01_node_firmware/`](../01_node_firmware/).

> **`CODE_FIXED` is not `VERIFIED`.** The hub sketch has **not** been built with
> `arduino-cli` and has never run. Everything below is a source change backed by
> host tests — several of them running the hub's own headers under real threads —
> not a claim about a device.

Release candidate `3718fa6`. `Dongle_Binary.ino` SHA-256
`a154af5b…`, `mesq_hub_core.h` `ef14443d…`.

---

## Summary

| ID | Phase 1 title | Diagnosis | Resolution |
|---|---|---|---|
| `HUB-01` | `max_connection` cannot gate the pods | `FALSIFIED` (Phase 1) | `NOT_APPLICABLE` |
| `HUB-02` | Two task contexts write one unframed serial stream | `CONFIRMED` | `CODE_FIXED` |
| `HUB-03` | **Any** inbound serial byte reboots the entire suit | `CONFIRMED` | `CODE_FIXED` |
| `HUB-04` | `ws.cleanupClients()` is never called | `CONFIRMED` | `CODE_FIXED` |
| `HUB-05` | Dropped packets are silent | `CONFIRMED` | `CODE_FIXED` |
| `HUB-06` | Two of three FreeRTOS tasks are empty | `CONFIRMED` | `ACCEPTED_RISK` |
| `HUB-07` | `esp_now_add_peer` return unchecked | `CONFIRMED` | `CODE_FIXED` |
| `HUB-08` | USB CDC is not the bottleneck | `FALSIFIED` (negative result) | `NOT_APPLICABLE` |
| **P3-01** | The `-DMESQ_INSTR=1` build had never compiled | `CONFIRMED` (new) | `CODE_FIXED` |

---

## `HUB-02` — two writers, one stream

**Was.** `OnDataRecv` called `Serial.write(incomingData, 16)` from the WiFi task.
`handleWebSocketMessage` called `Serial.println(json)` from the AsyncTCP task.
`onEvent` printed connect/disconnect text into the same live stream. No mutual
exclusion anywhere. Phase 2 measured the collision (F2): **one interleave destroys
both records** — the pod frame is split by text and the JSON line is split by
binary.

**Now.** Ownership, not locking.

- `mesq_hub_core.h:160-220` — `MesqUsbQueue`, a 128 × 96 B ring with counters.
- `Dongle_Binary.ino:424` — the ESP-NOW callback **copies 16 bytes and returns**.
- `Dongle_Binary.ino:~470` — the WebSocket handler enqueues its line whole.
- `Dongle_Binary.ino:547` — `usbWriterTask`, priority 2, **the only caller of
  `Serial.write()` for stream data**.
- `Dongle_Binary.ino:127` — `hubEmitStatus()`: hub speech becomes framed `0xFE`
  records instead of raw text.

**Why not a mutex.** It would have to be held across `Serial.write()` *inside the
ESP-NOW receive callback*, which is exactly the blocking-in-a-callback the
framework forbids.

**A second defect this removed.** The callback was passing the framework's
**borrowed** RX buffer straight into a blocking write. The framework may reuse
that buffer the moment the callback returns. Copying first is required, not
merely convenient.

**Wire compatibility.** The phone's JSON stays a bare newline-terminated line on
the wire. The browser's phone path is unchanged.

**Evidence.** `tools/firmware_tests.cpp test_hub02()`: two real producer threads
(20,000 pod frames + 5,000 JSON lines) against one drain, deliberately
over-driven so the drop path is exercised.

- **no record split or interleaved** — the drained bytes are walked and checked
- `accepted + dropped = offered` — nothing vanished unaccounted
- `pushed == popped` — everything queued was drained
- `highWater ≤ 128`

**Open.** Real USB CDC throughput is unknown. `A1_07` M-HUB02: if `dropped > 0`
with a real fleet, the ring is undersized and the number tells you by how much.

---

## `HUB-03` — reset on any byte

**Was.**
```c
void loop() {
    if (Serial.available() > 0) { Serial.readString(); sendReset(); }
    vTaskDelay(pdMS_TO_TICKS(100));
}
```
A terminal probe, line noise, an `echo` into the wrong tty, or the browser's own
startup traffic dropped the entire suit mid-capture. Phase 1 listed this as one of
the latching causes behind symptoms S3/S4.

**Now.** `loop()` only sleeps. Inbound bytes go to `hostCmdTask`
(`Dongle_Binary.ino:568`), which acts only on a complete frame:

```
[0xAA][0x55][0xFC][cmd][len][payload 0..16][xor over cmd,len,payload]
```

`0xFC` is outside the bone range 0..16 and distinct from `0xFE` (hub→host status)
and `0xFF` (hub→pod control), so no record kind can be mistaken for another. The
reset's reason and initiator count are emitted as a status record — Phase 1 had no
way to know *why* a fleet restarted.

**What the checksum does and does not do.** It catches accidental corruption:
line noise, a half-written frame. It is **not authentication** — anyone who can
open the port can send a valid frame. That is acceptable for a USB cable in a lab
and is stated rather than implied. A CRC would not change it.

**Evidence.** `test_hubcmd()`:

| Input | Phase 1 | Phase 3 |
|---|---|---|
| 26 bytes of text and noise | reset | **0 triggers** |
| One ordinary JSON line | reset | **0** |
| **100,000 random bytes** | ~100,000 resets | **0** |
| **5,000 realistic JSON lines** | 5,000 resets | **0** |
| A valid framed command | n/a | fires **exactly once** |
| The same, byte at a time | n/a | fires exactly once |
| Corrupted checksum | n/a | rejected and counted |
| Over-long declared length | n/a | refused at the length byte, recovers immediately |
| Unknown but well-formed command | n/a | surfaced, not guessed |

**Browser side.** `window.sWrite("reboot")` became `window.mesqRebootFleet()`,
which builds a real frame (`js/mesq_parser.js encodeCommand`). The JS and C
encoders are asserted **byte-identical**: `aa55fc010001`.

**Deployment order.** Against a hub still running Phase 1 firmware, the **first
byte** of the new frame triggers the old reset — same outcome. So the browser can
be deployed **before** the hubs are reflashed. A Phase 3 hub ignores the literal
text `"reboot"` entirely, so flash the hubs before relying on that text anywhere
else.

---

## `HUB-05` — silent packet loss

**Was.** `count` was on the wire and nobody ever compared consecutive values. A
node losing half its packets and a healthy node looked identical to the operator.

**Now.** `MesqNodeStats` per bone id (`mesq_hub_core.h:230-290`), updated in the
receive callback, **always on — not behind `MESQ_INSTR`**. Phase 2 had this behind
a build flag; a measurement nobody flashes is not a measurement.

Two framed status records per second carry per-node `received`/`lost`, queue depth
and high-water, drops, peer failures and command counters. The browser collects
them in `window._hubStatus`.

**The arithmetic is wrap-safe, and it distinguishes four cases Phase 1 conflated:**

| Event | Phase 1 | Phase 3 |
|---|---|---|
| 5-packet hole | invisible | 1 gap, 5 lost |
| Repeated counter | invisible | 1 duplicate |
| Older counter | invisible | 1 reordered |
| Counter wrap 65535 → 0 | invisible | **0 lost** |
| **Pod reboot** (counter restarts from a high value) | invisible | **1 resync, 0 lost** |

The reboot case is the important one. Without it a single pod restart would be
recorded as ~65,000 lost packets and would poison every loss figure in the
session. `MESQ_RESYNC_THRESHOLD` draws the line: a forward jump of more than 1,000
is >30 s of silence at 32 Hz and is not credible as loss.

**Cost.** 2 records/s ≈ 500 B/s against a stream already carrying ~8.7 KB/s of
pose data (17 × 32 × 16) — about 5% more bytes. That is `[inference]`, not a
measurement; `A1_07` M-HUB02 confirms it.

---

## `HUB-04` and `HUB-07`

**`HUB-04`.** `ws.cleanupClients()` is now called once per second in
`podTimeoutTask` (`Dongle_Binary.ino:282`). `AsyncWebSocket` retains disconnected
client objects until it is called, so a session where phones come and go leaked
until the heap ran out — presenting as the hub degrading over a long shoot rather
than failing outright. One line. **The leak was not reproduced off-device**, so
this is `CODE_FIXED` on the strength of the library's documented behaviour.

**`HUB-07`.** Phase 1 set `peerMacsInit[id] = true` **before** calling
`esp_now_add_peer()` and ignored the return value. If the add failed, the flag
claimed the peer was registered, nothing ever retried, and `sendReset()` silently
skipped that pod **forever**. The flag is now set only on `ESP_OK`, and failures
increment `g_peerAddFail`, which appears in the status line.

---

## `HUB-06` — the empty tasks

`espNowTask` and `webSocketTask` are still empty `vTaskDelay` loops. They were
**not** removed: deleting them means removing the task handles other code
references, which is a wider change than this phase warrants, and they cost one
tick each. Documented in place with a comment saying so. **`ACCEPTED_RISK`**, and
carried in `../01_node_firmware/A1_10_REFERENCED_BACKLOG.md`.

---

## P3-01 — the instrumentation build had never compiled

`mesq_emitStatus()` referenced `SYNC0` and `SYNC1` above their `#define`. Macros
are textual, so **every `-DMESQ_INSTR=1` build of this sketch failed outright**.
Phase 2's own report records that nothing was ever compiled (`P2-B0-02`), which is
exactly how it survived.

Fixed by moving the wire-format block above the instrumentation block
(`Dongle_Binary.ino:64-77`). Found by `tools/check_sketches.sh`, which now
type-checks the hub in both configurations.

This is the clearest single argument for the type-checker existing: an
instrumentation build that does not compile is worse than no instrumentation,
because the plan assumed it was available.

---

## Negative results, retained

- **`HUB-01`** — `max_connection` cannot gate the pods. They use ESP-NOW and never
  associate, so no AP limit applies. The four-node theory is **refuted**.
- **`HUB-08`** — serial bandwidth ~18% utilised. **Not a constraint.** Note this
  is arithmetic, not a measurement; `A1_07` M-HUB02 tests it for real.

---

## Verification

| Suite | Hub-relevant assertions | Result |
|---|---|---|
| `tools/check_sketches.sh` | hub, hub+`MESQ_INSTR` | **both type-check** (the second did not before) |
| `tools/firmware_tests.cpp` `test_hubcmd` | 12 | pass |
| `tools/firmware_tests.cpp` `test_hub02` | 4 | pass |
| `tools/firmware_tests.cpp` `test_hubstats` | 6 | pass |
| `tools/parser_tests.js` C1 (cross-language command frame) | 5 | pass |

Reproduce: `tools/run_all_tests.sh`.

## Open, hardware-dependent

| ID | What | Where |
|---|---|---|
| M-HUB02 | USB queue depth and drops under a real fleet | `../01_node_firmware/A1_07…md` |
| M-HUB03 | Noise vs a real command, on the wire | same |
| M-HUB05 | Integrity counters against a known pod power-cycle | same |
| B1 | **`arduino-cli` build of this sketch** — blocks all three | same |
