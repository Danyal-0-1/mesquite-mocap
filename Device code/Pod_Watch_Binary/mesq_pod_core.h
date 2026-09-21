/* =========================================================================
   MESQUITE POD CORE  --  Device code/Pod_Watch_Binary/mesq_pod_core.h

   The three pieces of pod logic that are (a) easy to get wrong and (b)
   impossible to test on a watch: the cross-core sample handoff, the
   quaternion reconstruction policy, and the transmit deadline.

   Everything here compiles on the ESP32 AND on a desktop with
   -DMESQ_HOST_TEST, so tools/firmware_tests.cpp exercises the real code
   under real threads. Compilation is not verification; this is.

   Addresses Phase 1 NODE-01, NODE-02, NODE-03, SENS-03.
   LEARNING: A1_05_LEARNING_GUIDE.md ch. 4 (handoff), 5 (quaternion), 6 (time).
   ========================================================================= */
#ifndef MESQ_POD_CORE_H
#define MESQ_POD_CORE_H

#include <stdint.h>
#include <math.h>
#include <string.h>

/* -------------------------------------------------------------------------
   CONCURRENCY primitive.

   On the ESP32 this is portMUX_TYPE: an SMP spinlock that also masks
   interrupts on the calling core. On the host it is a spinlock with the same
   shape, so the test exercises the same critical-section boundaries.

   WHY a critical section and not a seqlock: a seqlock would be lock-free, but
   it depends on acquire/release ordering guarantees I cannot verify against
   this exact Xtensa toolchain without hardware, and getting the barriers
   subtly wrong reintroduces the very tearing this replaces. The critical
   section copies 24 bytes -- well under a microsecond -- and cannot tear.
   Rule: NOTHING may be called inside it. No I2C, no radio, no Serial.
   ------------------------------------------------------------------------- */
#ifdef MESQ_HOST_TEST
  #include <atomic>
  struct MesqMux { std::atomic_flag f = ATOMIC_FLAG_INIT; };
  static inline void mesqEnter(MesqMux *m) { while (m->f.test_and_set(std::memory_order_acquire)) {} }
  static inline void mesqExit (MesqMux *m) { m->f.clear(std::memory_order_release); }
  // std::atomic_flag is not assignable, so initialisation is a call.
  static inline void mesqMuxInit(MesqMux *m) { m->f.clear(std::memory_order_release); }
#else
  // MESQ_STUB_BUILD lets tools/check_sketches.sh type-check THIS branch --
  // the real ESP32 path -- without the FreeRTOS tree on the host.
  #ifndef MESQ_STUB_BUILD
    #include "freertos/FreeRTOS.h"
  #endif
  typedef portMUX_TYPE MesqMux;
  static inline void mesqEnter(MesqMux *m) { portENTER_CRITICAL(m); }
  static inline void mesqExit (MesqMux *m) { portEXIT_CRITICAL(m); }
  static inline void mesqMuxInit(MesqMux *m) { portMUX_TYPE t = portMUX_INITIALIZER_UNLOCKED; *m = t; }
#endif

/* =========================================================================
   1. COHERENT SAMPLE RECORD  -- NODE-01, NODE-02
   =========================================================================
   Phase 1 shipped four independent floats in a plain `struct Quat quat`.
   TaskReadIMU on core 1 wrote them one at a time; TaskWifi on core 0 read
   them one at a time, with no synchronisation of any kind. A read that lands
   between two of those stores returns a mix of two different orientations --
   a "torn" quaternion, which is not a rotation at all and whose norm is not 1.

   The fix is not to make the writes atomic. It is to stop treating the pose
   as four numbers and start treating it as ONE RECORD that also carries the
   time it was taken, the sequence it belongs to, and whether it is usable.
   Those five things must agree with each other or none of them mean anything.

   UNITS: quaternion components are dimensionless, in [-1, 1], with
          w^2+x^2+y^2+z^2 = 1. sample_ms is milliseconds on THIS pod's
          millis() clock, which shares no epoch with any other pod (SYNC-01).
   ========================================================================= */
struct MesqSample {
    float    w, x, y, z;
    uint32_t sample_ms;    // pod millis() at FIFO decode, not at transmit
    uint32_t sample_seq;   // increments once per DISTINCT DMP sample
    uint8_t  valid;        // 0 = never publish this downstream
};

/* Single producer (TaskReadIMU, core 1), single consumer (TaskWifi, core 0).

   Length-one with overwrite, deliberately, not a FreeRTOS queue: the radio
   always wants the NEWEST pose. A depth-N queue would hand the transmitter a
   backlog of stale orientations after any hiccup, which is worse than
   dropping them -- Phase 1 NODE-03 already showed the DMP produces ~55/s
   while the radio sends ~32/s, so ~23 samples/s are SUPPOSED to be discarded.
   `dropped` counts them so the discard is measured rather than assumed. */
class MesqSampleChannel {
public:
    MesqSampleChannel() : seq_(0), dropped_(0), published_(0), consumed_(0) {
        mesqMuxInit(&mux_);
        memset(&slot_, 0, sizeof(slot_));
        slot_.w = 1.0f;          // identity rotation until the first real sample
    }

    /* Producer. Called from TaskReadIMU only. Assigns the sequence number
       itself so a caller cannot desynchronise it from the payload. */
    void publish(float w, float x, float y, float z, uint32_t ms) {
        mesqEnter(&mux_);
        // If a sample is ALREADY waiting, we are about to overwrite one the
        // radio never got to. That is the intended ~55 Hz -> ~32 Hz decimation,
        // but it must be counted: `published = consumed + dropped` is the
        // pod-side leg of the master prompt's G-ledger, and an unexplained
        // imbalance there is a defect, not a rounding artefact.
        if (fresh_) dropped_++;
        slot_.w = w; slot_.x = x; slot_.y = y; slot_.z = z;
        slot_.sample_ms  = ms;
        slot_.sample_seq = ++seq_;
        slot_.valid      = 1;
        fresh_ = true;
        published_++;
        mesqExit(&mux_);
    }

    /* Consumer. Called from TaskWifi only. Always succeeds: if no new sample
       has arrived the previous one is returned again with *is_fresh false, so
       the transmitter can mark the packet as a HELD pose rather than passing
       a repeat off as a new measurement (SYNC-08).

       The copy is 24 bytes inside the critical section and nothing else. */
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

    uint32_t dropped()   const { return dropped_; }
    uint32_t published() const { return published_; }
    uint32_t consumed()  const { return consumed_; }

private:
    MesqMux    mux_;
    MesqSample slot_;
    uint32_t   seq_;
    uint32_t   dropped_;
    uint32_t   published_;
    uint32_t   consumed_;
    bool       fresh_ = false;
};

/* Invariant a torn read would break. A mixed record fails this because the
   two halves came from different orientations, so the norm drifts off 1.
   NOTE the limit of this check: passing it is necessary, not sufficient --
   that is why sample_seq exists and why the stress test compares sequence
   numbers, not just norms (master prompt §11, "Concurrency and timing"). */
static inline bool mesqSampleCoherent(const MesqSample &s, float tol = 1e-3f) {
    float n = s.w * s.w + s.x * s.x + s.y * s.y + s.z * s.z;
    return isfinite(n) && fabsf(n - 1.0f) <= tol;
}

/* =========================================================================
   2. QUATERNION RECONSTRUCTION POLICY  -- SENS-03
   =========================================================================
   MATH: the ICM-20948 DMP emits a 6-axis Game Rotation Vector as three
   int32 components scaled by 2^30 (Q30 fixed point). The scalar part is not
   transmitted; it is recovered from the unit-norm constraint

        w^2 + x^2 + y^2 + z^2 = 1      =>      w = sqrt(1 - (x^2+y^2+z^2))

   Phase 1 called sqrt() on that radicand unguarded. When it went negative
   sqrt returned NaN, q_to_i16() mapped NaN to 0, and the pod transmitted
   (0,0,0,0) -- which is not a rotation, has no inverse, and silently
   corrupts every downstream composition it touches.

   Two different things produce a negative radicand and they need opposite
   treatment:

     ROUNDOFF. Each Q30 component carries at most 2^-30 of quantisation
     error, so the sum of three squares can overshoot 1 by no more than about
     3 * 2 * 1 * 2^-30 ~= 5.6e-9. Clamping to zero here costs nothing: the
     true w was already within 7.5e-5 of zero.

     A BAD SAMPLE. A garbage FIFO read gives components well outside the unit
     sphere and a radicand of order -1. There is no correct pose to recover.
     Rejecting it and holding the previous orientation is right; inventing a
     plausible rotation is not.

   MESQ_RADICAND_EPS sits at 1e-6, about 180x the roundoff bound and about
   six orders of magnitude away from a real failure, so the two cases cannot
   be confused.
   ========================================================================= */
#define MESQ_Q30_SCALE 1073741824.0   /* 2^30 */
static const double MESQ_RADICAND_EPS = 1e-6;

enum MesqQuatStatus {
    MESQ_Q_OK       = 0,   // radicand non-negative, norm good
    MESQ_Q_REPAIRED = 1,   // tiny negative radicand clamped to zero
    MESQ_Q_REJECTED = 2    // non-finite, or too far outside the unit sphere
};

/* Reconstructs w from the three DMP components already scaled to [-1,1].
   On MESQ_Q_REJECTED the outputs are left untouched -- the caller keeps the
   previous pose. It NEVER returns the zero quaternion. */
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

    /* Renormalise. After clamping, the vector part may be marginally long;
       one divide restores the invariant every consumer depends on. */
    double n = sqrt(w * w + sumsq);
    if (!isfinite(n) || n < 1e-9) return MESQ_Q_REJECTED;   // degenerate, incl. all-zero

    *out_w = w / n; *out_x = x / n; *out_y = y / n; *out_z = z / n;
    return st;
}

/* Q30 -> [-1,1]. Kept as its own function so the scale appears exactly once. */
static inline double mesqQ30ToDouble(int32_t q) { return ((double)q) / MESQ_Q30_SCALE; }

/* =========================================================================
   3. TRANSMIT DEADLINE  -- NODE-03, NET-03
   =========================================================================
   Phase 1:
        static uint32_t prev_ms = millis();
        if (millis() > (prev_ms + (1000 / fps))) { ...send...; prev_ms = millis(); }

   Three defects in two lines.

     (a) 1000 / 32 is INTEGER division -> 31, not 31.25.
     (b) `>` rather than `>=` means the branch first passes at 32 ms.
     (c) prev_ms is re-anchored to the time the send FINISHED, so every late
         tick pushes the whole schedule later for good. The error accumulates
         instead of cancelling.
   Net effect: a declared 32 fps delivering at most ~31.25 attempts/s, and
   drifting below that under load.

   The replacement keeps the deadline on a fixed grid in MICROSECONDS, so
   31250 us is exact, and advances it by whole periods so lateness in one
   tick is absorbed by the next rather than carried forever.

   NET-03: a fleet-wide reset leaves 17 pods transmitting in phase, all
   contending for the same airtime slot. Seeding each pod's first deadline at
   id * period / node_count spreads them deterministically across the period
   at zero cost and with no coordination.
   ========================================================================= */
#define MESQ_MAX_CATCHUP 4   /* bounded: never fire a burst to "catch up" */

struct MesqDeadline {
    uint32_t period_us;
    uint32_t next_us;
    uint32_t late_us_max;
    uint32_t resyncs;        /* times we fell >MAX_CATCHUP periods behind */
};

/* node_id/node_count give the deterministic phase offset. Pass 0/1 for none. */
static inline void mesqDeadlineInit(MesqDeadline *d, uint32_t period_us,
                                    uint32_t now_us, uint32_t node_id,
                                    uint32_t node_count) {
    d->period_us   = period_us;
    d->late_us_max = 0;
    d->resyncs     = 0;
    uint32_t phase = (node_count > 1) ? (uint32_t)(((uint64_t)node_id * period_us) / node_count) : 0;
    d->next_us     = now_us + phase;
}

/* UNITS: now_us is a free-running microsecond counter. The subtraction is
   written as (now - next) on unsigned types so it stays correct across the
   32-bit wrap at ~71.6 minutes -- a pod outlives that in a single session. */
static inline bool mesqDeadlineDue(MesqDeadline *d, uint32_t now_us) {
    int32_t delta = (int32_t)(now_us - d->next_us);
    if (delta < 0) return false;

    if ((uint32_t)delta > d->late_us_max) d->late_us_max = (uint32_t)delta;

    if ((uint32_t)delta > d->period_us * MESQ_MAX_CATCHUP) {
        /* We were blocked for a long time (a display redraw, an init retry).
           Snapping to now is right: firing MAX_CATCHUP packets back to back
           would dump a burst into the air for no benefit. */
        d->next_us = now_us + d->period_us;
        d->resyncs++;
    } else {
        d->next_us += d->period_us;     /* fixed grid -- no accumulated drift */
    }
    return true;
}

#endif /* MESQ_POD_CORE_H */
