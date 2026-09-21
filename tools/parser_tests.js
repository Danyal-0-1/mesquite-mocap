#!/usr/bin/env node
/* =========================================================================
   MESQUITE PARSER ACCEPTANCE SUITE  --  tools/parser_tests.js

   Tests js/mesq_parser.js, the SAME module js/webserialnative.js loads.
   Phase 2's fixture held a hand-copied duplicate of the parser; this one
   does not, so a regression in the browser parser fails here.

   Covers the master prompt's §11 "Protocol and codec" list:
     golden vectors, offsets/endianness/signed range, counter wrap,
     truncation, oversize, malformed, random, fragmented, concatenated,
     delimiter bytes inside payloads, false sync followed by a valid frame,
     exhaustive split points, seeded random chunking, disconnect in every
     parser state, and strict buffer bounds.

   Run:  node tools/parser_tests.js
   ========================================================================= */
'use strict';
const path = require('path');
const P = require(path.join(__dirname, '..', 'js', 'mesq_parser.js'));

let pass = 0, fail = 0;
function ok(cond, msg) {
  if (cond) { pass++; console.log('  PASS  ' + msg); }
  else { fail++; console.log('  FAIL  ' + msg); }
}
function section(t) { console.log('\n[' + t + ']'); }

function collector() {
  const got = { pod: [], json: [], status: [], drops: [] };
  let clock = 0;
  const p = P.createParser({
    onPodFrame:   o => got.pod.push(o),
    onJsonObject: o => got.json.push(o),
    onStatusLine: s => got.status.push(s),
    onDrop:     (r, n) => got.drops.push({ r, n }),
    now: () => clock
  });
  return { p, got, advance(ms) { clock += ms; } };
}

const pod = (o) => P.encodePodPacket(Object.assign(
  { id: 0, batt: 50, x: 0, y: 0, z: 0, w: 1, count: 0, millis: 0 }, o));

function cat(...arrs) {
  const total = arrs.reduce((n, a) => n + a.length, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const a of arrs) { out.set(a, off); off += a.length; }
  return out;
}
const ascii = (s) => Uint8Array.from(Buffer.from(s, 'latin1'));

function statusFrame(text) {
  return cat(new Uint8Array([0xAA, 0x55, 0xFE, text.length]), ascii(text));
}

/* ===================================================================== */
section('G1  golden vector: exact bytes, offsets, endianness');
{
  const b = pod({ id: 2, batt: 87, x: 0.5, y: -0.5, z: 0.25, w: 0.75,
                  count: 0x1234, millis: 0xABCD });
  const hex = Buffer.from(b).toString('hex');
  ok(b.length === 16, 'frame is exactly 16 bytes');
  ok(b[0] === 0xAA && b[1] === 0x55, 'sync word 0xAA 0x55 at offsets 0,1');
  ok(b[2] === 2, 'bone id at offset 2');
  ok(b[3] === 87, 'battery at offset 3');
  // MATH: firmware truncates toward zero, so 0.5 * 32767 = 16383.5 -> 16383
  // = 0x3FFF, little-endian "ff 3f". -0.5 -> -16383 = 0xC001 -> "01 c0".
  ok(hex.slice(8, 12) === 'ff3f', 'qx little-endian int16, truncated (0.5 -> 0x3FFF)');
  ok(hex.slice(12, 16) === '01c0', 'qy negative int16 two-complement (-0.5 -> 0xC001)');
  ok(hex.slice(24, 28) === '3412', 'count little-endian (0x1234)');
  ok(hex.slice(28, 32) === 'cdab', 'ms little-endian (0xABCD)');
  console.log('     golden hex = ' + hex);

  const rt = P.unpackPodPacket(b, 0);
  ok(rt.bone === 'HipsAlt' && rt.id === 2, 'round-trip bone id -> name');
  ok(Math.abs(rt.x - 0.5) < 1e-4 && Math.abs(rt.y + 0.5) < 1e-4, 'round-trip quaternion within quantisation');
  ok(rt.count === 0x1234 && rt.millis === 0xABCD, 'round-trip count and ms');
}

/* ===================================================================== */
section('G2  int16 saturation and signed range');
{
  const b = pod({ x: 1.0, y: -1.0, z: 0, w: 0 });
  const r = P.unpackPodPacket(b, 0);
  ok(r.x <= 1.0 && r.y >= -1.0, 'full-scale components stay inside [-1,1]');
  const dv = new DataView(b.buffer);
  ok(dv.getInt16(4, true) === 32767, 'x=+1.0 encodes to +32767, not wrapped');
  ok(dv.getInt16(6, true) === -32767, 'y=-1.0 encodes to -32767, not -32768');
}

/* ===================================================================== */
section('T1  clean stream, many frames in one chunk');
{
  const c = collector();
  const frames = [];
  for (let n = 0; n < 40; n++) frames.push(pod({ id: n % 17, count: n }));
  c.p.feed(cat(...frames));
  ok(c.got.pod.length === 40, '40 concatenated frames all decoded');
  ok(c.p.pending() === 0, 'no bytes left pending');
}

/* ===================================================================== */
section('T2  EXHAUSTIVE split points -- every byte boundary');
{
  const stream = cat(pod({ id: 1, count: 1 }), ascii('{"bone":"Hips","x":0,"y":0,"z":0,"w":1}\n'),
                     statusFrame('I1 rx=1,2,3'), pod({ id: 3, count: 2 }));
  let bad = 0;
  for (let sp = 0; sp <= stream.length; sp++) {
    const c = collector();
    c.p.feed(stream.slice(0, sp));
    c.p.feed(stream.slice(sp));
    if (c.got.pod.length !== 2 || c.got.json.length !== 1 || c.got.status.length !== 1) bad++;
  }
  ok(bad === 0, 'all ' + (stream.length + 1) + ' split points decode 2 pod + 1 json + 1 status');
}

/* ===================================================================== */
section('T3  byte-at-a-time fragmentation');
{
  const stream = cat(pod({ id: 5, count: 7 }), statusFrame('x'), pod({ id: 6, count: 8 }));
  const c = collector();
  for (let k = 0; k < stream.length; k++) c.p.feed(stream.slice(k, k + 1));
  ok(c.got.pod.length === 2 && c.got.status.length === 1, 'single-byte feeds decode identically');
}

/* ===================================================================== */
section('T4  WEB-02 REGRESSION: unterminated JSON must not latch');
{
  // The exact Phase 2 T6(b) construction: a '{' with no newline ever, then
  // pod frames whose payload bytes contain NO 0x0A. Pre-fix this latched
  // permanently and _jsonLine grew without bound.
  const c = collector();
  c.p.feed(ascii('{"bone":"Hips","x":0.1'));
  const frames = [];
  for (let n = 0; n < 50; n++) {
    // w=1 -> 0xFF 0x7F ; all other payload bytes chosen 0x0A-free
    frames.push(pod({ id: 1, batt: 50, x: 0, y: 0, z: 0, w: 1, count: 0x0101, millis: 0x0202 }));
  }
  c.p.feed(cat(...frames));
  ok(c.got.pod.length === 50, 'all 50 pod frames decoded despite the open JSON line (was 0)');
  ok(c.p.pending() < P.MAX_JSON_LINE, 'pending buffer stays bounded (' + c.p.pending() + ' B)');
  ok(c.got.drops.some(d => d.r === 'json_interleave'), 'abandon recorded as json_interleave');
}

/* ===================================================================== */
section('T5  HUB-02: binary frame spliced into the middle of a JSON line');
{
  const c = collector();
  c.p.feed(cat(ascii('{"bone":"Hips","x":0.5,'),
               pod({ id: 9, count: 42 }),
               ascii('"y":0.5,"z":0,"w":1}\n'),
               pod({ id: 10, count: 43 })));
  ok(c.got.pod.length === 2, 'both binary frames survive the interleave (was 0)');
  ok(c.got.pod[0].id === 9 && c.got.pod[0].count === 42, 'spliced frame decodes correctly');
  ok(c.got.json.length === 0, 'the corrupted JSON line is discarded, not mis-parsed');
  ok(c.p.stats.jsonInterleave >= 1, 'interleave counted for the operator');
}

/* ===================================================================== */
section('T6  JSON overflow bound');
{
  const c = collector();
  c.p.feed(ascii('{' + 'a'.repeat(P.MAX_JSON_LINE + 200)));
  ok(c.p.stats.jsonOverflow >= 1, 'oversized line abandoned at MAX_JSON_LINE');
  ok(c.p.pending() <= P.MAX_RX_BUF, 'buffer bounded');
  c.p.feed(pod({ id: 4, count: 1 }));
  ok(c.got.pod.length === 1, 'parser still accepts frames after the overflow');
}

/* ===================================================================== */
section('T7  JSON stall timeout (mid-line disconnect)');
{
  const c = collector();
  c.p.feed(ascii('{"bone":"Hips"'));
  c.advance(P.JSON_STALL_MS + 1);
  c.p.feed(ascii(' '));              // any byte re-enters feed() to age it out
  ok(c.p.stats.jsonStall >= 1, 'stalled line abandoned on the timeout');
  c.p.feed(pod({ id: 7, count: 3 }));
  ok(c.got.pod.length === 1, 'frames resume after a stall abandon');
}

/* ===================================================================== */
section('T8  false sync: 0xAA 0x55 with an invalid bone id');
{
  const c = collector();
  // id 0x63 is outside 0..16 -> must skip ONE byte, not sixteen.
  c.p.feed(cat(new Uint8Array([0xAA, 0x55, 0x63]), pod({ id: 11, count: 99 })));
  ok(c.got.pod.length === 1, 'the valid frame after a false sync is recovered');
  ok(c.got.pod[0].id === 11 && c.got.pod[0].count === 99, 'and decodes correctly');
  ok(c.p.stats.falseSync >= 1, 'false sync counted');
}
{
  const c = collector();
  c.p.feed(cat(new Uint8Array([0xAA, 0x31]), pod({ id: 12, count: 5 })));
  ok(c.got.pod.length === 1, '0xAA not followed by 0x55 skips one byte only');
}

/* ===================================================================== */
section('T9  sync-like bytes inside a pod payload must not false-trigger');
{
  const c = collector();
  // Craft a frame whose quaternion bytes contain 0xAA 0x55.
  const b = pod({ id: 8, count: 1 });
  b[6] = 0xAA; b[7] = 0x55;
  c.p.feed(cat(b, pod({ id: 8, count: 2 })));
  ok(c.got.pod.length === 2, 'payload-embedded sync word does not split framing');
  ok(c.got.pod[1].count === 2, 'the following frame is still aligned');
}

/* ===================================================================== */
section('T10  counter wrap 65535 -> 0');
{
  const c = collector();
  c.p.feed(cat(pod({ id: 0, count: 65534 }), pod({ id: 0, count: 65535 }), pod({ id: 0, count: 0 })));
  const seq = c.got.pod.map(o => o.count);
  ok(seq.join(',') === '65534,65535,0', 'counter wraps without sign error: ' + seq.join(','));
  // wrap-safe delta arithmetic the consumer must use
  const d = (65535 - 65534) & 0xFFFF, d2 = (0 - 65535) & 0xFFFF;
  ok(d === 1 && d2 === 1, 'wrap-safe delta ((b-a)&0xFFFF) gives 1 across the wrap');
}

/* ===================================================================== */
section('T11  truncated / oversized / malformed / random input');
{
  const c = collector();
  c.p.feed(pod({ id: 1, count: 1 }).slice(0, 9));     // half a frame, then nothing
  ok(c.got.pod.length === 0, 'truncated frame is not emitted');
  c.p.feed(pod({ id: 1, count: 1 }).slice(9));        // rest arrives
  ok(c.got.pod.length === 1, 'truncated frame completes across the gap');
}
{
  const c = collector();
  c.p.feed(ascii('{not json at all}\n'));
  ok(c.p.stats.jsonParseErr === 1, 'malformed JSON counted, not thrown');
  c.p.feed(pod({ id: 2, count: 1 }));
  ok(c.got.pod.length === 1, 'parser survives malformed JSON');
}
{
  // Deterministic pseudo-random noise, fixed seed.
  let s = 12345;
  const rnd = () => (s = (s * 1103515245 + 12345) & 0x7FFFFFFF) / 0x7FFFFFFF;
  const noise = new Uint8Array(3000);
  for (let k = 0; k < noise.length; k++) noise[k] = Math.floor(rnd() * 256);
  const c = collector();
  c.p.feed(noise);
  c.p.feed(pod({ id: 14, count: 1234 }));
  ok(c.got.pod.some(o => o.id === 14 && o.count === 1234),
     'a valid frame is recovered after 3000 bytes of seeded noise');
  ok(c.p.pending() <= P.MAX_RX_BUF, 'buffer stayed within MAX_RX_BUF under noise');
}

/* ===================================================================== */
section('T12  seeded random chunking fuzz');
{
  let s = 987654321;
  const rnd = () => (s = (s * 1103515245 + 12345) & 0x7FFFFFFF) / 0x7FFFFFFF;
  let worst = null;
  for (let trial = 0; trial < 200; trial++) {
    const frames = [];
    for (let n = 0; n < 12; n++) frames.push(pod({ id: n % 17, count: n }));
    const stream = cat(...frames);
    const c = collector();
    let off = 0;
    while (off < stream.length) {
      const n = 1 + Math.floor(rnd() * 20);
      c.p.feed(stream.slice(off, off + n));
      off += n;
    }
    if (c.got.pod.length !== 12) { worst = trial; break; }
  }
  ok(worst === null, '200 randomly chunked trials each decode all 12 frames');
}

/* ===================================================================== */
section('T13  buffer hard bound under a stream that never parses');
{
  const c = collector();
  for (let k = 0; k < 50; k++) c.p.feed(new Uint8Array(512).fill(0x41)); // 'A' junk
  ok(c.p.pending() <= P.MAX_RX_BUF, 'pending never exceeds MAX_RX_BUF (' + c.p.pending() + ')');
  c.p.feed(pod({ id: 16, count: 1 }));
  ok(c.got.pod.length === 1, 'frame decoded after 25 KB of junk');
}

/* ===================================================================== */
section('T14  disconnect in every parser state, then reconnect');
{
  const states = {
    'mid pod frame':    pod({ id: 1, count: 1 }).slice(0, 7),
    'mid status frame': statusFrame('hello').slice(0, 6),
    'mid json line':    ascii('{"bone":"Hip'),
    'after sync byte':  new Uint8Array([0xAA]),
    'mid sync word':    new Uint8Array([0xAA, 0x55])
  };
  for (const [name, partial] of Object.entries(states)) {
    const c = collector();
    c.p.feed(partial);
    c.p.reset();                                   // what a reconnect does
    c.p.feed(pod({ id: 0, count: 5 }));
    ok(c.got.pod.length === 1 && c.got.pod[0].count === 5,
       'reset() during "' + name + '" then a clean frame decodes');
  }
}

/* ===================================================================== */
section('T15  status frames interleaved with pod frames');
{
  const c = collector();
  c.p.feed(cat(pod({ id: 1, count: 1 }), statusFrame('I1 rx=5,6,7 drop=0/0/0'),
               pod({ id: 2, count: 2 }), statusFrame('I2 rssi=-40'),
               pod({ id: 3, count: 3 })));
  ok(c.got.pod.length === 3, '3 pod frames around 2 status frames');
  ok(c.got.status.length === 2 && c.got.status[0].startsWith('I1 rx='), 'status text intact');
}

/* ===================================================================== */
section('T16  NaN / degenerate quaternion policy on the JSON path');
{
  const c = collector();
  c.p.feed(ascii('{"bone":"Hips","x":nan,"y":0,"z":0,"w":1}\n'));
  ok(c.p.stats.nanFrames === 1, 'nan literal repaired to null then dropped as a dead frame');
  ok(c.got.json.length === 0, 'NaN frame never reaches the rig');
}

/* ===================================================================== */
section('C1  host -> hub command frames (HUB-03)');
{
  const f = P.encodeCommand(P.CMD_RESET_FLEET);
  const h = Buffer.from(f).toString('hex');
  console.log('     reset frame = ' + h);
  // This exact string is asserted in tools/firmware_tests.cpp, which runs the
  // C parser from Device code/Dongle_Binary/mesq_hub_core.h against it. If
  // the two encoders ever diverge, one of the two suites fails.
  ok(h === 'aa55fc010001', 'reset frame matches the C encoder byte for byte');
  ok(f[0] === 0xAA && f[1] === 0x55 && f[2] === 0xFC, 'sync word plus the 0xFC host-command marker');
  ok(f[f.length - 1] === (f[3] ^ f[4]), 'trailing byte is the xor over cmd and len');

  const g = P.encodeCommand(0x05, [1, 2, 3]);
  ok(g.length === 9, 'a 3-byte payload gives a 9-byte frame');
  ok(g[g.length - 1] === (0x05 ^ 3 ^ 1 ^ 2 ^ 3), 'checksum covers the payload too');

  let threw = false;
  try { P.encodeCommand(1, new Array(17).fill(0)); } catch (e) { threw = true; }
  ok(threw, 'an over-long payload is refused at the encoder, not truncated silently');

  // The old call site sent the literal text "reboot". Confirm that text is
  // NOT a valid frame, i.e. the new hub would correctly ignore it.
  const oldWay = ascii('reboot');
  ok(!(oldWay[0] === 0xAA && oldWay[1] === 0x55), 'the legacy "reboot" text is not a valid command frame');
}

/* ===================================================================== */
console.log('\n' + '='.repeat(70));
console.log('pass=' + pass + '  fail=' + fail);
console.log('='.repeat(70));
process.exit(fail ? 1 : 0);
