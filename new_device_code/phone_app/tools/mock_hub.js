#!/usr/bin/env node
/* =========================================================================
   MOCK HUB  --  new_device_code/phone_app/tools/mock_hub.js

   WHY THIS FILE EXISTS
   To test the phone app without the suit. It stands in for the hub's phone
   path and for the page's parser, and reports exactly what the page would
   receive:

     phone --ws text frame--> [hub rule] --line + '\n'--> js/mesq_parser.js
                                                            |
                              counts, rate, last pose  <----+

   The hub rule is Dongle_Binary.ino handleWebSocketMessage(): only a
   complete, unfragmented TEXT frame is forwarded, and only if it fits a
   96-byte USB record with the '\n' the hub adds (MESQ_USB_REC_MAX in
   mesq_hub_core.h). Everything else is dropped silently by the real hub;
   here every drop is counted by reason. `--hub json` applies the older
   JSON dongle's rule instead (Device code/Dongle__JSON: no length limit).

   What is accepted goes through the page's own parser module, so a line
   that passes here is a line the page's handleWSMessage() would get.

   USAGE (Node 18 or later, no packages needed)
     node mock_hub.js                       listen on :8765/ws, print stats
     node mock_hub.js --port 8765 --out capture.jsonl
     node mock_hub.js --replay take.jsonl   offline: check a scenario file
     node mock_hub.js --self-test           check the mock itself
   In the app: Settings, Hub, ws://<this computer's address>:8765/ws, with
   the phone and the computer on the same Wi-Fi.
   ========================================================================= */
'use strict';

const http = require('http');
const crypto = require('crypto');
const fs = require('fs');
const net = require('net');
const os = require('os');
const path = require('path');

const REPO = path.resolve(__dirname, '..', '..', '..');
const MesqParser = require(path.join(REPO, 'js', 'mesq_parser.js'));

// PROTOCOL: mesq_hub_core.h MESQ_USB_REC_MAX. The hub appends '\n', so a
// phone message may be at most 95 bytes.
const MESQ_USB_REC_MAX = 96;
const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

function parseArgs(argv) {
  const a = { port: 8765, host: '0.0.0.0', path: '/ws', hub: 'binary', quiet: false };
  for (let i = 0; i < argv.length; i++) {
    const k = argv[i];
    const next = () => argv[++i];
    if (k === '--port') a.port = parseInt(next(), 10);
    else if (k === '--host') a.host = next();
    else if (k === '--path') a.path = next();
    else if (k === '--hub') a.hub = next();
    else if (k === '--out') a.out = next();
    else if (k === '--report') a.report = next();
    else if (k === '--expect') a.expect = next();
    else if (k === '--idle-exit') a.idleExit = parseInt(next(), 10);
    else if (k === '--replay') a.replay = next();
    else if (k === '--self-test') a.selfTest = true;
    else if (k === '--quiet') a.quiet = true;
    else { console.error('unknown option ' + k); process.exit(2); }
  }
  if (a.hub !== 'binary' && a.hub !== 'json') { console.error('--hub is binary or json'); process.exit(2); }
  return a;
}

/* -----------------------------------------------------------------------
   The page side: one parser, as js/webserialnative.js creates it, plus the
   page's position guard (js/custom_icm.js:931, `obj.px && obj.py && obj.pz`),
   which drops a frame with a coordinate of exactly 0.
   ----------------------------------------------------------------------- */
function createPage() {
  const page = {
    objects: 0, guardDrops: 0, orientationOnly: 0, drops: {}, last: null, lines: [],
    parser: null,
  };
  page.parser = MesqParser.createParser({
    onJsonObject(j) {
      page.objects++;
      page.last = j;
      if (j.bone === 'Hips') {
        const hasPos = j.px !== undefined || j.py !== undefined || j.pz !== undefined;
        if (!hasPos) page.orientationOnly++;
        else if (!(j.px && j.py && j.pz)) page.guardDrops++;
      }
    },
    onPodFrame() {},
    onStatusLine() {},
    onDrop(reason, n) { page.drops[reason] = (page.drops[reason] || 0) + n; },
  });
  return page;
}

/* -----------------------------------------------------------------------
   The hub's rule for one complete WebSocket frame. Returns null when the
   frame is forwarded, or the reason the real hub would drop it.
   ----------------------------------------------------------------------- */
function hubRefuses(frame, hubKind) {
  if (frame.opcode === 0x0 || !frame.fin) return 'fragmented';     // info->final, info->index == 0
  if (frame.split) return 'split_across_tcp_reads';                // info->len == len
  if (frame.opcode === 0x2) return 'binary';                       // info->opcode == WS_TEXT
  if (frame.opcode !== 0x1) return 'not_text';
  if (hubKind === 'binary' && frame.payload.length + 1 > MESQ_USB_REC_MAX) return 'oversize';
  return null;
}

/* -----------------------------------------------------------------------
   A minimal RFC 6455 server side: handshake, unmasking, ping/pong, close.
   ----------------------------------------------------------------------- */
function acceptKey(key) {
  return crypto.createHash('sha1').update(key + WS_GUID).digest('base64');
}

function serverFrame(opcode, payload) {
  const len = payload.length;
  let head;
  if (len < 126) head = Buffer.from([0x80 | opcode, len]);
  else { head = Buffer.alloc(4); head[0] = 0x80 | opcode; head[1] = 126; head.writeUInt16BE(len, 2); }
  return Buffer.concat([head, payload]);
}

function frameReader(onFrame, onError) {
  let buf = Buffer.alloc(0);
  let chunk = 0;
  let pendingFrom = -1;       // the read in which an unfinished frame began
  return function feed(data) {
    chunk++;
    buf = buf.length === 0 ? data : Buffer.concat([buf, data]);
    for (;;) {
      if (buf.length === 0) { pendingFrom = -1; return; }
      if (pendingFrom < 0) pendingFrom = chunk;
      if (buf.length < 2) return;
      const b0 = buf[0], b1 = buf[1];
      const masked = (b1 & 0x80) !== 0;
      let len = b1 & 0x7f, off = 2;
      if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) {
        if (buf.length < 10) return;
        const big = buf.readBigUInt64BE(2);
        if (big > 1000000n) { onError('frame too large'); return; }
        len = Number(big); off = 10;
      }
      const maskOff = off;
      if (masked) off += 4;
      if (buf.length < off + len) return;
      const payload = Buffer.from(buf.subarray(off, off + len));
      if (masked) for (let i = 0; i < len; i++) payload[i] ^= buf[maskOff + (i & 3)];
      buf = buf.subarray(off + len);
      // A frame that began in an earlier read reached the server in pieces.
      // AsyncWebSocket hands such a frame over in parts (info->index > 0 or
      // info->len != len), and the hub's rule drops every part.
      const split = pendingFrom < chunk;
      pendingFrom = -1;
      onFrame({ fin: (b0 & 0x80) !== 0, opcode: b0 & 0x0f, masked, payload, split });
    }
  };
}

function createHub(opts) {
  const page = createPage();
  const s = {
    connections: 0, open: 0, frames: 0, accepted: 0, refused: {}, unmasked: 0, pings: 0,
    bytesMax: 0, bytesSum: 0, recent: [], started: Date.now(), lastMessage: 0,
    accepted_texts: [],
  };
  const out = opts.out ? fs.createWriteStream(opts.out, { flags: 'a' }) : null;

  function takeText(text, t) {
    s.accepted++;
    s.accepted_texts.push(text);
    s.bytesSum += Buffer.byteLength(text);
    s.bytesMax = Math.max(s.bytesMax, Buffer.byteLength(text));
    s.recent.push(t);
    s.lastMessage = t;
    const before = page.objects;
    // The hub appends '\n' and the bytes go down the USB stream.
    page.parser.feed(Buffer.from(text + '\n', 'utf8'));
    if (out) out.write(JSON.stringify({ t_ms: t, bytes: Buffer.byteLength(text), line: text,
                                        parsed: page.objects > before }) + '\n');
  }

  function handleFrame(frame, socket) {
    s.frames++;
    if (!frame.masked) { s.unmasked++; socket.destroy(); return; }       // RFC 6455 5.1
    if (frame.opcode === 0x8) { socket.end(serverFrame(0x8, frame.payload.subarray(0, 2))); return; }
    if (frame.opcode === 0x9) { s.pings++; socket.write(serverFrame(0xA, frame.payload)); return; }
    if (frame.opcode === 0xA) return;
    const why = hubRefuses(frame, opts.hub);
    if (why) { s.refused[why] = (s.refused[why] || 0) + 1; return; }
    takeText(frame.payload.toString('utf8'), Date.now());
  }

  const server = http.createServer((req, res) => {
    res.writeHead(426, { 'Content-Type': 'text/plain' });
    res.end('Mesquite mock hub: WebSocket only, at ' + opts.path + '\n');
  });
  server.on('upgrade', (req, socket) => {
    const key = req.headers['sec-websocket-key'];
    const up = String(req.headers.upgrade || '').toLowerCase();
    if (req.url !== opts.path || up !== 'websocket' || !key || req.headers['sec-websocket-version'] !== '13') {
      socket.end('HTTP/1.1 400 Bad Request\r\n\r\n');
      return;
    }
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
                 'Sec-WebSocket-Accept: ' + acceptKey(key) + '\r\n\r\n');
    socket.setNoDelay(true);
    s.connections++;
    s.open++;
    if (!opts.quiet) console.log(`[hub] connect #${s.connections} from ${socket.remoteAddress}`);
    const feed = frameReader(f => handleFrame(f, socket), why => { console.log('[hub] ' + why); socket.destroy(); });
    socket.on('data', feed);
    // The phone closing its side (FIN) ends the connection; the upgraded
    // socket would otherwise stay half-open and never emit 'close'.
    socket.on('end', () => socket.end());
    socket.on('close', () => { s.open--; if (!opts.quiet) console.log('[hub] disconnect'); });
    socket.on('error', () => {});
  });
  return { server, page, stats: s, takeText, close() { if (out) out.end(); } };
}

function summary(hub) {
  const s = hub.stats, p = hub.page;
  return {
    connections: s.connections, frames: s.frames, accepted: s.accepted, refused: s.refused,
    unmasked: s.unmasked, pings: s.pings,
    bytes_max: s.bytesMax, bytes_mean: s.accepted ? +(s.bytesSum / s.accepted).toFixed(1) : 0,
    parser: {
      json_lines: p.parser.stats.jsonLines, parse_errors: p.parser.stats.jsonParseErr,
      nan_frames: p.parser.stats.nanFrames, overflow: p.parser.stats.jsonOverflow, drops: p.drops,
    },
    page: { objects: p.objects, orientation_only: p.orientationOnly, position_guard_drops: p.guardDrops },
  };
}

function lanAddresses() {
  const out = [];
  for (const list of Object.values(os.networkInterfaces())) {
    for (const a of list || []) if (a.family === 'IPv4' && !a.internal) out.push(a.address);
  }
  return out;
}

function printLine(hub) {
  const s = hub.stats, now = Date.now();
  s.recent = s.recent.filter(t => t > now - 1000);
  const last = hub.page.last;
  const pose = last ? `q(${last.x},${last.y},${last.z},${last.w})` +
    (last.px !== undefined ? ` p(${last.px},${last.py},${last.pz}) m` : ' p(held)') : '-';
  const ref = Object.entries(s.refused).map(([k, v]) => `${k} ${v}`).join(', ') || 'none';
  console.log(`[hub] ${s.recent.length}/s  accepted ${s.accepted}  refused ${ref}  ` +
              `max ${s.bytesMax} B  page objects ${hub.page.objects}  guard drops ${hub.page.guardDrops}  ${pose}`);
}

function finish(hub, opts) {
  const sum = summary(hub);
  let code = 0;
  if (opts.expect) {
    const want = fs.readFileSync(opts.expect, 'utf8').split('\n').filter(l => l.length);
    const got = hub.stats.accepted_texts;
    let firstDiff = -1;
    for (let i = 0; i < Math.max(want.length, got.length); i++) if (want[i] !== got[i]) { firstDiff = i; break; }
    sum.expect = { sent: want.length, accepted: got.length, identical: firstDiff < 0, first_difference: firstDiff };
    if (firstDiff >= 0 || sum.parser.json_lines !== want.length || sum.page.position_guard_drops) code = 1;
  }
  if (opts.report) fs.writeFileSync(opts.report, JSON.stringify(sum, null, 2) + '\n');
  console.log(JSON.stringify(sum, null, 2));
  hub.close();
  return code;
}

/* -----------------------------------------------------------------------
   --replay: a scenario file from the app's checks (MESQ_SCENARIO_DIR), or a
   file of raw lines. No network: each line is checked against the hub rule
   and fed to the parser, and the same is done for the "naive" variant (the
   message without the zero nudge) to show what the page would have dropped.
   ----------------------------------------------------------------------- */
function replay(file, opts) {
  const lines = fs.readFileSync(file, 'utf8').split('\n').filter(l => l.length);
  const hub = createHub({ hub: opts.hub, quiet: true });
  const naive = createPage();
  let naiveRefused = 0;
  for (const raw of lines) {
    let text = raw, alt = null;
    // A scenario record carries "truth"; frames that sent nothing have no "wire".
    const rec = raw.includes('"truth"') ? JSON.parse(raw) : null;
    if (rec) {
      text = rec.wire;
      alt = rec.naive;
      if (text == null) continue;
    }
    const frame = { fin: true, opcode: 1, masked: true, payload: Buffer.from(text, 'utf8') };
    const why = hubRefuses(frame, opts.hub);
    if (why) hub.stats.refused[why] = (hub.stats.refused[why] || 0) + 1;
    else hub.takeText(text, 0);
    if (alt) {
      if (hubRefuses({ fin: true, opcode: 1, payload: Buffer.from(alt) }, opts.hub)) naiveRefused++;
      else naive.parser.feed(Buffer.from(alt + '\n'));
    }
  }
  const sum = summary(hub);
  sum.naive_without_zero_nudge = { position_guard_drops: naive.guardDrops, refused: naiveRefused };
  console.log(JSON.stringify(sum, null, 2));
  const bad = Object.keys(sum.refused).length || sum.parser.parse_errors || sum.parser.nan_frames ||
              sum.page.position_guard_drops || sum.parser.json_lines !== sum.accepted;
  return bad ? 1 : 0;
}

/* -----------------------------------------------------------------------
   --self-test: drive the mock with a raw socket client and check each rule.
   ----------------------------------------------------------------------- */
function clientFrame(opcode, payload, fin = true) {
  const mask = crypto.randomBytes(4);
  const p = Buffer.from(payload);
  for (let i = 0; i < p.length; i++) p[i] ^= mask[i & 3];
  const head = p.length < 126 ? Buffer.from([(fin ? 0x80 : 0) | opcode, 0x80 | p.length])
                              : Buffer.from([(fin ? 0x80 : 0) | opcode, 0x80 | 126, p.length >> 8, p.length & 255]);
  return Buffer.concat([head, mask, p]);
}

function selfTest() {
  const hub = createHub({ port: 0, path: '/ws', hub: 'binary', quiet: true });
  // Exactly 95 and 96 bytes: the largest message the hub forwards, and the
  // smallest it drops.
  const ok95 = '{"bone":"Hips","x":-0.123,"y":-0.123,"z":-0.123,"w":0.9771,"px":-1.234,"py":-1.002,"pz":-1.234}';
  const too96 = ok95.replace('"w":0.9771', '"w":0.97712');
  if (Buffer.byteLength(ok95) !== 95 || Buffer.byteLength(too96) !== 96) {
    console.log(`  FAIL  test strings are ${Buffer.byteLength(ok95)} and ${Buffer.byteLength(too96)} bytes`);
    return Promise.resolve(1);
  }
  return new Promise(resolve => {
    hub.server.listen(0, '127.0.0.1', () => {
      const port = hub.server.address().port;
      const sock = net.connect(port, '127.0.0.1');
      let handshake = '', pong = false;
      sock.on('data', d => {
        if (!handshake.includes('\r\n\r\n')) {
          handshake += d.toString('latin1');
          if (!handshake.includes('\r\n\r\n')) return;
          sock.write(clientFrame(1, ok95));                       // accepted
          sock.write(clientFrame(1, too96));                      // oversize
          sock.write(clientFrame(1, '{"bone":"Hi', false));       // fragmented ...
          sock.write(clientFrame(0, 'ps","x":0,"y":0,"z":0,"w":1}'));
          sock.write(clientFrame(2, ok95));                       // binary
          sock.write(clientFrame(9, 'hi'));                       // ping -> pong
          setTimeout(() => sock.end(clientFrame(8, '')), 150);
          return;
        }
        if (d[0] === 0x8A) pong = true;
      });
      sock.on('close', () => {
        const s = hub.stats, p = hub.page;
        const checks = [
          ['handshake answered 101 with the right accept key', /101 Switching/.test(handshake)],
          ['a 95-byte text message is accepted', s.accepted === 1],
          ['a 96-byte message is refused as oversize', s.refused.oversize === 1],
          ['a fragmented message is refused', s.refused.fragmented === 2],
          ['a binary frame is refused', s.refused.binary === 1],
          ['a ping is answered with a pong', pong],
          ['the accepted line reaches the page parser', p.parser.stats.jsonLines === 1 && p.objects === 1],
        ];
        let failed = 0;
        for (const [name, good] of checks) { console.log(`  ${good ? 'ok  ' : 'FAIL'}  ${name}`); if (!good) failed++; }
        hub.server.close();
        resolve(failed ? 1 : 0);
      });
      sock.write(`GET /ws HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
                 `Sec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\n\r\n`);
    });
  });
}

/* ----------------------------------------------------------------------- main */
async function main() {
  const opts = parseArgs(process.argv.slice(2));
  if (opts.selfTest) { console.log('mock hub self-test'); process.exit(await selfTest()); }
  if (opts.replay) process.exit(replay(opts.replay, opts));

  const hub = createHub(opts);
  hub.server.listen(opts.port, opts.host, () => {
    const port = hub.server.address().port;
    console.log(`[hub] mock hub (${opts.hub} rule, messages up to ${opts.hub === 'binary' ? 95 : 'any'} bytes)`);
    console.log(`[hub] listening on port ${port}, path ${opts.path}`);
    for (const ip of lanAddresses()) console.log(`[hub]   in the app: ws://${ip}:${port}${opts.path}`);
  });
  const tick = setInterval(() => {
    const s = hub.stats;
    if (!opts.quiet && s.connections) printLine(hub);
    // --idle-exit: stop once messages have come and then stopped, or after
    // 60 s if none ever came, so a script waiting on the mock cannot hang.
    const idle = s.accepted ? Date.now() - s.lastMessage > opts.idleExit : Date.now() - s.started > 60000;
    if (opts.idleExit && idle) {
      clearInterval(tick);
      hub.server.close();
      process.exit(finish(hub, opts));
    }
  }, 1000);
  process.on('SIGINT', () => { clearInterval(tick); process.exit(finish(hub, opts)); });
}

main();
