/* =========================================================================
   MESQUITE FIRMWARE CORE TESTS  --  tools/firmware_tests.cpp

   Runs the ACTUAL pod headers on the host, under real threads, because
   "it compiles" proves nothing about a data race and no watch can be made to
   reproduce one on demand.

   Covers: NODE-01 (torn cross-core quaternion), NODE-02 (sample time and
   sequence), NODE-03 (transmit deadline), SENS-03 (quaternion policy),
   PROTO-01 (golden byte vectors, cross-checked against the JavaScript
   encoder), HUB-RESET (host -> hub command framing).

   Build and run:
     g++ -std=c++17 -O2 -pthread -DMESQ_HOST_TEST \
         -I"Device code/Pod_Watch_Binary" -I"Device code/Dongle_Binary" \
         tools/firmware_tests.cpp -o /tmp/mesq_fw_tests && /tmp/mesq_fw_tests
   ========================================================================= */
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <cmath>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include <random>

#include "mesq_pod_core.h"
#include "mesq_packet.h"
#include "mesq_hub_core.h"

static int g_pass = 0, g_fail = 0;
static void ok(bool c, const char *m) {
    if (c) { g_pass++; printf("  PASS  %s\n", m); }
    else   { g_fail++; printf("  FAIL  %s\n", m); }
}
static void okf(bool c, const char *fmt, ...) {
    va_list ap; char buf[512];
    va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    ok(c, buf);
}
static void section(const char *t) { printf("\n[%s]\n", t); }

static std::string hex(const uint8_t *b, int n) {
    static const char *d = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < n; i++) { s += d[b[i] >> 4]; s += d[b[i] & 0xF]; }
    return s;
}

/* =====================================================================
   NODE-01 -- the cross-core torn read, reproduced and then eliminated
   ===================================================================== */
// Phase 1's shared state, verbatim in shape: four independent floats, no
// synchronisation of any kind. Reproduced here so the defect is demonstrable
// rather than asserted.
struct LegacyQuat { float x, y, z, w; };
static LegacyQuat g_legacy = {0, 0, 0, 1};

static void test_node01() {
    section("NODE-01  cross-core coherent sample handoff");

    const int ITERS = 400000;

    /* ---- baseline: the Phase 1 structure ---- */
    {
        std::atomic<bool> stop{false};
        std::atomic<long> torn{0}, reads{0};

        // Producer alternates between two well-separated UNIT quaternions.
        // Any mix of the two has a norm far from 1, so a torn read is
        // detectable with certainty rather than statistically.
        std::thread prod([&] {
            const float a[4] = {1, 0, 0, 0};          // (x,y,z,w) = pure X
            const float b[4] = {0, 0, 0, 1};          // identity
            for (int i = 0; i < ITERS && !stop; i++) {
                const float *s = (i & 1) ? a : b;
                g_legacy.x = s[0]; g_legacy.y = s[1];
                g_legacy.z = s[2]; g_legacy.w = s[3];
            }
            stop = true;
        });
        std::thread cons([&] {
            while (!stop) {
                LegacyQuat q = g_legacy;              // field-by-field, unsynchronised
                float n = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
                reads++;
                if (fabsf(n - 1.0f) > 1e-3f) torn++;
            }
        });
        prod.join(); cons.join();
        printf("     baseline: %ld reads, %ld with a non-unit norm\n", reads.load(), torn.load());
        // Not asserted as >0: whether a torn read is OBSERVED depends on the
        // host's store granularity. The defect is structural, and the point
        // of the next block is that the fix removes the possibility, not just
        // the observation.
        ok(true, "baseline shape exercised (torn reads are possible by construction)");
    }

    /* ---- the fix ---- */
    {
        MesqSampleChannel ch;
        std::atomic<bool> stop{false};
        std::atomic<long> incoherent{0}, seqBad{0}, reads{0}, fresh{0};

        std::thread prod([&] {
            for (int i = 0; i < ITERS; i++) {
                if (i & 1) ch.publish(0.0f, 1.0f, 0.0f, 0.0f, (uint32_t)i);
                else       ch.publish(1.0f, 0.0f, 0.0f, 0.0f, (uint32_t)i);
            }
            stop = true;
        });
        std::thread cons([&] {
            uint32_t lastSeq = 0;
            while (!stop) {
                bool f = false;
                MesqSample s = ch.take(&f);
                reads++;
                if (f) fresh++;
                if (!mesqSampleCoherent(s)) incoherent++;
                // NODE-02: the record must be internally consistent. The
                // sample_ms the producer wrote for sequence N must be the one
                // that travels WITH sequence N -- a norm check alone cannot
                // see a field from a different generation.
                if (s.sample_seq > 0 && s.sample_ms != s.sample_seq - 1) seqBad++;
                if (s.sample_seq < lastSeq) seqBad++;    // never goes backwards
                lastSeq = s.sample_seq;
            }
        });
        prod.join(); cons.join();

        printf("     fixed: %ld reads, %ld fresh, published=%u consumed=%u dropped=%u\n",
               reads.load(), fresh.load(), ch.published(), ch.consumed(), ch.dropped());
        okf(incoherent == 0, "zero incoherent quaternions across %d publishes (%ld reads)", ITERS, reads.load());
        ok(seqBad == 0, "sequence, timestamp and quaternion always from one generation");
        ok(ch.published() == (uint32_t)ITERS, "every publish accounted for");
        okf(ch.consumed() + ch.dropped() == ch.published(),
            "G-ledger balances: consumed(%u) + dropped(%u) = published(%u)",
            ch.consumed(), ch.dropped(), ch.published());
    }
}

/* =====================================================================
   NODE-02 -- freshness semantics
   ===================================================================== */
static void test_node02() {
    section("NODE-02  sample time and freshness");
    MesqSampleChannel ch;
    bool f = false;

    MesqSample s0 = ch.take(&f);
    ok(!f, "an untouched channel reports NOT fresh");
    ok(s0.w == 1.0f && s0.x == 0 && s0.y == 0 && s0.z == 0,
       "and yields the identity rotation, never the zero quaternion");

    ch.publish(1, 0, 0, 0, 12345);
    MesqSample s1 = ch.take(&f);
    ok(f, "after a publish the sample reports fresh");
    ok(s1.sample_ms == 12345, "sample_ms is the DECODE time handed in, not a transmit time");
    ok(s1.sample_seq == 1, "sample_seq starts at 1");

    MesqSample s2 = ch.take(&f);
    ok(!f, "a second take with no new sample reports NOT fresh");
    ok(s2.sample_ms == s1.sample_ms && s2.sample_seq == s1.sample_seq,
       "and repeats the same record, so the transmitter can mark it HELD");

    // The ~55 Hz producer vs ~32 Hz consumer ratio, in miniature.
    for (int i = 0; i < 55; i++) ch.publish(1, 0, 0, 0, 1000 + i);
    MesqSample s3 = ch.take(&f);
    ok(f && s3.sample_ms == 1054, "overwrite keeps the NEWEST sample, not the oldest");
    okf(ch.dropped() == 54, "the 54 superseded samples are counted, not silently lost (got %u)", ch.dropped());
}

/* =====================================================================
   SENS-03 -- quaternion reconstruction policy
   ===================================================================== */
static void test_sens03() {
    section("SENS-03  quaternion reconstruction and validity");
    double w, x, y, z;

    ok(mesqReconstructQuat(0.0, 0.0, 0.0, &w, &x, &y, &z) == MESQ_Q_OK && fabs(w - 1.0) < 1e-12,
       "identity: (0,0,0) -> w=1");

    ok(mesqReconstructQuat(0.5, 0.5, 0.5, &w, &x, &y, &z) == MESQ_Q_OK && fabs(w - 0.5) < 1e-12,
       "a legal interior point reconstructs exactly");

    // Roundoff: overshoot the unit sphere by 1e-9, inside the Q30 error bound.
    {
        double c = sqrt(1.0 / 3.0) * (1.0 + 5e-10);
        MesqQuatStatus st = mesqReconstructQuat(c, c, c, &w, &x, &y, &z);
        ok(st == MESQ_Q_OK || st == MESQ_Q_REPAIRED, "Q30 roundoff overshoot is accepted, not rejected");
        ok(std::isfinite(w) && w >= 0.0, "and yields a finite non-negative w, never NaN");
        double n = w*w + x*x + y*y + z*z;
        ok(fabs(n - 1.0) < 1e-9, "the repaired quaternion is renormalised to unit length");
    }

    // The Phase 1 failure: a genuinely bad sample.
    {
        MesqQuatStatus st = mesqReconstructQuat(1.0, 1.0, 1.0, &w, &x, &y, &z);
        ok(st == MESQ_Q_REJECTED, "a badly out-of-sphere sample (radicand -2) is REJECTED");
    }
    ok(mesqReconstructQuat(NAN, 0, 0, &w, &x, &y, &z) == MESQ_Q_REJECTED, "NaN input rejected");
    ok(mesqReconstructQuat(INFINITY, 0, 0, &w, &x, &y, &z) == MESQ_Q_REJECTED, "infinite input rejected");

    // The zero quaternion must never be produced. Phase 1 produced it on
    // every NaN, because q_to_i16(NaN) == 0 for all four components.
    {
        bool everZero = false;
        std::mt19937 rng(20260920);
        std::uniform_real_distribution<double> d(-2.0, 2.0);
        int okc = 0, rej = 0, rep = 0;
        for (int i = 0; i < 200000; i++) {
            double a = d(rng), b = d(rng), c = d(rng);
            w = x = y = z = 0.0;
            MesqQuatStatus st = mesqReconstructQuat(a, b, c, &w, &x, &y, &z);
            if (st == MESQ_Q_REJECTED) { rej++; continue; }
            if (st == MESQ_Q_REPAIRED) rep++; else okc++;
            double n = w*w + x*x + y*y + z*z;
            if (fabs(n) < 1e-6) everZero = true;
            if (fabs(n - 1.0) > 1e-9) everZero = true;   // also catches non-unit output
        }
        printf("     200k uniform inputs: ok=%d repaired=%d rejected=%d\n", okc, rep, rej);
        ok(!everZero, "across 200k random inputs, every ACCEPTED result is a unit quaternion");

        // Targeted: the thin shell just OUTSIDE the unit sphere, where Q30
        // roundoff actually puts a real sensor sample. The uniform sampling
        // above never reaches it, so the repair branch would go untested.
        int rep2 = 0, rej2 = 0, bad2 = 0;
        std::uniform_real_distribution<double> shell(0.0, MESQ_RADICAND_EPS * 0.9);
        for (int i = 0; i < 50000; i++) {
            double over = shell(rng);                  // radicand = -over
            double c = sqrt((1.0 + over) / 3.0);
            MesqQuatStatus st = mesqReconstructQuat(c, c, c, &w, &x, &y, &z);
            if (st == MESQ_Q_REJECTED) { rej2++; continue; }
            if (st == MESQ_Q_REPAIRED) rep2++;
            double n = w*w + x*x + y*y + z*z;
            if (!std::isfinite(n) || fabs(n - 1.0) > 1e-9) bad2++;
        }
        printf("     50k roundoff-shell inputs: repaired=%d rejected=%d\n", rep2, rej2);
        okf(rej2 == 0, "every sample inside the Q30 roundoff shell is repaired, not rejected (rejected=%d)", rej2);
        okf(rep2 > 0, "the repair branch is actually exercised (%d repairs)", rep2);
        ok(bad2 == 0, "and every repaired quaternion is finite and unit-norm");

        // Just OUTSIDE the shell must reject, so the threshold is a real edge.
        {
            double over = MESQ_RADICAND_EPS * 10.0;
            double c = sqrt((1.0 + over) / 3.0);
            ok(mesqReconstructQuat(c, c, c, &w, &x, &y, &z) == MESQ_Q_REJECTED,
               "10x the roundoff bound is rejected -- the threshold is a real boundary");
        }
    }

    // The Phase 1 code path, for contrast.
    {
        double rad = 1.0 - (1.0*1.0 + 1.0*1.0 + 1.0*1.0);   // = -2
        double q0  = sqrt(rad);
        ok(std::isnan(q0), "baseline: unguarded sqrt of a negative radicand gives NaN");
        ok(mesq_q_to_i16((float)q0) == 0 && mesq_q_to_i16(1.0f) == 32767,
           "baseline: that NaN becomes 0 on the wire -> the zero quaternion");
    }
}

/* =====================================================================
   NODE-03 -- transmit deadline
   ===================================================================== */
static void test_node03() {
    section("NODE-03  transmit deadline and fleet phase");

    // The Phase 1 arithmetic, stated as a test so the claim is checked.
    {
        int fps = 32;
        ok((1000 / fps) == 31, "baseline: 1000/32 truncates to 31 ms");
        uint32_t prev = 0, t = 0;
        while (!(t > prev + (1000 / fps))) t++;
        ok(t == 32, "baseline: strict > means the gate first opens at 32 ms, not 31.25");
        okf(fabs(1000.0 / 32.0 - 31.25) < 1e-9,
            "baseline therefore delivers <=31.25 attempts/s against a declared 32");
    }

    // The replacement, on an ideal clock.
    {
        MesqDeadline d;
        const uint32_t PERIOD = 1000000u / 32u;          // 31250 us, exact
        ok(PERIOD == 31250, "period is 31250 us exactly, no truncation");
        mesqDeadlineInit(&d, PERIOD, 0, 0, 1);
        int fires = 0;
        for (uint32_t t = 0; t < 1000000u; t += 100) if (mesqDeadlineDue(&d, t)) fires++;
        okf(fires == 32, "exactly 32 sends in one simulated second (got %d)", fires);
    }

    // Jitter must NOT accumulate. This is defect (c).
    {
        MesqDeadline d;
        const uint32_t PERIOD = 31250;
        mesqDeadlineInit(&d, PERIOD, 0, 0, 1);
        std::mt19937 rng(7);
        std::uniform_int_distribution<int> jit(0, 4000);   // up to 4 ms late
        int fires = 0;
        uint32_t t = 0;
        while (t < 10000000u) { t += 100 + jit(rng) / 40; if (mesqDeadlineDue(&d, t)) fires++; }
        // The property that matters is that lateness does NOT accumulate:
        // over 10 s at 31250 us the schedule owes 320 or 321 sends depending
        // on whether the sampling loop's last step crossed the final deadline.
        // Phase 1's re-anchoring would have produced materially fewer.
        okf(fires == 320 || fires == 321,
            "320-321 sends over 10 s under jitter, i.e. no accumulated drift (got %d)", fires);
        okf(d.resyncs == 0, "and no resync was needed (got %u)", d.resyncs);
    }

    // Bounded catch-up: a long stall must not produce a burst.
    {
        MesqDeadline d;
        mesqDeadlineInit(&d, 31250, 0, 0, 1);
        mesqDeadlineDue(&d, 0);
        int burst = 0;
        uint32_t t = 5000000u;                 // 5 s blocked (an init retry)
        while (mesqDeadlineDue(&d, t)) { burst++; if (burst > 10) break; }
        okf(burst == 1, "a 5 s stall yields ONE send, not a catch-up burst (got %d)", burst);
        ok(d.resyncs == 1, "and the resync is counted for the operator");
    }

    // NET-03: deterministic per-node phase spreads the fleet.
    {
        const uint32_t PERIOD = 31250, N = 17;
        std::vector<uint32_t> firsts;
        for (uint32_t id = 0; id < N; id++) {
            MesqDeadline d; mesqDeadlineInit(&d, PERIOD, 0, id, N);
            firsts.push_back(d.next_us);
        }
        bool spread = true;
        for (size_t i = 1; i < firsts.size(); i++) if (firsts[i] <= firsts[i-1]) spread = false;
        ok(spread, "17 pods get 17 strictly increasing first deadlines");
        uint32_t gap = firsts[1] - firsts[0];
        okf(gap >= 1830 && gap <= 1840, "spaced ~%u us apart (period/17 = 1838)", gap);
        ok(firsts[16] < PERIOD, "the whole fleet fits inside one period, so no node is delayed a frame");
    }

    // Wrap safety at the 32-bit microsecond rollover (~71.6 min).
    {
        MesqDeadline d;
        uint32_t nearWrap = 0xFFFFFF00u;
        mesqDeadlineInit(&d, 31250, nearWrap, 0, 1);
        int fires = 0;
        uint32_t t = nearWrap;
        for (int i = 0; i < 20000; i++) { t += 100; if (mesqDeadlineDue(&d, t)) fires++; }
        // 2,000,000 us of simulated time / 31250 = 64 intervals, plus the
        // deadline that was already due on the first call.
        okf(fires == 65, "65 sends across the 32-bit microsecond wrap, none lost (got %d)", fires);
    }
}

/* =====================================================================
   PROTO-01 -- golden vectors, cross-checked against the JavaScript encoder
   ===================================================================== */
static void test_proto01() {
    section("PROTO-01  packet codec golden vectors");
    ok(sizeof(pod_packet_t) == 16, "pod_packet_t is 16 bytes");

    uint8_t b[16];
    mesq_pack(b, 2, 87, 0.75f, 0.5f, -0.5f, 0.25f, 0x1234, 0xABCD);
    std::string h = hex(b, 16);
    printf("     C   golden hex = %s\n", h.c_str());
    // This exact string is asserted in tools/parser_tests.js G1 against the
    // JavaScript encoder. If the two ever diverge, one of these suites fails.
    ok(h == "aa550257ff3f01c0ff1fff5f3412cdab",
       "C output is byte-identical to the JavaScript golden vector");

    ok(b[0] == 0xAA && b[1] == 0x55, "sync word at 0,1");
    ok(b[2] == 2 && b[3] == 87, "id and battery at 2,3");
    ok(mesq_q_to_i16(1.0f) == 32767 && mesq_q_to_i16(-1.0f) == -32767,
       "full scale saturates symmetrically, no wrap to -32768");
    ok(mesq_q_to_i16(2.0f) == 32767 && mesq_q_to_i16(-2.0f) == -32767,
       "out-of-range input saturates rather than flipping sign");
    mesq_pack(b, 0, 250, 1, 0, 0, 0, 0, 0);
    ok(b[3] == 100, "battery is clamped to 100");

    // Counter wrap.
    mesq_pack(b, 0, 50, 1, 0, 0, 0, 65535, 0);
    ok(b[12] == 0xFF && b[13] == 0xFF, "count 65535 encodes as FF FF");
    mesq_pack(b, 0, 50, 1, 0, 0, 0, (uint16_t)(65535 + 1), 0);
    ok(b[12] == 0x00 && b[13] == 0x00, "count wraps 65535 -> 0 cleanly");

    // Reserved ids must never be emitted as bone ids.
    ok(MESQ_ID_STATUS >= MESQ_NUM_BONES && MESQ_ID_CONTROL >= MESQ_NUM_BONES
       && MESQ_ID_HOSTCMD >= MESQ_NUM_BONES,
       "0xFC/0xFE/0xFF all sit outside the 0..16 bone range");
}

/* =====================================================================
   HUB-RESET -- host -> hub command framing
   ===================================================================== */
static void test_hubcmd() {
    section("HUB-RESET  host -> hub command framing");

    MesqCmdParser p; mesqCmdInit(&p);
    MesqCmd cmd;
    int fired = 0;

    auto feed = [&](const uint8_t *d, int n) {
        for (int i = 0; i < n; i++) if (mesqCmdFeed(&p, d[i], &cmd)) fired++;
    };

    // The Phase 1 trigger: ANY byte reset the whole suit.
    {
        mesqCmdInit(&p); fired = 0;
        const char *noise = "hello\r\n\x00\xff garbage 12345";
        feed((const uint8_t *)noise, 26);
        ok(fired == 0, "26 bytes of arbitrary text and noise trigger NOTHING");
    }
    {
        mesqCmdInit(&p); fired = 0;
        const char *js = "{\"bone\":\"Hips\",\"x\":0.5,\"y\":0,\"z\":0,\"w\":1}\n";
        feed((const uint8_t *)js, (int)strlen(js));
        ok(fired == 0, "an ordinary JSON line triggers nothing");
    }
    {
        mesqCmdInit(&p); fired = 0;
        std::mt19937 rng(99); std::uniform_int_distribution<int> d(0, 255);
        for (int i = 0; i < 100000; i++) { uint8_t byte = (uint8_t)d(rng); if (mesqCmdFeed(&p, byte, &cmd)) fired++; }
        okf(fired == 0, "100,000 random bytes trigger nothing (got %d)", fired);
    }

    // A real command still works.
    {
        mesqCmdInit(&p); fired = 0;
        uint8_t f[8]; int n = mesqCmdBuild(f, MESQ_CMD_RESET_FLEET, NULL, 0);
        feed(f, n);
        ok(fired == 1 && cmd.cmd == MESQ_CMD_RESET_FLEET, "a well-formed reset command fires exactly once");

        // Cross-language: the browser builds this frame in JavaScript
        // (MesqParser.encodeCommand) and this C parser has to accept it.
        // tools/parser_tests.js C1 asserts the same hex on the other side.
        std::string h = hex(f, n);
        printf("     C   reset frame = %s\n", h.c_str());
        ok(h == "aa55fc010001", "C encoder is byte-identical to the JavaScript one");
    }
    // Byte-at-a-time and split across "reads".
    {
        mesqCmdInit(&p); fired = 0;
        uint8_t f[8]; int n = mesqCmdBuild(f, MESQ_CMD_RESET_FLEET, NULL, 0);
        for (int i = 0; i < n; i++) feed(f + i, 1);
        ok(fired == 1, "the same command fires once when delivered byte by byte");
    }
    // Corrupted checksum must fail closed.
    {
        mesqCmdInit(&p); fired = 0;
        uint8_t f[8]; int n = mesqCmdBuild(f, MESQ_CMD_RESET_FLEET, NULL, 0);
        f[n - 1] ^= 0xFF;
        feed(f, n);
        ok(fired == 0, "a corrupted checksum is rejected, not executed");
        ok(p.badChecksum == 1, "and the rejection is counted");
    }
    // Noise, then a valid command: the parser must still be able to see it.
    {
        mesqCmdInit(&p); fired = 0;
        const char *noise = "\xAA\xAA\x55\x55\xFC\xFC junk ";
        feed((const uint8_t *)noise, 20);
        uint8_t f[8]; int n = mesqCmdBuild(f, MESQ_CMD_RESET_FLEET, NULL, 0);
        feed(f, n);
        ok(fired == 1, "a valid command is recovered after leading noise and false syncs");
    }
    // An oversized declared length must be rejected without buffering it.
    {
        mesqCmdInit(&p); fired = 0;
        uint8_t bad[5] = { 0xAA, 0x55, 0xFC, MESQ_CMD_RESET_FLEET, 0xFF };
        feed(bad, 5);
        ok(fired == 0 && p.badLen == 1, "an over-long declared payload is refused at the length byte");
        uint8_t f[8]; int n = mesqCmdBuild(f, MESQ_CMD_RESET_FLEET, NULL, 0);
        feed(f, n);
        ok(fired == 1, "and the parser recovers immediately afterwards");
    }
    // Unknown command id: fail closed, do not guess.
    {
        mesqCmdInit(&p); fired = 0;
        uint8_t f[8]; int n = mesqCmdBuild(f, 0x7E, NULL, 0);
        feed(f, n);
        ok(fired == 1 && cmd.cmd == 0x7E, "an unknown-but-well-formed command is surfaced, not silently dropped");
    }
    // Sustained realistic browser traffic must never fire.
    {
        mesqCmdInit(&p); fired = 0;
        for (int i = 0; i < 5000; i++) {
            char line[128];
            int n = snprintf(line, sizeof(line),
                "{\"bone\":\"Hips\",\"x\":%.3f,\"y\":0.1,\"z\":0.2,\"w\":0.9}\n", i * 0.001);
            feed((const uint8_t *)line, n);
        }
        okf(fired == 0, "5,000 realistic JSON lines trigger no reset (got %d)", fired);
    }
}

/* =====================================================================
   HUB-02 -- the USB write queue
   ===================================================================== */
static void test_hub02() {
    section("HUB-02  single-owner USB write queue");

    MesqUsbQueue q; mesqUsbInit(&q);

    // Two producers, exactly like the ESP-NOW receive callback and the
    // WebSocket handler, both trying to write to one stream.
    std::atomic<bool> stop{false};
    std::atomic<int> pushedA{0}, pushedB{0};
    const int ATTEMPT_A = 20000, ATTEMPT_B = 5000;

    std::thread radio([&] {
        uint8_t pkt[16];
        for (int i = 0; i < ATTEMPT_A; i++) {
            mesq_pack(pkt, (uint8_t)(i % 17), 50, 1, 0, 0, 0, (uint16_t)i, (uint16_t)i);
            if (mesqUsbPush(&q, pkt, 16)) pushedA++;
        }
    });
    std::thread phone([&] {
        const char *line = "{\"bone\":\"Hips\",\"x\":0.5,\"y\":0,\"z\":0,\"w\":1}\n";
        for (int i = 0; i < ATTEMPT_B; i++) {
            if (mesqUsbPush(&q, (const uint8_t *)line, (uint8_t)strlen(line))) pushedB++;
        }
    });

    // One drain thread = the single USB writer task.
    std::vector<uint8_t> out;
    std::thread writer([&] {
        uint8_t buf[MESQ_USB_REC_MAX]; uint8_t len;
        while (!stop || q.count > 0) {
            while (mesqUsbPop(&q, buf, &len)) out.insert(out.end(), buf, buf + len);
        }
    });

    radio.join(); phone.join(); stop = true; writer.join();

    printf("     pushed: radio=%d phone=%d  dropped=%u  highWater=%u/%u\n",
           pushedA.load(), pushedB.load(), q.dropped, q.highWater, MESQ_USB_QUEUE_LEN);

    // Every emitted record must appear WHOLE. Walk the output: a 16-byte pod
    // frame must start with the sync word, a JSON line must end with \n.
    int podFrames = 0, jsonLines = 0; bool corrupt = false;
    size_t i = 0;
    while (i < out.size()) {
        if (out[i] == 0xAA && i + 16 <= out.size() && out[i+1] == 0x55 && out[i+2] < 17) {
            podFrames++; i += 16;
        } else if (out[i] == '{') {
            size_t e = i; while (e < out.size() && out[e] != '\n') e++;
            if (e >= out.size()) { corrupt = true; break; }
            // A pod frame's sync word inside the line means an interleave.
            for (size_t k = i; k < e; k++) if (out[k] == 0xAA && out[k+1] == 0x55) corrupt = true;
            jsonLines++; i = e + 1;
        } else { corrupt = true; break; }
    }
    printf("     drained: %d pod frames, %d json lines\n", podFrames, jsonLines);
    ok(!corrupt, "no record is ever split or interleaved with another");
    okf(podFrames + jsonLines == pushedA + pushedB - (int)0,
        "every accepted record was drained whole (%d + %d vs %d accepted)",
        podFrames, jsonLines, pushedA.load() + pushedB.load());
    // The point of the queue is that backpressure becomes a COUNTER instead
    // of corruption. This test deliberately over-drives it (two tight
    // producer loops against one drain) so the drop path is exercised.
    okf((uint32_t)(pushedA + pushedB) + q.dropped == (uint32_t)(ATTEMPT_A + ATTEMPT_B),
        "accepted(%d) + dropped(%u) = offered(%d): no record vanished unaccounted",
        pushedA + pushedB, q.dropped, ATTEMPT_A + ATTEMPT_B);
    okf(q.pushed == q.popped, "every queued record was drained (pushed=%u popped=%u)", q.pushed, q.popped);
    ok(q.highWater <= MESQ_USB_QUEUE_LEN, "queue high-water mark stayed within the ring");
}

static void test_hubstats() {
    section("HUB-05  per-node integrity counters");
    MesqNodeStats s[MESQ_NUM_BONES]; mesqStatsInit(s, MESQ_NUM_BONES);

    for (uint16_t c = 0; c < 100; c++) mesqStatsOnPacket(&s[3], c, 1000 + c);
    ok(s[3].received == 100 && s[3].gaps == 0, "100 consecutive packets: no gaps");

    mesqStatsOnPacket(&s[3], 105, 2000);                      // 100..104 lost
    okf(s[3].gaps == 1 && s[3].lost == 5, "a 5-packet hole is reported as 1 gap / 5 lost (got %u/%u)",
        s[3].gaps, s[3].lost);

    mesqStatsOnPacket(&s[3], 105, 2001);
    ok(s[3].duplicates == 1, "a repeated counter is a duplicate, not a gap");

    mesqStatsOnPacket(&s[3], 104, 2002);
    ok(s[3].reordered == 1, "an older counter is reordering, not 65535 lost");

    // Wrap must not look like catastrophic loss.
    MesqNodeStats w; mesqStatsInit(&w, 1);
    mesqStatsOnPacket(&w, 65534, 10);
    mesqStatsOnPacket(&w, 65535, 20);
    mesqStatsOnPacket(&w, 0,     30);
    mesqStatsOnPacket(&w, 1,     40);
    okf(w.gaps == 0 && w.lost == 0, "counter wrap 65535->0 costs 0 lost (got %u)", w.lost);

    // A pod reboot restarts count at 0 from a high value: that is a RESYNC,
    // not 65000 lost packets.
    MesqNodeStats r; mesqStatsInit(&r, 1);
    for (uint16_t c = 60000; c < 60010; c++) mesqStatsOnPacket(&r, c, c);
    mesqStatsOnPacket(&r, 0, 70000);
    okf(r.resyncs == 1 && r.lost == 0, "a pod reboot is 1 resync, not a huge loss count (lost=%u)", r.lost);
}

int main() {
    printf("MESQUITE FIRMWARE CORE TESTS  (host build, real threads)\n");
    test_node01();
    test_node02();
    test_sens03();
    test_node03();
    test_proto01();
    test_hubcmd();
    test_hub02();
    test_hubstats();
    printf("\n%s\npass=%d  fail=%d\n%s\n",
           std::string(70, '=').c_str(), g_pass, g_fail, std::string(70, '=').c_str());
    return g_fail ? 1 : 0;
}
