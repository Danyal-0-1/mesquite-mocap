/* =========================================================================
   MESQUITE POSE PACKET  --  Device code/Pod_Watch_Binary/mesq_packet.h

   PROTOCOL v1. The layout is UNCHANGED from Phase 1 -- 16 bytes, same
   offsets, same endianness -- so every deployed hub, browser and capture
   tool keeps working. Phase 3 changed the MEANING of one field and documents
   the meaning of another; see `ms_lo` below. A layout change would require
   the versioned migration in A1_01_SOLUTION_DECISIONS.md and the user's
   approval, and has deliberately not been made.

     off  size  field    meaning
     0    1     sync0    0xAA
     1    1     sync1    0x55
     2    1     id       bone enum 0..16 (0xFE/0xFF reserved for control)
     3    1     batt     0..100 percent
     4    2     qx_i16   int16 little-endian, = float * 32767, truncated
     6    2     qy_i16
     8    2     qz_i16
     10   2     qw_i16
     12   2     count    uint16 PACKET sequence, wraps at 65535
     14   2     ms_lo    uint16 low bits of pod millis() AT SAMPLE DECODE

   PROTOCOL `count`: increments once per TRANSMIT, not per sensor sample.
   The DMP produces ~55 samples/s and the radio sends ~32/s, so a sample
   counter here would look like permanent loss to a gap detector. Gaps in
   `count` therefore mean RADIO loss, which is what SYNC-03 measures.

   PROTOCOL `ms_lo`: Phase 1 wrote millis() at the moment of transmission,
   which made it useless for anything except a rough arrival clock (NODE-02,
   SYNC-02). Phase 3 writes the pod clock at the moment the sample was
   decoded from the FIFO. Cost: zero bytes. Gain: two consecutive packets
   carrying the SAME ms_lo provably carry the SAME sensor sample, so the
   browser can tell a fresh measurement from a held pose (SYNC-08) without
   any wire change. It still wraps every 65.536 s and still shares no epoch
   with any other pod (SYNC-01) -- it is a freshness marker, not a timebase.
   ========================================================================= */
#ifndef MESQ_PACKET_H
#define MESQ_PACKET_H

#include <stdint.h>
#include <math.h>

#define MESQ_PACKET_LEN   16
#define MESQ_SYNC0        0xAA
#define MESQ_SYNC1        0x55
#define MESQ_ID_STATUS    0xFE   /* hub -> host framed status  */
#define MESQ_ID_CONTROL   0xFF   /* hub -> pod control          */
#define MESQ_ID_HOSTCMD   0xFC   /* host -> hub control         */
#define MESQ_NUM_BONES    17

typedef struct __attribute__((packed)) pod_packet_t {
    uint8_t  sync0;
    uint8_t  sync1;
    uint8_t  id;
    uint8_t  batt;
    int16_t  qx;
    int16_t  qy;
    int16_t  qz;
    int16_t  qw;
    uint16_t count;
    uint16_t ms_lo;
} pod_packet_t;

/* A silent layout change is the one protocol failure no runtime check can
   catch, so it is caught at compile time instead. */
#if defined(__cplusplus) && __cplusplus >= 201103L
  static_assert(sizeof(pod_packet_t) == MESQ_PACKET_LEN, "pod_packet_t must be exactly 16 bytes");
#endif

/* MATH: quantise a float in [-1,1] to int16. Saturates rather than wrapping,
   so an out-of-range value cannot flip sign and become a valid-looking pose
   pointing the other way. NaN maps to 0 -- but note that the SENS-03 policy
   in mesq_pod_core.h rejects a non-finite sample long before it gets here, so
   this is the second line of defence, not the first.
   UNITS: the step is 1/32767, i.e. 0.006 deg of angular error (Phase 1
   EST-03 measured this as negligible -- do not revisit it). */
static inline int16_t mesq_q_to_i16(float v) {
    if (isnan(v)) return 0;
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 32767.0f);      /* C cast truncates toward zero */
}

/* Serialise explicitly rather than memcpy'ing the struct: the field order is
   then a property of THIS function, visible and testable, instead of a
   property of whatever the compiler decided to do with the struct. */
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
    out[6]  = (uint8_t)( (uint16_t)y       & 0xFF);
    out[7]  = (uint8_t)(((uint16_t)y >> 8) & 0xFF);
    out[8]  = (uint8_t)( (uint16_t)z       & 0xFF);
    out[9]  = (uint8_t)(((uint16_t)z >> 8) & 0xFF);
    out[10] = (uint8_t)( (uint16_t)w       & 0xFF);
    out[11] = (uint8_t)(((uint16_t)w >> 8) & 0xFF);
    out[12] = (uint8_t)( count       & 0xFF);
    out[13] = (uint8_t)((count >> 8) & 0xFF);
    out[14] = (uint8_t)( ms_lo       & 0xFF);
    out[15] = (uint8_t)((ms_lo >> 8) & 0xFF);
}

#endif /* MESQ_PACKET_H */
