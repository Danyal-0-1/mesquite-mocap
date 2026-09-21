# A1_05 — Retype These (for Daynal)

You asked to be told which code you should type out yourself rather than read.
This is that list, in order. Each item gives you the **baseline** (what was
there), the **final** (what is there now), the **invariant** the change
introduces, and the **test that tells the two apart** — so you can verify your own
retyping rather than trusting it.

**How to use this.** Open the file, delete the section, type it back from the
excerpt here, then run the stated command. If it passes, you have it. Do not
copy-paste; the point is the typing.

Everything here is from the frozen revision `3718fa6`.

---

## Order

| # | What | File | Why this order |
|---|---|---|---|
| 1 | Quaternion reconstruction | `mesq_pod_core.h` | pure maths, no concurrency, no hardware |
| 2 | Packet serialisation | `mesq_packet.h` | bytes, endianness, casts — still no concurrency |
| 3 | The transmit deadline | `mesq_pod_core.h` | integer time, wrap arithmetic |
| 4 | The coherent sample channel | `mesq_pod_core.h` | **the hard one** — two cores |
| 5 | The parser's JSON branch | `js/mesq_parser.js` | state machines and bounded recovery |

Do them in this order. 4 is much easier once 1–3 are in your fingers.

---

## 1. Quaternion reconstruction — `SENS-03`

**File:** `Device code/Pod_Watch_Binary/mesq_pod_core.h`, `mesqReconstructQuat()`

### Baseline (what was there)
`Pod_Watch_Binary.ino`, inside the Quat6 branch:

```c
double q1 = ((double)data.Quat6.Data.Q1) / 1073741824.0;
double q2 = ((double)data.Quat6.Data.Q2) / 1073741824.0;
double q3 = ((double)data.Quat6.Data.Q3) / 1073741824.0;

double q0 = sqrt(1.0 - ((q1 * q1) + (q2 * q2) + (q3 * q3)));   // <-- unguarded
```

**The defect.** When the radicand is negative, `sqrt` returns `NaN`. Then
`q_to_i16(NaN)` returns `0` — and it does that for all four components, because
`w` is `NaN` and the others get multiplied into a `NaN` comparison chain. The pod
transmits `(0, 0, 0, 0)`. That is not a rotation. It has no inverse. Every
composition downstream is silently wrong.

### Final
```c
static const double MESQ_RADICAND_EPS = 1e-6;

enum MesqQuatStatus {
    MESQ_Q_OK       = 0,
    MESQ_Q_REPAIRED = 1,
    MESQ_Q_REJECTED = 2
};

static inline MesqQuatStatus mesqReconstructQuat(double x, double y, double z,
                                                 double *out_w, double *out_x,
                                                 double *out_y, double *out_z) {
    if (!isfinite(x) || !isfinite(y) || !isfinite(z)) return MESQ_Q_REJECTED;

    double sumsq = x * x + y * y + z * z;
    double rad   = 1.0 - sumsq;

    MesqQuatStatus st = MESQ_Q_OK;
    if (rad < 0.0) {
        if (rad < -MESQ_RADICAND_EPS) return MESQ_Q_REJECTED;  // a real bad sample
        rad = 0.0;                                             // roundoff
        st  = MESQ_Q_REPAIRED;
    }

    double w = sqrt(rad);

    double n = sqrt(w * w + sumsq);
    if (!isfinite(n) || n < 1e-9) return MESQ_Q_REJECTED;   // degenerate, incl. all-zero

    *out_w = w / n; *out_x = x / n; *out_y = y / n; *out_z = z / n;
    return st;
}
```

### The invariant
> Every value this function *accepts* is a finite unit quaternion. It never
> returns the zero quaternion. A rejected sample changes nothing, so the caller
> keeps the previous pose.

### Predict before you run
Work these out on paper first:

1. `mesqReconstructQuat(0, 0, 0, …)` → what is `w`?
2. `mesqReconstructQuat(1, 1, 1, …)` → what is the radicand? What does it return?
3. Where does `5.6e-9` come from, and why is `1e-6` above it but far below −1?

### Verify
```bash
g++ -std=c++17 -O2 -pthread -DMESQ_HOST_TEST \
    -I"Device code/Pod_Watch_Binary" -I"Device code/Dongle_Binary" \
    tools/firmware_tests.cpp -o /tmp/t && /tmp/t 2>&1 | sed -n '/SENS-03/,/NODE-03/p'
```
Expect: 200,000 random inputs with **every accepted result a unit quaternion**,
50,000 roundoff-shell inputs all repaired and none rejected.

### Variation to try
Set `MESQ_RADICAND_EPS` to `1e-12` and rerun. Which assertion fails, and why does
that tell you the threshold is doing real work?

### Mastery questions
- Why is renormalising *after* clamping necessary rather than cosmetic?
- Why is `n < 1e-9` a rejection and not a repair?
- Why must a rejected sample leave the outputs untouched instead of zeroing them?

---

## 2. Packet serialisation — `PROTO-01`

**File:** `Device code/Pod_Watch_Binary/mesq_packet.h`, `mesq_q_to_i16()` and
`mesq_pack()`

### Baseline
```c
myData.qx = q_to_i16(quat.x);   // ... then memcpy the whole struct onto the wire
esp_now_send(broadcastAddress, (uint8_t *)&myData, sizeof(myData));
```
Field order was whatever the compiler produced for the struct. It happened to be
right, but nothing checked it and nothing could have caught it changing.

### Final
```c
static inline int16_t mesq_q_to_i16(float v) {
    if (isnan(v)) return 0;
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 32767.0f);      /* C cast truncates toward zero */
}

static inline void mesq_pack(uint8_t *out, uint8_t id, uint8_t batt,
                             float qw, float qx, float qy, float qz,
                             uint16_t count, uint16_t ms_lo) {
    int16_t x = mesq_q_to_i16(qx), y = mesq_q_to_i16(qy);
    int16_t z = mesq_q_to_i16(qz), w = mesq_q_to_i16(qw);
    out[0]  = MESQ_SYNC0;
    out[1]  = MESQ_SYNC1;
    out[2]  = id;
    out[3]  = batt > 100 ? 100 : batt;
    out[4]  = (uint8_t)( (uint16_t)x       & 0xFF);
    out[5]  = (uint8_t)(((uint16_t)x >> 8) & 0xFF);
    /* ... y at 6,7  z at 8,9  w at 10,11 ... */
    out[12] = (uint8_t)( count       & 0xFF);
    out[13] = (uint8_t)((count >> 8) & 0xFF);
    out[14] = (uint8_t)( ms_lo       & 0xFF);
    out[15] = (uint8_t)((ms_lo >> 8) & 0xFF);
}
```

### The invariant
> The byte layout is a property of this function, not of the compiler. The same
> inputs produce `aa550257ff3f01c0ff1fff5f3412cdab` in both C and JavaScript.

### Predict
1. `mesq_q_to_i16(0.5f)` → which integer? Which two bytes, in which order?
2. `mesq_q_to_i16(-1.0f)` → why `-32767` and not `-32768`?
3. Why does the cast **truncate** rather than round, and why did that matter
   enough to change the JavaScript side to match?

### Verify
```bash
/tmp/t 2>&1 | sed -n '/PROTO-01/,/HUB-RESET/p'
node tools/parser_tests.js 2>&1 | sed -n '/G1/,/G2/p'
```
Both must print the same golden hex.

### Variation
Change the JS `quantise()` back to `Math.round`. Which assertion fails, and by how
many LSBs?

---

## 3. The transmit deadline — `NODE-03` / `NET-03`

**File:** `Device code/Pod_Watch_Binary/mesq_pod_core.h`, `mesqDeadlineDue()`

### Baseline
```c
static uint32_t prev_ms = millis();
if (millis() > (prev_ms + (1000 / fps))) {      // fps = 32
    ...send...
    prev_ms = millis();                          // re-anchored to the SEND time
}
```

**Three defects in two lines.** Find all three before reading on:
1. `1000 / 32` is integer division → `31`, not `31.25`.
2. `>` rather than `>=` means the branch first passes at **32** ms.
3. `prev_ms = millis()` re-anchors to when the send *finished*, so lateness
   accumulates permanently instead of cancelling.

### Final
```c
#define MESQ_MAX_CATCHUP 4

struct MesqDeadline {
    uint32_t period_us;
    uint32_t next_us;
    uint32_t late_us_max;
    uint32_t resyncs;
};

static inline void mesqDeadlineInit(MesqDeadline *d, uint32_t period_us,
                                    uint32_t now_us, uint32_t node_id,
                                    uint32_t node_count) {
    d->period_us   = period_us;
    d->late_us_max = 0;
    d->resyncs     = 0;
    uint32_t phase = (node_count > 1) ? (uint32_t)(((uint64_t)node_id * period_us) / node_count) : 0;
    d->next_us     = now_us + phase;
}

static inline bool mesqDeadlineDue(MesqDeadline *d, uint32_t now_us) {
    int32_t delta = (int32_t)(now_us - d->next_us);
    if (delta < 0) return false;

    if ((uint32_t)delta > d->late_us_max) d->late_us_max = (uint32_t)delta;

    if ((uint32_t)delta > d->period_us * MESQ_MAX_CATCHUP) {
        d->next_us = now_us + d->period_us;
        d->resyncs++;
    } else {
        d->next_us += d->period_us;     /* fixed grid -- no accumulated drift */
    }
    return true;
}
```

### The invariant
> Sends land on a fixed grid. Lateness in one tick is absorbed by the next, never
> carried forward. After a long stall exactly one send fires, not a burst.

### The line worth staring at
```c
int32_t delta = (int32_t)(now_us - d->next_us);
```
`now_us` and `next_us` are **unsigned**. Their difference wraps correctly by
definition, and casting to signed then gives a correct sign even across the 32-bit
microsecond rollover at ~71.6 minutes — which a capture session will cross. Write
it as `now < next` instead and it breaks exactly once, about 72 minutes in, which
is the worst kind of bug to find in the field.

### Predict
1. `period_us = 31250`, `next_us = 0`. How many times does `mesqDeadlineDue` fire
   as `now_us` sweeps 0 → 1,000,000?
2. `node_id = 5`, `node_count = 17`, `period_us = 31250`. What is the phase offset?
3. The task is blocked for 5 s. How many sends fire on the next call?

### Verify
```bash
/tmp/t 2>&1 | sed -n '/NODE-03/,/PROTO-01/p'
```

### Variation
Change `d->next_us += d->period_us;` to `d->next_us = now_us + d->period_us;` —
the Phase 1 behaviour. Which assertion fails? That failure *is* the accumulated
drift.

---

## 4. The coherent sample channel — `NODE-01` (the hard one)

**File:** `Device code/Pod_Watch_Binary/mesq_pod_core.h`, `MesqSampleChannel`

Do 1–3 first.

### Baseline
```c
struct Quat { float x, y, z, w; } quat;     // file scope

// core 1, TaskReadIMU:
quat.w = q0; quat.x = q1; quat.y = q2; quat.z = q3;

// core 0, TaskWifi:
myData.qx = q_to_i16(quat.x);
myData.qy = q_to_i16(quat.y);
myData.qz = q_to_i16(quat.z);
myData.qw = q_to_i16(quat.w);
```

**The defect.** Two cores, four independent stores, four independent loads,
nothing between them. A read landing mid-write returns half of one orientation and
half of another. Its norm is not 1, so it is not a rotation.

**`volatile` would not have helped.** It stops the compiler caching the value in a
register. It does not make four stores into one, and it says nothing about
ordering between cores. Internalise this one.

### Final
```c
struct MesqSample {
    float    w, x, y, z;
    uint32_t sample_ms;
    uint32_t sample_seq;
    uint8_t  valid;
};

class MesqSampleChannel {
public:
    MesqSampleChannel() : seq_(0), dropped_(0), published_(0), consumed_(0) {
        mesqMuxInit(&mux_);
        memset(&slot_, 0, sizeof(slot_));
        slot_.w = 1.0f;          // identity until the first real sample
    }

    void publish(float w, float x, float y, float z, uint32_t ms) {
        mesqEnter(&mux_);
        if (fresh_) dropped_++;           // overwriting one the radio never got
        slot_.w = w; slot_.x = x; slot_.y = y; slot_.z = z;
        slot_.sample_ms  = ms;
        slot_.sample_seq = ++seq_;
        slot_.valid      = 1;
        fresh_ = true;
        published_++;
        mesqExit(&mux_);
    }

    MesqSample take(bool *is_fresh) {
        MesqSample out;
        mesqEnter(&mux_);
        out = slot_;
        bool f = fresh_;
        fresh_ = false;
        if (f) consumed_++;
        mesqExit(&mux_);
        if (is_fresh) *is_fresh = f;
        return out;
    }

private:
    MesqMux    mux_;
    MesqSample slot_;
    uint32_t   seq_, dropped_, published_, consumed_;
    bool       fresh_ = false;
};
```

### The invariants
> 1. Every field a consumer sees came from **one** `publish()` call.
> 2. `published = consumed + dropped`, always.
> 3. A consumer that gets nothing new is **told** so, and can mark the packet held.
> 4. Nothing is called inside the critical section.

### The subtle line
```c
if (fresh_) dropped_++;
```
My first draft had this as `if (!fresh_)`, which is backwards, and **the ledger
assertion in the test caught it** — 90,168 + 90,168 ≠ 400,000. That is the
argument for writing the accounting identity as a test rather than trusting the
counter. Think about why the condition is what it is before you type it.

### Predict
1. `publish(); publish(); take();` → what are `published`, `consumed`, `dropped`?
2. `take()` on a brand-new channel → what quaternion, and what does `is_fresh` say?
3. Why does the constructor set `slot_.w = 1.0f` rather than leaving all zeros?

### Verify
```bash
/tmp/t 2>&1 | sed -n '/NODE-01/,/SENS-03/p'
```
Expect: **0 incoherent** across 400,000 publishes on two real threads, and the
ledger balanced. The baseline block in the same output shows the torn reads for
comparison.

### Variation
Comment out `mesqEnter`/`mesqExit` in **both** methods and rerun. The incoherent
count should become non-zero. That is the race, reproduced on demand.

### Mastery questions
- Why length-one-with-overwrite rather than a FreeRTOS queue of depth 8?
- Why is a unit-norm check *necessary but not sufficient* to prove coherence, and
  what does `sample_seq` add that the norm cannot?
- Why was a seqlock rejected?
- What would break if `esp_now_send()` were called inside the critical section?

---

## 5. The parser's JSON branch — `WEB-02`

**File:** `js/mesq_parser.js`, inside `feed()`

### Baseline
```js
if (b === SYNC0 && _jsonLine.length === 0) { /* binary */ }
if (b === 0x7B || _jsonLine.length > 0) {
  _jsonLine += String.fromCharCode(b);
  if (b === 0x0A) { /* parse and reset */ }
  i += 1; continue;
}
```
Once a `{` opened a line, every byte — including whole binary frames — went into a
string until a `0x0A` arrived. If none ever did, the parser was dead until reload.

### Final
```js
if (b === LBRACE) {
  var limit = Math.min(buf.length, i + MAX_JSON_LINE);
  var end = -1, interleaveAt = -1;
  for (var k = i; k < limit; k++) {
    if (buf[k] === SYNC0 && k + 1 < buf.length && buf[k + 1] === SYNC1) {
      interleaveAt = k;
      break;
    }
    if (buf[k] === NEWLINE) { end = k; break; }
  }

  if (interleaveAt >= 0) {
    stats.jsonInterleave++;
    onDrop('json_interleave', interleaveAt - i);
    jsonPendingSince = -1;
    i = interleaveAt;                  // hand control back to the binary branch
    continue;
  }
  if (end >= 0) {
    jsonPendingSince = -1;
    commitJsonLine(bytesToString(buf, i, end).trim());
    i = end + 1;
    continue;
  }
  if (buf.length - i >= MAX_JSON_LINE) {
    stats.jsonOverflow++;
    onDrop('json_overflow', MAX_JSON_LINE);
    jsonPendingSince = -1;
    i += 1;                            // step past '{' and rescan
    continue;
  }
  if (jsonPendingSince < 0) jsonPendingSince = nowFn();
  else if (nowFn() - jsonPendingSince > JSON_STALL_MS) {
    stats.jsonStall++;
    onDrop('json_stall', buf.length - i);
    jsonPendingSince = -1;
    i += 1;
    continue;
  }
  break;
}
```

### The invariants
> 1. Every branch either consumes bytes or breaks to wait — it can never spin.
> 2. The buffer is bounded on three axes.
> 3. Each abandon path advances past the offending byte, so no state can latch.

### The idea worth taking away
Valid JSON here is **7-bit ASCII**, so `0xAA 0x55` **cannot** appear inside a
well-formed line. Finding it there is *proof* of a hub-side interleave, not a
guess. That is what makes this a correctness fix rather than a heuristic. Look for
an invariant in your data that makes the ambiguous case impossible — it is a
technique that generalises.

### The sentinel
```js
var jsonPendingSince = -1;   // -1 = no line pending; 0 is a legal timestamp
```
My first draft used `0` as "nothing pending". Node's fixture clock starts at 0, so
the stall timeout never fired, and the stall test caught it. The same bug class —
a truthiness or zero-sentinel test on a numeric value — also appeared in the BVH
exporter's guard and in `PHN-01`'s position guard. **Three times in one codebase.**
When a value can legitimately be zero, never use zero to mean "absent".

### Predict
1. Feed `'{"bone":"Hips"'` then a 16-byte binary frame. Which branch fires? How
   many pod frames decode?
2. Feed `'{'` followed by 600 bytes of `'a'`. Which counter increments?
3. Why `slice` rather than `subarray` for the tail trim?

### Verify
```bash
node tools/parser_tests.js 2>&1 | sed -n '/T4/,/T8/p'
node tools/replay_harness.js 2>&1 | sed -n '/T10/,$p'
```
The second shows the Phase 2 baseline and the Phase 3 module on the same fault
bytes.

### Variation
Set `MAX_JSON_LINE` to `1e9`. Which assertion fails, and what does the pending
buffer do?

---

## Desktop-first, hardware later

Everything above runs on your laptop. Items 1–4 are host C++; item 5 is Node. None
needs a pod.

**When you do get to hardware:** use a disposable firmware branch and **one** pod
before any fleet deployment. `system_assessment_2/ROLLOUT.md` has the staged
procedure, and `A1_07_OPEN_ITEMS_AND_HARDWARE_RUNBOOK.md` has the measurements
worth taking while you are there.

## Answer key

<details>
<summary>Expand after you have written your own answers</summary>

**1.1** `w = 1`. Radicand is `1 − 0 = 1`.
**1.2** Radicand `1 − 3 = −2`; returns `MESQ_Q_REJECTED`.
**1.3** Each Q30 component carries ≤ 2⁻³⁰ error; three components, derivative of
`c²` is `2c`, `|c| ≤ 1` → `3 × 2 × 1 × 2⁻³⁰ ≈ 5.6e-9`. `1e-6` is ~180× above it
and ~10⁶× below the `−1`-order radicand of a genuinely bad sample.

**2.1** `0.5 × 32767 = 16383.5` → truncates to `16383` = `0x3FFF` → bytes `ff 3f`.
**2.2** `-1.0 × 32767 = -32767` exactly. `int16` reaches `-32768`, but saturating
at `-32767` keeps the range symmetric so `+1` and `−1` are mirror images.
**2.3** The C cast truncates; matching it in JS makes the golden vectors valid as
firmware references. `Math.round` differed by 1 LSB on negative half-values.

**3.1** 32. Deadlines at 0, 31250, …, 968750.
**3.2** `5 × 31250 / 17 = 9191 µs`.
**3.3** One. `delta` exceeds `4 × period`, so it resyncs to `now + period` and
increments `resyncs`.

**4.1** `published = 2`, `consumed = 1`, `dropped = 1`. The second publish
overwrote an unconsumed sample.
**4.2** Identity `(w=1, x=y=z=0)`, and `is_fresh` is `false`.
**4.3** So a consumer taking before any sample arrives gets a valid rotation. All
zeros would be the zero quaternion — the exact thing `SENS-03` exists to prevent.

**5.1** The interleave branch. All frames decode; the text line is discarded and
counted.
**5.2** `jsonOverflow`, once the pending bytes reach `MAX_JSON_LINE`.
**5.3** `subarray` keeps a view onto the original `ArrayBuffer`, so the whole
allocation stays alive. `slice` copies and lets the old buffer be collected.

</details>
