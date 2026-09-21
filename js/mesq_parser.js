/* =========================================================================
   MESQUITE STREAM PARSER  --  js/mesq_parser.js

   WHY THIS FILE EXISTS
   The USB CDC link from the hub carries three interleaved record kinds on one
   byte stream. Phase 1 `WEB-02` and Phase 2 `T6(b)` proved the original
   inline parser could latch permanently: one unterminated JSON line captured
   every subsequent byte, including binary pod frames, forever. Phase 2 also
   left the test fixture holding a hand-copied duplicate of the parser
   ("kept in sync manually"), so a fix here would not have been tested there.

   This module is the single implementation. js/webserialnative.js consumes
   it in the browser; tools/replay_harness.js consumes it under Node. There is
   no second copy to drift.

   PROTOCOL: three record kinds share the 0xAA 0x55 sync word.
     [0xAA][0x55][id 0..16][batt][qx][qy][qz][qw][count][ms] -- 16 B pod pose
     [0xAA][0x55][0xFE][len][payload...]                     -- hub status
     '{' ... '}' '\n'                                        -- phone JSON line
   0xFF is reserved for hub -> pod control and never appears host-bound.
   0xFC is reserved for host -> hub control and never appears host-bound.

   MATH: valid JSON text here is 7-bit ASCII, so the bytes 0xAA and 0x55 can
   NEVER occur inside a well-formed JSON line. That is what makes the resync
   below deterministic rather than heuristic -- see `RECOVERY` at feed().
   ========================================================================= */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.MesqParser = factory();
}(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  var POD_PACKET_LEN = 16;
  var SYNC0          = 0xAA;
  var SYNC1          = 0x55;
  var STATUS_MARKER  = 0xFE;   // hub -> host framed status
  var LBRACE         = 0x7B;   // '{'
  var NEWLINE        = 0x0A;

  // PROTOCOL: bone ids are 0..16 inclusive. MUST match Pod_Watch_Binary.ino's
  // boneName[] and Dongle_Binary.ino's POD_ABBR[].
  var BONE_NAMES = [
    "Head", "Spine", "HipsAlt",
    "LeftArm", "LeftForeArm", "LeftHand",
    "RightArm", "RightForeArm", "RightHand",
    "LeftUpLeg", "LeftLeg", "LeftFoot",
    "RightUpLeg", "RightLeg", "RightFoot",
    "LeftShoulder", "RightShoulder"
  ];

  // RECOVERY: hard bounds. Without these a hostile or merely broken stream
  // grows a buffer until the tab dies.
  //   MAX_JSON_LINE -- the phone emits ~150 B lines; 512 is >3x headroom.
  //   MAX_RX_BUF    -- a parser that cannot make progress must shed bytes
  //                    rather than accumulate them.
  //   JSON_STALL_MS -- an unterminated line left by a mid-line disconnect is
  //                    abandoned instead of waiting for a newline forever.
  var MAX_JSON_LINE = 512;
  var MAX_RX_BUF    = 4096;
  var JSON_STALL_MS = 2000;

  // Legacy repair for the JSON pod firmware, which put a `String bone` inside
  // a memcpy'd struct and so shipped a heap pointer on the wire. Longer names
  // first so "HipsAlt" is not consumed as "Hips".
  var BONE_REPAIR_RE = /"bone":"(LeftForeArm|RightForeArm|LeftShoulder|RightShoulder|LeftUpLeg|RightUpLeg|LeftHand|RightHand|LeftArm|RightArm|LeftLeg|RightLeg|LeftFoot|RightFoot|HipsAlt|Spine|Head|Hips)"([^,}]*)/g;
  var NAN_RE = /:\s*-?nan\b/gi;
  var INF_RE = /:\s*-?inf(?:inity)?\b/gi;

  function repairLegacyJson(line) {
    var lastBrace = line.lastIndexOf('}');
    if (lastBrace >= 0 && lastBrace < line.length - 1) line = line.slice(0, lastBrace + 1);
    line = line.replace(BONE_REPAIR_RE, '"bone":"$1"');
    line = line.replace(NAN_RE, ':null');
    line = line.replace(INF_RE, ':null');
    return line;
  }

  function isBadNum(v) {
    return v === null || v === undefined || (typeof v === 'number' && !isFinite(v));
  }

  /* -----------------------------------------------------------------------
     unpackPodPacket

     UNITS: qx..qw are int16 = float * 32767, so the decoded component is in
     [-1, 1] with a quantisation step of 1/32767 (Phase 1 `EST-03` measured the
     resulting angular error at 0.006 deg -- negligible, do not revisit).

     PROTOCOL: `count` is the PACKET sequence, incremented once per transmit.
     It is NOT the sensor-sample sequence: the pod transmits at ~32 Hz while
     the DMP produces ~55 Hz, so a sample counter here would appear to gap
     constantly. Gap detection on `count` therefore measures RADIO loss, which
     is what SYNC-03 wants.

     PROTOCOL: `ms` is the low 16 bits of the pod's millis() AT SAMPLE DECODE
     (Phase 3 NODE-02). Two consecutive packets carrying the SAME `ms` carry
     the same sensor sample -- the pod had no fresh pose to send. That makes
     held frames detectable downstream, which Phase 1 `SYNC-08` asked for. It
     wraps every 65.536 s and shares no epoch with any other pod (`SYNC-01`).
     ----------------------------------------------------------------------- */
  function unpackPodPacket(buf, off) {
    var id = buf[off + 2];
    var name = BONE_NAMES[id];
    if (!name) return null;
    var dv = new DataView(buf.buffer, buf.byteOffset + off, POD_PACKET_LEN);
    return {
      bone:   name,
      id:     id,
      batt:   buf[off + 3] / 100,
      x:      dv.getInt16(4,  true) / 32767,
      y:      dv.getInt16(6,  true) / 32767,
      z:      dv.getInt16(8,  true) / 32767,
      w:      dv.getInt16(10, true) / 32767,
      count:  dv.getUint16(12, true),
      millis: dv.getUint16(14, true)
    };
  }

  // MATH: must be byte-identical to the pod's q_to_i16(), which is a C cast
  // `(int16_t)(v * 32767.0f)` -- truncation TOWARD ZERO, not rounding. Using
  // Math.round() here disagreed by 1 LSB on negative half-values and would
  // have made these golden vectors subtly wrong as firmware references.
  function quantise(v) {
    if (typeof v !== 'number' || !isFinite(v)) return 0;
    if (v >  1.0) v =  1.0;
    if (v < -1.0) v = -1.0;
    return Math.trunc(v * 32767);
  }

  function encodePodPacket(o) {
    var b = new Uint8Array(POD_PACKET_LEN);
    var dv = new DataView(b.buffer);
    b[0] = SYNC0; b[1] = SYNC1;
    b[2] = o.id & 0xFF;
    b[3] = Math.max(0, Math.min(100, o.batt | 0));
    dv.setInt16(4,  quantise(o.x), true);
    dv.setInt16(6,  quantise(o.y), true);
    dv.setInt16(8,  quantise(o.z), true);
    dv.setInt16(10, quantise(o.w), true);
    dv.setUint16(12, o.count & 0xFFFF, true);
    dv.setUint16(14, o.millis & 0xFFFF, true);
    return b;
  }

  /* -----------------------------------------------------------------------
     createParser({ onPodFrame, onJsonObject, onStatusLine, onDrop, now })

     CONCURRENCY: single-threaded. `feed()` is re-entrant-unsafe by design --
     call it from one reader loop only.
     ----------------------------------------------------------------------- */
  function createParser(opts) {
    opts = opts || {};
    var onPodFrame   = opts.onPodFrame   || function () {};
    var onJsonObject = opts.onJsonObject || function () {};
    var onStatusLine = opts.onStatusLine || function () {};
    var onDrop       = opts.onDrop       || function () {};
    var nowFn        = opts.now || function () { return Date.now(); };

    var buf = new Uint8Array(0);
    var jsonPendingSince = -1;   // -1 = no line pending; 0 is a legal timestamp

    var stats = {
      bytes: 0, podFrames: 0, jsonLines: 0, statusLines: 0,
      jsonParseErr: 0, nanFrames: 0,
      falseSync: 0,        // 0xAA not followed by 0x55, or bad bone id
      jsonOverflow: 0,     // line exceeded MAX_JSON_LINE -> abandoned
      jsonStall: 0,        // line abandoned on the stall timeout
      jsonInterleave: 0,   // binary sync found inside a JSON line (HUB-02)
      bufOverflow: 0,      // MAX_RX_BUF exceeded -> oldest bytes shed
      junkBytes: 0,        // bytes belonging to no record kind
      lastBadLine: null, lastBadLineRepaired: null
    };

    function append(chunk) {
      var out = new Uint8Array(buf.length + chunk.length);
      out.set(buf, 0);
      out.set(chunk, buf.length);
      buf = out;
    }

    function bytesToString(a, from, to) {
      var s = "";
      for (var k = from; k < to; k++) s += String.fromCharCode(a[k]);
      return s;
    }

    function commitJsonLine(raw) {
      var j = null;
      try {
        j = JSON.parse(raw);
      } catch (e1) {
        var repaired = repairLegacyJson(raw);
        try {
          j = JSON.parse(repaired);
        } catch (e2) {
          stats.jsonParseErr++;
          stats.lastBadLine = raw;
          stats.lastBadLineRepaired = repaired;
          return;
        }
      }
      if (isBadNum(j.x) || isBadNum(j.y) || isBadNum(j.z) || isBadNum(j.w)) {
        stats.nanFrames++;
        return;
      }
      stats.jsonLines++;
      onJsonObject(j);
    }

    /* RECOVERY: the whole state machine. Every branch either consumes bytes
       or breaks to wait for more -- it can never spin, and it can never hold
       an unbounded amount of input. The three abandon paths (interleave,
       overflow, stall) are what make WEB-02 non-latching: each one advances
       `i` past the offending '{' so the next iteration rescans for a sync. */
    function feed(chunk) {
      stats.bytes += chunk.length;
      append(chunk);

      if (buf.length > MAX_RX_BUF) {
        var shed = buf.length - MAX_RX_BUF;
        stats.bufOverflow += shed;
        onDrop('buf_overflow', shed);
        buf = buf.slice(shed);
      }

      var i = 0;
      while (i < buf.length) {
        var b = buf[i];

        /* -- Branch A: a record announced by the sync word -- */
        if (b === SYNC0) {
          if (buf.length - i < 2) break;
          if (buf[i + 1] !== SYNC1) { stats.falseSync++; i += 1; continue; }

          // A1: hub status frame.
          if (buf.length - i >= 3 && buf[i + 2] === STATUS_MARKER) {
            if (buf.length - i < 4) break;
            var slen = buf[i + 3];
            if (buf.length - i < 4 + slen) break;
            stats.statusLines++;
            onStatusLine(bytesToString(buf, i + 4, i + 4 + slen));
            i += 4 + slen;
            continue;
          }

          // A2: pod pose frame. Validate the bone id BEFORE consuming 16
          // bytes -- the pre-Phase-3 parser consumed a full frame on an
          // invalid id, which turned one false sync into 16 lost bytes.
          if (buf.length - i < 3) break;
          if (buf[i + 2] >= BONE_NAMES.length) { stats.falseSync++; i += 1; continue; }
          if (buf.length - i < POD_PACKET_LEN) break;
          var obj = unpackPodPacket(buf, i);
          stats.podFrames++;
          onPodFrame(obj);
          i += POD_PACKET_LEN;
          continue;
        }

        /* -- Branch B: a phone JSON line -- */
        if (b === LBRACE) {
          var limit = Math.min(buf.length, i + MAX_JSON_LINE);
          var end = -1, interleaveAt = -1;
          for (var k = i; k < limit; k++) {
            // MATH: JSON here is 7-bit ASCII, so 0xAA 0x55 inside a line is
            // proof of a hub-side interleave (HUB-02), not of line content.
            // Abandoning the line and keeping the binary frame is therefore
            // deterministic, not a guess.
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
            i = interleaveAt;                  // hand control back to Branch A
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
          // Incomplete but still within budget: wait for more bytes, unless
          // the stream has stalled mid-line (mid-line disconnect).
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

        /* -- Junk: belongs to no record kind -- */
        stats.junkBytes++;
        i += 1;
      }

      // Keep only the unconsumed tail. `slice` (not `subarray`) so the old
      // backing ArrayBuffer is released instead of being retained by a view.
      buf = buf.slice(i);
    }

    function reset() {
      buf = new Uint8Array(0);
      jsonPendingSince = -1;
      for (var k in stats) {
        if (typeof stats[k] === 'number') stats[k] = 0;
      }
      stats.lastBadLine = null;
      stats.lastBadLineRepaired = null;
    }

    return {
      feed: feed,
      reset: reset,
      stats: stats,
      pending: function () { return buf.length; }
    };
  }

  return {
    POD_PACKET_LEN: POD_PACKET_LEN,
    SYNC0: SYNC0, SYNC1: SYNC1, STATUS_MARKER: STATUS_MARKER,
    MAX_JSON_LINE: MAX_JSON_LINE, MAX_RX_BUF: MAX_RX_BUF,
    JSON_STALL_MS: JSON_STALL_MS,
    BONE_NAMES: BONE_NAMES,
    createParser: createParser,
    unpackPodPacket: unpackPodPacket,
    encodePodPacket: encodePodPacket,
    quantise: quantise,
    repairLegacyJson: repairLegacyJson
  };
}));
