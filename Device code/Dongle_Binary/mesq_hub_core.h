/* =========================================================================
   MESQUITE HUB CORE  --  Device code/Dongle_Binary/mesq_hub_core.h

   The three pieces of hub logic worth testing off-device: the host -> hub
   command framer, the single-owner USB write queue, and the per-node
   integrity counters.

   Compiles on the ESP32-S3 and, with -DMESQ_HOST_TEST, on a desktop so
   tools/firmware_tests.cpp can run the queue under two real producer threads.

   Addresses Phase 1 HUB-02, HUB-03, HUB-05, SYNC-01/SYNC-03.
   LEARNING: A1_05_LEARNING_GUIDE.md ch. 8 (hub).
   ========================================================================= */
#ifndef MESQ_HUB_CORE_H
#define MESQ_HUB_CORE_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#ifdef MESQ_HOST_TEST
  #include <atomic>
  struct MesqHubMux { std::atomic_flag f = ATOMIC_FLAG_INIT; };
  static inline void mesqHubEnter(MesqHubMux *m) { while (m->f.test_and_set(std::memory_order_acquire)) {} }
  static inline void mesqHubExit (MesqHubMux *m) { m->f.clear(std::memory_order_release); }
  // std::atomic_flag is not assignable, so initialisation is a call, not an
  // assignment. Same shape as portMUX so the ESP32 path stays idiomatic.
  static inline void mesqHubMuxInit(MesqHubMux *m) { m->f.clear(std::memory_order_release); }
#else
  // MESQ_STUB_BUILD lets tools/check_sketches.sh type-check THIS branch --
  // the real ESP32 path -- without the FreeRTOS tree on the host.
  #ifndef MESQ_STUB_BUILD
    #include "freertos/FreeRTOS.h"
  #endif
  typedef portMUX_TYPE MesqHubMux;
  static inline void mesqHubEnter(MesqHubMux *m) { portENTER_CRITICAL(m); }
  static inline void mesqHubExit (MesqHubMux *m) { portEXIT_CRITICAL(m); }
  static inline void mesqHubMuxInit(MesqHubMux *m) { portMUX_TYPE t = portMUX_INITIALIZER_UNLOCKED; *m = t; }
#endif

#ifndef MESQ_NUM_BONES
#define MESQ_NUM_BONES 17
#endif

/* =========================================================================
   1. HOST -> HUB COMMAND FRAMER  -- HUB-03
   =========================================================================
   Phase 1:
        void loop() {
          if (Serial.available() > 0) { Serial.readString(); sendReset(); }
        }

   ANY inbound byte rebooted all 17 pods. Not a command byte -- any byte. A
   terminal probe, a line of line noise, an `echo` into the wrong tty, the
   browser's own startup traffic: all of them dropped the suit mid-capture
   and required a power cycle to recover from the resulting confusion. This
   is one of the two latching failures Phase 1 attributed to symptoms S3/S4.

   PROTOCOL: a command is now explicit and self-checking.

        [0xAA][0x55][0xFC][cmd][len][payload 0..16][xor]

   0xFC is reserved: it is outside the 0..16 bone range, and distinct from
   0xFE (hub -> host status) and 0xFF (hub -> pod control), so no record kind
   can be mistaken for another. `xor` covers cmd, len and payload.

   The checksum is here to catch ACCIDENTAL corruption -- line noise, a
   half-written frame. It is NOT authentication and does not make the reset
   command safe against a hostile writer on the port; anyone who can open the
   port can send a valid frame. That is acceptable for a USB cable in a lab
   and is stated rather than implied.
   ========================================================================= */
#define MESQ_CMD_SYNC0        0xAA
#define MESQ_CMD_SYNC1        0x55
#define MESQ_CMD_MARKER       0xFC
#define MESQ_CMD_MAX_PAYLOAD  16

#define MESQ_CMD_RESET_FLEET  0x01
#define MESQ_CMD_PING         0x02

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t payload[MESQ_CMD_MAX_PAYLOAD];
} MesqCmd;

typedef struct {
    uint8_t  state;      /* 0 sync0, 1 sync1, 2 marker, 3 cmd, 4 len, 5 payload, 6 xor */
    uint8_t  cmd, len, idx, xorAcc;
    uint8_t  payload[MESQ_CMD_MAX_PAYLOAD];
    uint32_t accepted, badChecksum, badLen, resets;
} MesqCmdParser;

static inline void mesqCmdInit(MesqCmdParser *p) { memset(p, 0, sizeof(*p)); }

/* Feed one byte. Returns 1 exactly once, when a complete valid frame closes.
   RECOVERY: every rejection resets to state 0 and the offending byte is
   re-examined as a potential new sync, so a false start cannot wedge it. */
static inline int mesqCmdFeed(MesqCmdParser *p, uint8_t b, MesqCmd *out) {
    switch (p->state) {
    case 0: if (b == MESQ_CMD_SYNC0) p->state = 1; return 0;
    case 1:
        if (b == MESQ_CMD_SYNC1) p->state = 2;
        else p->state = (b == MESQ_CMD_SYNC0) ? 1 : 0;
        return 0;
    case 2:
        if (b == MESQ_CMD_MARKER) p->state = 3;
        else p->state = (b == MESQ_CMD_SYNC0) ? 1 : 0;
        return 0;
    case 3: p->cmd = b; p->xorAcc = b; p->state = 4; return 0;
    case 4:
        if (b > MESQ_CMD_MAX_PAYLOAD) {      /* refuse before allocating anything */
            p->badLen++;
            p->state = (b == MESQ_CMD_SYNC0) ? 1 : 0;
            return 0;
        }
        p->len = b; p->xorAcc ^= b; p->idx = 0;
        p->state = (b == 0) ? 6 : 5;
        return 0;
    case 5:
        p->payload[p->idx++] = b; p->xorAcc ^= b;
        if (p->idx >= p->len) p->state = 6;
        return 0;
    case 6:
        p->state = 0;
        if (b != p->xorAcc) { p->badChecksum++; return 0; }
        p->accepted++;
        if (out) { out->cmd = p->cmd; out->len = p->len; memcpy(out->payload, p->payload, p->len); }
        return 1;
    }
    p->state = 0;
    return 0;
}

/* Build a frame. Returns its length. Buffer must hold 5 + len + 1 bytes. */
static inline int mesqCmdBuild(uint8_t *out, uint8_t cmd, const uint8_t *payload, uint8_t len) {
    if (len > MESQ_CMD_MAX_PAYLOAD) len = MESQ_CMD_MAX_PAYLOAD;
    int n = 0;
    out[n++] = MESQ_CMD_SYNC0; out[n++] = MESQ_CMD_SYNC1; out[n++] = MESQ_CMD_MARKER;
    uint8_t x = cmd ^ len;
    out[n++] = cmd; out[n++] = len;
    for (uint8_t i = 0; i < len; i++) { out[n++] = payload[i]; x ^= payload[i]; }
    out[n++] = x;
    return n;
}

/* =========================================================================
   2. SINGLE-OWNER USB WRITE QUEUE  -- HUB-02
   =========================================================================
   Phase 1 had two independent task contexts writing one USB stream with no
   mutual exclusion:

       OnDataRecv()            -> Serial.write(16 raw bytes)   [WiFi task]
       handleWebSocketMessage()-> Serial.println(json line)     [AsyncTCP task]

   Phase 2 measured the collision (F2): one interleave destroys BOTH records.
   The pod frame is split by text and the JSON line is split by binary.

   The fix is ownership, not locking. A mutex would have to be held across
   Serial.write() inside the ESP-NOW receive callback, which is exactly the
   blocking-in-a-callback the framework forbids. Instead every producer copies
   its record into a bounded ring and ONE task drains it. Consequences:

     - the receive callback copies 16 bytes and returns immediately, which
       also satisfies the rule that borrowed RX data must be copied into
       owned storage before the callback returns;
     - a record is written whole or not at all;
     - backpressure becomes a COUNTER (`dropped`) instead of corruption.

   Sized for the fleet: 17 pods x 32 Hz = 544 records/s. 128 slots is ~235 ms
   of buffer, which covers a USB stall without letting latency grow unbounded.
   ========================================================================= */
#define MESQ_USB_REC_MAX    96     /* longest record: a phone JSON line */
#define MESQ_USB_QUEUE_LEN  128

typedef struct {
    uint8_t  data[MESQ_USB_QUEUE_LEN][MESQ_USB_REC_MAX];
    uint8_t  len[MESQ_USB_QUEUE_LEN];
    volatile uint16_t head, tail, count;
    uint32_t dropped, highWater, pushed, popped;
    MesqHubMux mux;
} MesqUsbQueue;

static inline void mesqUsbInit(MesqUsbQueue *q) {
    memset((void *)q, 0, sizeof(*q));
    mesqHubMuxInit(&q->mux);
}

/* Producer side. Safe from a callback: it copies and returns, never blocks on
   I/O. Returns 0 if the record was dropped (queue full or oversized). */
static inline int mesqUsbPush(MesqUsbQueue *q, const uint8_t *rec, uint8_t len) {
    if (len == 0 || len > MESQ_USB_REC_MAX) { q->dropped++; return 0; }
    int okPush = 0;
    mesqHubEnter(&q->mux);
    if (q->count < MESQ_USB_QUEUE_LEN) {
        memcpy(q->data[q->head], rec, len);
        q->len[q->head] = len;
        q->head = (uint16_t)((q->head + 1) % MESQ_USB_QUEUE_LEN);
        q->count++;
        q->pushed++;
        if (q->count > q->highWater) q->highWater = q->count;
        okPush = 1;
    } else {
        /* RECOVERY: drop the NEWEST rather than overwrite the oldest. The
           oldest is already half-written to USB conceptually; dropping the
           newest keeps the stream self-consistent and bounds latency. */
        q->dropped++;
    }
    mesqHubExit(&q->mux);
    return okPush;
}

/* Consumer side. Exactly one task may call this. Returns 0 when empty. */
static inline int mesqUsbPop(MesqUsbQueue *q, uint8_t *out, uint8_t *outLen) {
    int got = 0;
    mesqHubEnter(&q->mux);
    if (q->count > 0) {
        uint8_t n = q->len[q->tail];
        memcpy(out, q->data[q->tail], n);
        *outLen = n;
        q->tail = (uint16_t)((q->tail + 1) % MESQ_USB_QUEUE_LEN);
        q->count--;
        q->popped++;
        got = 1;
    }
    mesqHubExit(&q->mux);
    return got;
}

/* =========================================================================
   3. PER-NODE INTEGRITY COUNTERS  -- HUB-05, SYNC-01, SYNC-03
   =========================================================================
   Phase 1: "dropped packets are silent". `count` was on the wire and nobody
   ever compared consecutive values, so a node that lost half its packets and
   a node that was working perfectly looked identical to the operator.

   PROTOCOL: `count` is uint16 and wraps. All arithmetic below is done on
   uint16 differences so the wrap at 65535 -> 0 costs zero lost packets. The
   three ways a counter can move backwards are distinguished, because they
   mean completely different things:

     delta == 0                  duplicate (a radio retry that got through twice)
     delta small and negative    reordering
     delta large and negative    the pod REBOOTED and restarted its counter

   Phase 1 had no reboot case at all, so one pod restart would have been
   reported as ~65,000 lost packets and poisoned every loss statistic in the
   session. RESYNC_THRESHOLD draws the line: a forward jump larger than this
   is not credible as loss at 32 Hz (it is >30 s of silence) and is treated as
   a session restart.
   ========================================================================= */
#define MESQ_RESYNC_THRESHOLD 1000

typedef struct {
    uint32_t received, lost, gaps, duplicates, reordered, resyncs;
    uint32_t firstSeenMs, lastSeenMs;
    uint16_t lastCount;
    uint8_t  seen;
} MesqNodeStats;

static inline void mesqStatsInit(MesqNodeStats *s, int n) { memset(s, 0, sizeof(MesqNodeStats) * n); }

static inline void mesqStatsOnPacket(MesqNodeStats *s, uint16_t count, uint32_t nowMs) {
    s->received++;
    s->lastSeenMs = nowMs;
    if (!s->seen) { s->seen = 1; s->firstSeenMs = nowMs; s->lastCount = count; return; }

    uint16_t fwd = (uint16_t)(count - s->lastCount);   /* wrap-safe forward distance */

    if (fwd == 0) { s->duplicates++; return; }

    if (fwd == 1) { s->lastCount = count; return; }    /* the normal case */

    if (fwd < MESQ_RESYNC_THRESHOLD) {                 /* a credible hole */
        s->gaps++;
        s->lost += (uint32_t)(fwd - 1);
        s->lastCount = count;
        return;
    }

    /* fwd is huge. Either the pod rebooted, or these packets arrived out of
       order. A backward distance smaller than the threshold means reordering;
       anything else is a restart. */
    uint16_t back = (uint16_t)(s->lastCount - count);
    if (back < MESQ_RESYNC_THRESHOLD) { s->reordered++; return; }

    s->resyncs++;
    s->lastCount = count;
}

#endif /* MESQ_HUB_CORE_H */
