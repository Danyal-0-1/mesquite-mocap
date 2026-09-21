#!/usr/bin/env node
/* =========================================================================
   BVH EXPORT ACCEPTANCE  --  tools/export_tests.js

   BVH-01 / SYNC-07 / WEB-03 / EST-05. Phase 2 landed the measured-frame-time
   branch in js/bvh_converter.js but nothing populated window.mesqFrameTiming,
   so every export still asserted 1/30 s. Phase 3 wired it in index.html.
   These tests pin the behaviour so it cannot silently regress to a constant.

   Run:  node tools/export_tests.js
   ========================================================================= */
'use strict';
const fs = require('fs'), path = require('path'), vm = require('vm');

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) { pass++; console.log('  PASS  ' + m); } else { fail++; console.log('  FAIL  ' + m); } };
const section = t => console.log('\n[' + t + ']');

// Load bvh_converter.js with the minimum surface it touches. generateBVH()
// itself needs no THREE; only quaternionToEulerDegrees does.
function loadConverter(frameTiming) {
  const ctx = {
    console: { log(){}, error: console.error, warn: console.warn }, THREE: { Euler: function () { this.setFromQuaternion = () => this; } },
    KalmanFilter: function () { this.filter = v => v; },
    model: null, recording: false, recordedMotionData: [],
    numFramesrecorded: 0
  };
  ctx.window = ctx;
  if (frameTiming) ctx.mesqFrameTiming = frameTiming;
  vm.createContext(ctx);
  vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'js', 'bvh_converter.js'), 'utf8'), ctx);
  return ctx;
}

const jointInfo = [
  { name: 'Hips',      position: [0, 10, 0], rotation: [0, 0, 0], level: 0 },
  { name: 'Spine',     position: [0,  5, 0], rotation: [0, 0, 0], level: 1 },
  { name: 'LeftUpLeg', position: [1, -5, 0], rotation: [0, 0, 0], level: 1 },
];
const motion = (n) => Array.from({ length: n }, () => jointInfo.map(
  j => ({ name: j.name, position: j.position, rotation: [1, 2, 3] })));

const frameTimeOf = (bvh) => parseFloat(/Frame Time: ([0-9.eE+-]+)/.exec(bvh)[1]);
const sourceOf    = (bvh) => /; MESQ_FRAME_TIME_SOURCE (\S+)/.exec(bvh)[1];

section('E1  measured frame time is used when the recorder supplies one');
{
  // 600 frames spanning 18.75 s -> 31.25 ms/frame, i.e. the pod's real 32 Hz
  // cadence, NOT the 33.33 ms that 1/30 asserts.
  const frames = 600, spanMs = 31.25 * (frames - 1);
  const ctx = loadConverter({ frames, startMs: 1000, endMs: 1000 + spanMs,
                              startISO: '2026-09-20T10:00:00.000Z',
                              endISO:   '2026-09-20T10:00:18.719Z' });
  const bvh = ctx.generateBVH(jointInfo, motion(frames));
  const ft = frameTimeOf(bvh);
  ok(Math.abs(ft - 0.03125) < 1e-9, 'Frame Time = measured 0.03125 s, not 0.03333 (got ' + ft + ')');
  ok(sourceOf(bvh) === 'measured', 'provenance line says "measured"');
  ok(/Frames: 600/.test(bvh), 'frame count matches the recording');
  ok(bvh.includes('; MESQ_CAPTURE_START 2026-09-20T10:00:00.000Z'), 'wall-clock start anchor present');

  // The whole point: declared duration must equal real duration.
  const declared = ft * frames;
  ok(Math.abs(declared - (spanMs + 31.25) / 1000) < 1e-6,
     'declared BVH duration matches captured duration (' + declared.toFixed(4) + ' s)');
}

section('E2  the 1/30 fallback is still available BUT is labelled untrustworthy');
{
  const ctx = loadConverter(null);
  const bvh = ctx.generateBVH(jointInfo, motion(100));
  ok(Math.abs(frameTimeOf(bvh) - 1 / 30) < 1e-9, 'falls back to 1/30 when no timing was recorded');
  ok(sourceOf(bvh) === 'ASSUMED_1_30_UNTRUSTWORTHY',
     'and says so in the file, so a reader cannot mistake it for a measurement');
}

section('E3  the ~7% dilation the fallback caused, quantified');
{
  const frames = 1000, realDt = 0.03125;
  const realDuration = realDt * frames;              // 31.25 s
  const assertedDuration = (1 / 30) * frames;        // 33.33 s
  const err = (assertedDuration - realDuration) / realDuration;
  ok(Math.abs(err - 0.0667) < 0.001,
     'asserting 1/30 over 32 Hz data stretches the timeline by ' + (err * 100).toFixed(2) + '%');
  const ctx = loadConverter({ frames, startMs: 0, endMs: realDt * 1000 * (frames - 1) });
  ok(Math.abs(frameTimeOf(ctx.generateBVH(jointInfo, motion(frames))) - realDt) < 1e-9,
     'with I12 wired, that error is zero');
}

section('E4  degenerate timing inputs fall back rather than emitting nonsense');
{
  for (const [name, t] of Object.entries({
    'single frame':      { frames: 1, startMs: 0, endMs: 0 },
    'zero span':         { frames: 100, startMs: 500, endMs: 500 },
    'end before start':  { frames: 100, startMs: 900, endMs: 100 },
    'missing endMs':     { frames: 100, startMs: 0 },
  })) {
    const ctx = loadConverter(t);
    const bvh = ctx.generateBVH(jointInfo, motion(10));
    const ft = frameTimeOf(bvh);
    ok(ft > 0 && isFinite(ft) && sourceOf(bvh) === 'ASSUMED_1_30_UNTRUSTWORTHY',
       '"' + name + '" -> labelled fallback, never a zero or negative Frame Time');
  }
}

section('E5  structural sanity of the emitted file');
{
  const ctx = loadConverter({ frames: 5, startMs: 0, endMs: 125 });
  const bvh = ctx.generateBVH(jointInfo, motion(5));
  ok(bvh.startsWith('HIERARCHY\n'), 'starts with HIERARCHY');
  ok(bvh.includes('ROOT Hips'), 'root joint is Hips');
  ok(bvh.includes('CHANNELS 6 Xposition Yposition Zposition Xrotation Yrotation Zrotation'),
     'root carries 6 channels');
  ok((bvh.match(/CHANNELS 3 /g) || []).length === 2, 'the 2 non-root joints carry 3 channels each');
  const motionIdx = bvh.indexOf('MOTION');
  const dataLines = bvh.slice(motionIdx).split('\n').filter(l => /^[-0-9]/.test(l));
  ok(dataLines.length === 5, 'exactly 5 motion rows for 5 frames');
}

console.log('\n' + '='.repeat(70));
console.log('pass=' + pass + '  fail=' + fail);
console.log('='.repeat(70));
process.exit(fail ? 1 : 0);
