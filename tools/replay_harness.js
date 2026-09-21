#!/usr/bin/env node
/* =========================================================================
   MESQUITE PHASE 2 - REPLAY FIXTURE  (system_assessment_2 §4.5)

   PHASE 3 STATUS -- READ THIS FIRST
   This file now pins the PHASE 2 BASELINE on purpose. Its parser copy is
   frozen at the pre-fix algorithm so the WEB-02 / HUB-02 defects stay
   reproducible as evidence. It is NOT the acceptance suite.

     tools/replay_harness.js -> baseline: the defects, reproduced
     tools/parser_tests.js   -> acceptance: js/mesq_parser.js, the parser the
                                browser actually loads

   Section [T10] runs the identical fault bytes through the Phase 3 module so
   the before/after sits in one output.

   Feeds byte streams through the Phase 2 parser logic from
   js/webserialnative.js without a browser or hardware, so that:
     - I5 sequence-gap arithmetic is verifiable
     - WEB-02 (unterminated JSON latches the parser) reproduces ON DEMAND
     - HUB-02 (binary packet interleaved into a JSON line) reproduces ON DEMAND

   Usage:
     node tools/replay_harness.js selftest
     node tools/replay_harness.js replay <rawfile.bin>
   ========================================================================= */
'use strict';
const fs = require('fs');
const path = require('path');

// ---- minimal browser shims the parser expects -------------------------
global.window = global;
global.document = { addEventListener(){}, getElementById(){ return null; } };
try { Object.defineProperty(global,'navigator',{value:{serial:{addEventListener(){}}},configurable:true}); } catch(e){}
global.M = { toast(){} };
global.$ = function(){ return { addClass(){}, fadeIn(){} }; };

const MesqInstr = require(path.join(__dirname, '..', 'js', 'mesq_instr.js'));
global.MesqInstr = MesqInstr;

// ---- the parser under test -------------------------------------------
// Extracted verbatim in structure from js/webserialnative.js:200-302 so the
// fixture tests the real algorithm. Kept in sync manually; see REPORT.md.
const POD_PACKET_LEN = 16, SYNC0 = 0xAA, SYNC1 = 0x55;
const BONE_NAMES = ["Head","Spine","HipsAlt","LeftArm","LeftForeArm","LeftHand",
  "RightArm","RightForeArm","RightHand","LeftUpLeg","LeftLeg","LeftFoot",
  "RightUpLeg","RightLeg","RightFoot","LeftShoulder","RightShoulder"];

let _rxBuf = new Uint8Array(0), _jsonLine = "";
let handled = [];
let statusLines = [];

function reset(){ _rxBuf = new Uint8Array(0); _jsonLine = ""; handled = []; statusLines = []; MesqInstr.reset(); }

function unpack(buf16){
  const dv = new DataView(buf16.buffer, buf16.byteOffset, POD_PACKET_LEN);
  const id = buf16[2], name = BONE_NAMES[id];
  if (!name) return null;
  return { bone:name, batt:buf16[3]/100,
    x:dv.getInt16(4,true)/32767, y:dv.getInt16(6,true)/32767,
    z:dv.getInt16(8,true)/32767, w:dv.getInt16(10,true)/32767,
    count:dv.getUint16(12,true), millis:dv.getUint16(14,true) };
}

function _append(chunk){
  const out = new Uint8Array(_rxBuf.length + chunk.length);
  out.set(_rxBuf,0); out.set(chunk,_rxBuf.length); _rxBuf = out;
}

// PHASE 2 behaviour, frozen. js/webserialnative.js no longer looks like
// this -- it delegates to js/mesq_parser.js. Kept so the defect stays
// demonstrable side by side with the fix (see T10).
function feedSerialBytes(chunk){
  MesqInstr.onBytes(chunk.length);
  _append(chunk);
  let i = 0;
  while (i < _rxBuf.length){
    const b = _rxBuf[i];
    if (b === SYNC0 && _jsonLine.length === 0){
      if (_rxBuf.length - i < 2) break;
      if (_rxBuf[i+1] !== SYNC1){ i += 1; continue; }
      if (_rxBuf.length - i >= 4 && _rxBuf[i+2] === 0xFE){   // framed hub status
        const slen = _rxBuf[i+3];
        if (_rxBuf.length - i < 4 + slen) break;
        let txt=""; for(let k=0;k<slen;k++) txt+=String.fromCharCode(_rxBuf[i+4+k]);
        statusLines.push(txt);
        i += 4 + slen; continue;
      }
      if (_rxBuf.length - i < POD_PACKET_LEN) break;
      const obj = unpack(_rxBuf.subarray(i, i+POD_PACKET_LEN));
      if (obj){ MesqInstr.onBinaryFrame(); MesqInstr.onPacket(obj.bone, obj.count);
                handled.push(obj); }
      i += POD_PACKET_LEN; continue;
    }
    if (b === 0x7B || _jsonLine.length > 0){
      _jsonLine += String.fromCharCode(b);
      MesqInstr.onJsonLineLen(_jsonLine.length);
      if (b === 0x0A){
        try { const j = JSON.parse(_jsonLine.trim()); MesqInstr.onJsonLine();
              handled.push(j); } catch(e){}
        _jsonLine = "";
        MesqInstr.onJsonLineLen(0);
      }
      i += 1; continue;
    }
    i += 1;
  }
  _rxBuf = _rxBuf.subarray(i);
}

// ---- packet builders --------------------------------------------------
function mkPacket(id, count, ms){
  const b = new Uint8Array(16); const dv = new DataView(b.buffer);
  b[0]=SYNC0; b[1]=SYNC1; b[2]=id; b[3]=87;
  dv.setInt16(4,0,true); dv.setInt16(6,0,true);
  dv.setInt16(8,0,true); dv.setInt16(10,32767,true);
  dv.setUint16(12,count & 0xFFFF,true); dv.setUint16(14,(ms||0)&0xFFFF,true);
  return b;
}
const enc = s => Uint8Array.from(Buffer.from(s,'latin1'));

// ---- tests ------------------------------------------------------------
let pass=0, fail=0;
function check(name, cond, detail){
  if (cond){ pass++; console.log('  PASS  ' + name); }
  else { fail++; console.log('  FAIL  ' + name + (detail? '  -> '+detail : '')); }
}

function T_basic(){
  console.log('\n[T1] clean stream, no loss');
  reset();
  for (let c=0;c<100;c++) feedSerialBytes(mkPacket(1,c));
  const s = MesqInstr.snapshot();
  check('100 received', s.bones.Spine.received===100, JSON.stringify(s.bones.Spine));
  check('0 lost', s.bones.Spine.lost===0);
}

function T_chunk_split(){
  console.log('\n[T2] frames straddling chunk boundaries (Phase 1 said parser handles this)');
  reset();
  const all=[]; for(let c=0;c<50;c++) all.push(...mkPacket(1,c));
  const buf=Uint8Array.from(all);
  for(let i=0;i<buf.length;i+=7) feedSerialBytes(buf.subarray(i,Math.min(i+7,buf.length)));
  const s=MesqInstr.snapshot();
  check('50 received across 7-byte chunks', s.bones.Spine.received===50,
        'got '+(s.bones.Spine&&s.bones.Spine.received));
  check('0 lost', s.bones.Spine.lost===0);
}

function T_loss(){
  console.log('\n[T3] I5 detects real loss');
  reset();
  const drop=new Set([10,11,12,40]);
  for(let c=0;c<100;c++) if(!drop.has(c)) feedSerialBytes(mkPacket(1,c));
  const s=MesqInstr.snapshot();
  check('received 96', s.bones.Spine.received===96);
  check('lost 4 (I5 arithmetic correct)', s.bones.Spine.lost===4,
        'lost='+s.bones.Spine.lost);
}

function T_wrap(){
  console.log('\n[T4] I5 across the uint16 counter wrap');
  reset();
  for(let c=65530;c<65536;c++) feedSerialBytes(mkPacket(1,c));
  for(let c=0;c<6;c++) feedSerialBytes(mkPacket(1,c));
  const s=MesqInstr.snapshot();
  check('12 received', s.bones.Spine.received===12);
  check('0 lost across wrap', s.bones.Spine.lost===0, 'lost='+s.bones.Spine.lost);
}

function T_reboot(){
  console.log('\n[T5] pod reboot must NOT register as 65535 lost (the guard)');
  reset();
  for(let c=500;c<520;c++) feedSerialBytes(mkPacket(1,c));
  for(let c=0;c<20;c++)   feedSerialBytes(mkPacket(1,c));   // reboot: count resets
  const s=MesqInstr.snapshot();
  check('40 received', s.bones.Spine.received===40);
  check('resync counted, not loss', s.bones.Spine.resyncs===1,
        'resyncs='+s.bones.Spine.resyncs);
  check('lost stays 0', s.bones.Spine.lost===0, 'lost='+s.bones.Spine.lost);
}

function T_web02(){
  console.log('\n[T6] WEB-02 FAULT INJECTION: unterminated JSON line');
  console.log('     (a) with ordinary traffic - does the latch self-clear?');
  reset();
  for(let c=0;c<10;c++) feedSerialBytes(mkPacket(1,c));
  const before=MesqInstr.snapshot().bones.Spine.received;
  feedSerialBytes(enc('{"bone":"Hips","px":1.0'));      // NO newline
  for(let c=10;c<60;c++) feedSerialBytes(mkPacket(1,c));
  const after=MesqInstr.snapshot().bones.Spine.received;
  const lostToLatch = 50 - (after-before);
  console.log('      sent 50 more; decoded '+(after-before)+'; LOST to the latch: '+lostToLatch);
  check('latch self-clears (0x0A occurs in binary payload)', after>before,
        'after='+after);
  check('but packets ARE lost while latched', lostToLatch>0, 'lost='+lostToLatch);

  console.log('     (b) with 0x0A-free payloads - does it latch permanently?');
  reset();
  // counts chosen so neither count nor ms_lo bytes are 0x0A, and quaternion
  // bytes are 0x00/0x7F/0xFF only.
  const safe=[];
  for(let c=0x0101;safe.length<60;c++){
    const lo=c&0xFF, hi=(c>>8)&0xFF;
    if(lo!==0x0A && hi!==0x0A) safe.push(c);
  }
  for(let k=0;k<10;k++) feedSerialBytes(mkPacket(1,safe[k],0x0101));
  const b2=MesqInstr.snapshot().bones.Spine.received;
  feedSerialBytes(enc('{"bone":"Hips","px":1.0'));
  for(let k=10;k<60;k++) feedSerialBytes(mkPacket(1,safe[k],0x0101));
  const a2=MesqInstr.snapshot().bones.Spine.received;
  console.log('      decoded before='+b2+'  after sending 50 more='+a2
              +'   _jsonLine len='+_jsonLine.length);
  check('REPRODUCED: permanent latch when no 0x0A appears', a2===b2,
        'expected '+b2+', got '+a2);
  check('_jsonLine grows unbounded', _jsonLine.length>500, 'len='+_jsonLine.length);

  // how long does the latch typically last with random payloads?
  console.log('     (c) expected latch duration with random payload bytes');
  let tot=0, trials=2000;
  for(let t=0;t<trials;t++){
    let n=0;
    for(;;){ n++; let hit=false;
      for(let j=0;j<16;j++) if(Math.floor(Math.random()*256)===0x0A){hit=true;break;}
      if(hit) break; if(n>500) break; }
    tot+=n;
  }
  const mean=tot/trials;
  console.log('      mean packets until a 0x0A clears the latch: '+mean.toFixed(1)
              +'  (~'+(mean/32*1000).toFixed(0)+' ms at 32 Hz per pod)');
}

function T_hub02(){
  console.log('\n[T7] HUB-02 FAULT INJECTION: binary packet spliced into a JSON line');
  reset();
  const pkt=mkPacket(1,7);
  const line=enc('{"bone":"Hips","px":1.0,"py":2.0,"pz":3.0}\n');
  const mid=Math.floor(line.length/2);
  // hub writes half a JSON line, then an ESP-NOW callback interleaves a packet
  feedSerialBytes(line.subarray(0,mid));
  feedSerialBytes(pkt);
  feedSerialBytes(line.subarray(mid));
  const s=MesqInstr.snapshot();
  const gotBinary=s.stream.binaryFrames, gotJson=s.stream.jsonLines;
  console.log('      binary frames decoded: '+gotBinary+'   json lines decoded: '+gotJson);
  check('REPRODUCED: interleaved binary packet is swallowed', gotBinary===0,
        'binaryFrames='+gotBinary);
  check('and the JSON line is corrupted too', gotJson===0, 'jsonLines='+gotJson);
}

function T_status(){
  console.log('\n[T9] Phase 2 framed hub status (0xFE) must not corrupt pod framing');
  reset();
  const mk = (txt) => {
    const p=Buffer.from(txt,'latin1');
    return Uint8Array.from([SYNC0,SYNC1,0xFE,p.length,...p]);
  };
  for(let c=0;c<5;c++) feedSerialBytes(mkPacket(1,c));
  feedSerialBytes(mk('I1 rx=12,13,11 drop=0/0/0 wr_us(mean/max)=8/41 sta=1 heap=180000'));
  for(let c=5;c<15;c++) feedSerialBytes(mkPacket(1,c));
  const s=MesqInstr.snapshot();
  check('15 pod packets still decoded around the status frame',
        s.bones.Spine.received===15, 'got '+s.bones.Spine.received);
  check('0 lost (framing intact)', s.bones.Spine.lost===0, 'lost='+s.bones.Spine.lost);
  check('status line captured', statusLines.length===1, 'n='+statusLines.length);

  console.log('     status frame split across chunk boundaries');
  reset();
  const f=mk('I2 rssi=-51,-49 batt=87,91');
  for(let c=0;c<3;c++) feedSerialBytes(mkPacket(1,c));
  for(let k=0;k<f.length;k+=3) feedSerialBytes(f.subarray(k,Math.min(k+3,f.length)));
  for(let c=3;c<9;c++) feedSerialBytes(mkPacket(1,c));
  const s2=MesqInstr.snapshot();
  check('9 packets decoded across a split status frame',
        s2.bones.Spine.received===9, 'got '+s2.bones.Spine.received);
  check('status reassembled', statusLines.length===1 && statusLines[0].indexOf('rssi')>=0);
}

function T_falsesync(){
  console.log('\n[T8] 0xAA 0x55 cannot appear in ASCII JSON (false-sync check)');
  const line='{"bone":"Hips","px":-1.0,"py":0.55,"pz":170.0}\n';
  let has=false;
  for(let i=0;i<line.length-1;i++)
    if(line.charCodeAt(i)===0xAA && line.charCodeAt(i+1)===0x55) has=true;
  check('no false sync possible in ASCII text', !has);
}

if (process.argv[2]==='replay'){
  const f=process.argv[3];
  if(!f){ console.error('usage: replay <rawfile.bin>'); process.exit(2); }
  reset();
  const buf=new Uint8Array(fs.readFileSync(f));
  const CH=256;
  for(let i=0;i<buf.length;i+=CH) feedSerialBytes(buf.subarray(i,Math.min(i+CH,buf.length)));
  console.log(JSON.stringify(MesqInstr.snapshot(),null,2));
} else {
  console.log('='.repeat(70));
  console.log('MESQUITE REPLAY FIXTURE - parser + I5 verification, no hardware');
/* =====================================================================
   [T10] PHASE 3 COMPARISON -- identical fault bytes, shipped parser
   The two faults above are replayed through js/mesq_parser.js, the module
   js/webserialnative.js loads in the browser. This is the before/after the
   change report cites.
   ===================================================================== */
function T_phase3(){
  console.log('\n[T10] Phase 3 module vs the same fault bytes');
  const MP = require(path.join(__dirname, '..', 'js', 'mesq_parser.js'));

  // --- WEB-02: unterminated JSON, then 50 frames with no 0x0A in payload ---
  let pods = 0;
  const p1 = MP.createParser({ onPodFrame: () => pods++ });
  p1.feed(enc('{"bone":"Hips","x":0.1'));
  for (let n = 0; n < 50; n++) {
    p1.feed(MP.encodePodPacket({ id:1, batt:50, x:0, y:0, z:0, w:1,
                                 count:0x0101, millis:0x0202 }));
  }
  check('WEB-02: baseline lost every frame, Phase 3 decodes all 50',
        pods === 50, 'got ' + pods);
  check('WEB-02: Phase 3 pending buffer bounded',
        p1.pending() < MP.MAX_JSON_LINE, p1.pending() + ' B');

  // --- HUB-02: binary frame spliced into the middle of a JSON line ---
  let pods2 = 0, jsons2 = 0;
  const p2 = MP.createParser({ onPodFrame: () => pods2++, onJsonObject: () => jsons2++ });
  p2.feed(enc('{"bone":"Hips","x":0.5,'));
  p2.feed(MP.encodePodPacket({ id:9, batt:50, x:0, y:0, z:0, w:1, count:42, millis:7 }));
  p2.feed(enc('"y":0.5,"z":0,"w":1}\n'));
  check('HUB-02: baseline destroyed both records, Phase 3 keeps the pod frame',
        pods2 === 1, 'got ' + pods2);
  check('HUB-02: the corrupted JSON line is still discarded, not mis-parsed',
        jsons2 === 0, 'got ' + jsons2);
}

  console.log('='.repeat(70));
  T_basic(); T_chunk_split(); T_loss(); T_wrap(); T_reboot();
  T_web02(); T_hub02(); T_status(); T_falsesync();
  T_phase3();
  console.log('\n' + '='.repeat(70));
  console.log('pass=' + pass + '  fail=' + fail);
  console.log('='.repeat(70));
  process.exit(fail ? 1 : 0);
}
