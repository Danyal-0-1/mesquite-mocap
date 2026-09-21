let port = null;

const butConnect = document.getElementById('linkPods');

document.addEventListener('DOMContentLoaded', () => {
  butConnect.addEventListener('click', toggleConnect);

  if (!('serial' in navigator)) {
    M.toast({ html: "Web serial is not supported<a class='btn green black-text' target='_blank' href='https://caniuse.com/web-serial'>Learn more </a>", displayLength: 500000, classes: "red black-text" });

  }

});


// =========================================================================
//  WIRE FORMAT - dual mode
//  The dongle multiplexes two sources onto the same USB serial:
//    1. Pod data    -> 16-byte binary frames, sync 0xAA 0x55 (see Dongle.ino
//                      pod_packet_t and pod_watch.ino).
//    2. Phone (Hips) data -> JSON lines forwarded verbatim from the dongle's
//                      WebSocket handler. Always start with '{' and end '\n'.
//  We feed every incoming byte through one state machine that tells the two
//  apart by leading byte. Either path ultimately calls handleWSMessage()
//  with the same {bone,x,y,z,w,batt,count,millis,...} object shape.
// =========================================================================
const POD_PACKET_LEN = 16;
const SYNC0 = 0xAA;
const SYNC1 = 0x55;

// =========================================================================
//  PHASE 3: the byte-level state machine now lives in js/mesq_parser.js and
//  is shared verbatim with tools/parser_tests.js. Phase 2 kept a hand-copied
//  duplicate in the fixture ("kept in sync manually"), which meant a fix here
//  was not actually covered by any test. There is now one implementation.
//
//  RECOVERY: the parser is bounded on three axes -- max JSON line, max buffer,
//  and a stall timeout -- and treats an 0xAA 0x55 inside a JSON line as proof
//  of a hub-side interleave (HUB-02), abandoning the text line and keeping the
//  binary frame. WEB-02's permanent latch is structurally impossible now; see
//  tools/parser_tests.js T4/T5.
//
//  LEARNING: A1_05_LEARNING_GUIDE.md ch. "Parser state machine".
// =========================================================================
window._hubStatus = [];          // last 120 framed status lines from the hub

// Instrumentation hooks degrade to no-ops if js/mesq_instr.js is not loaded.
const _MI = (typeof MesqInstr !== 'undefined') ? MesqInstr : {
  onPacket(){}, onBytes(){}, onBinaryFrame(){}, onJsonLine(){},
  onJsonLineLen(){}, onPositionGuard(){}, onNanFrame(){}
};
window._MI = _MI;

// MUST match the bone-id table in Pod_Watch_Binary.ino and Dongle_Binary.ino.
// Re-exported from the shared module so there is a single source of truth.
const BONE_NAMES = MesqParser.BONE_NAMES;

// Live bone-id histogram. Inspect from DevTools console:
//   window._podRx        -> { Head: 312, HipsAlt: 309, ... }
//   window._podRxByteId  -> { 0: 312, 2: 309, ... }
// Empty key for HipsAlt means the IMU pod is NOT reaching the browser.
window._podRx = {};
window._podRxByteId = {};

// Mode counters so you can confirm dual-mode parsing in a mixed deployment.
window._rxMode = { binary: 0, json: 0 };

// SYNC-08 / NODE-02: per-node freshness. `millis` is now the pod's clock at
// SAMPLE DECODE, so two packets carrying the same value carry the same sensor
// sample -- the pod had nothing new to send. Counting those separates real
// measurements from held poses, which the export must not conflate.
window._podFresh = {};   // bone -> { fresh, held, lastMs }

window._rxBytes = 0;
window._rxLines = 0;
window._rxLast200 = "";

function _recordTail(chunk) {
  // Cheap printable-only tail buffer. Non-printable bytes become `.`.
  let s = "";
  for (let k = 0; k < chunk.length; k++) {
    const c = chunk[k];
    s += (c >= 0x20 && c < 0x7F) ? String.fromCharCode(c)
       : (c === 0x0A) ? "\n"
       : ".";
  }
  window._rxLast200 = (window._rxLast200 + s).slice(-200);
}

function _dispatch(obj, isJson) {
  try {
    handleWSMessage(obj);
  } catch (e) {
    // Don't fully swallow -- log the first occurrence per bone so a broken
    // bone surfaces in DevTools instead of silently going dark. This is
    // exactly the gap that hid the HipsAlt-not-mapping issue.
    if (typeof window._handleWSErr === 'undefined') window._handleWSErr = {};
    if (!window._handleWSErr[obj.bone]) {
      window._handleWSErr[obj.bone] = true;
      console.error('[mocap] handleWSMessage threw for '
        + (isJson ? 'JSON ' : '') + 'bone "' + obj.bone + '":', e);
    }
  }
}

const _parser = MesqParser.createParser({
  onPodFrame(obj) {
    window._podRxByteId[obj.id] = (window._podRxByteId[obj.id] || 0) + 1;
    window._podRx[obj.bone] = (window._podRx[obj.bone] || 0) + 1;
    window._rxMode.binary++;

    // Freshness classification, before the frame reaches the rig.
    const f = window._podFresh[obj.bone] || { fresh: 0, held: 0, lastMs: -1 };
    if (f.lastMs === obj.millis) { f.held++; obj.held = true; }
    else { f.fresh++; obj.held = false; }
    f.lastMs = obj.millis;
    window._podFresh[obj.bone] = f;

    _MI.onBinaryFrame();                       // I6
    _MI.onPacket(obj.bone, obj.count);         // I5: sequence-gap detection
    _dispatch(obj, false);
  },
  onJsonObject(j) {
    window._rxLines++;
    window._rxMode.json++;
    _MI.onJsonLine();                          // I6
    if (j.bone) window._podRx[j.bone] = (window._podRx[j.bone] || 0) + 1;
    _dispatch(j, true);
  },
  onStatusLine(txt) {
    window._hubStatus.push({ t: Date.now(), line: txt });
    if (window._hubStatus.length > 120) window._hubStatus.shift();
  },
  onDrop(reason, n) {
    window._parserDrops = window._parserDrops || {};
    window._parserDrops[reason] = (window._parserDrops[reason] || 0) + n;
    // Surface the first occurrence of each reason -- these used to be silent.
    window._parserDropSeen = window._parserDropSeen || {};
    if (!window._parserDropSeen[reason]) {
      window._parserDropSeen[reason] = true;
      console.warn('[mocap] parser recovered from "' + reason + '" (' + n + ' bytes). '
        + 'This is the WEB-02/HUB-02 path; see window._parserDrops.');
    }
  }
});
window._parser = _parser;

// One-call diagnostic. Paste `_mocapDebug()` in DevTools to see the full
// pipeline state.
window._mocapDebug = function () {
  const st = _parser.stats;
  const out = {
    bytesReceived:   window._rxBytes,
    linesReceived:   window._rxLines || 0,
    jsonParsedOK:    window._rxMode.json,
    binaryFramesOK:  window._rxMode.binary,
    parseFailures:   st.jsonParseErr,
    nanFramesDropped: st.nanFrames,
    falseSync:       st.falseSync,
    jsonInterleave:  st.jsonInterleave,
    jsonOverflow:    st.jsonOverflow,
    jsonStall:       st.jsonStall,
    bufOverflowBytes: st.bufOverflow,
    pendingBytes:    _parser.pending(),
    lastBadLineRaw:  st.lastBadLine || '(none)',
    lastBadLineFix:  st.lastBadLineRepaired || '(none)',
    perBone:         Object.assign({}, window._podRx),
    perBoneId:       Object.assign({}, window._podRxByteId),
    freshness:       Object.assign({}, window._podFresh),
    handlerErrors:   Object.assign({}, window._handleWSErr || {}),
    hubStatusLast:   (window._hubStatus.slice(-1)[0] || {}).line || '(none)',
    last200chars:    window._rxLast200,
  };
  console.table({
    bytes:  out.bytesReceived,
    lines:  out.linesReceived,
    jsonOK: out.jsonParsedOK,
    binOK:  out.binaryFramesOK,
    fails:  out.parseFailures,
    recovered: st.jsonInterleave + st.jsonOverflow + st.jsonStall,
  });
  console.log(out);
  return out;
};

function feedSerialBytes(chunk) {
  window._rxBytes += chunk.length;
  _MI.onBytes(chunk.length);                       // I6
  _recordTail(chunk);
  _parser.feed(chunk);
}

async function connectToPort(port) {
  port = port || await navigator.serial.requestPort();
  window.port = port;
  if (!port) {
    console.log("No port selected");
    return;
  }

  // Match the client's existing dongle firmware (`Serial.begin(115200)`).
  // ESP32-S3 native USB-CDC ignores baud, so this is essentially just a
  // label, but boards with a real UART bridge (CP2104/CH340) need an exact
  // match or you get garbled bytes. Stick with 115200 to mirror what the
  // deployed dongles use; the binary-mode dongle also opens 115200 unless
  // explicitly bumped.
  var options = { baudRate: 115200 };


  try {
    await port.open(options);
    toggleUIConnected(true);
    $("body").addClass("connected");

    $(".usbstatus").removeClass("red").addClass("green");
    // store port in localStorage
    localStorage.setItem("port", port.getInfo().usbProductId);
    //calibrate();
  } catch (e) {
    console.error(e);
    $(".usbstatus").removeClass("green").addClass("red");
    return;
  }

  // RECOVERY: reset stream state on every fresh connect so a stale partial
  // frame from a previous session cannot poison the parser. Covered by
  // tools/parser_tests.js T14 (disconnect in every parser state).
  _parser.reset();

  while (port && port.readable) {
    const reader = port.readable.getReader();
    try {
      while (true) {
        const { value, done } = await reader.read();
        if (done) {
          console.log("Reader done", done);
          $(".usbstatus").removeClass("green").addClass("red");
          break;
        }
        // value is a Uint8Array; feed it through the dual-mode parser.
        feedSerialBytes(value);
      }
    } catch (e) {
      console.error(e);
      $(".usbstatus").removeClass("green").addClass("red");
    } finally {
      reader.releaseLock();
    }
  }
}

function toggleConnect() {
  if ($("body").hasClass("connected")) {
    window.location.reload();
  } else {
    connectToPort();
  }
}

function writeToPort(data) {
  var port = window.port;
  if (!port || !port.writable) {
    console.error("Port is not writable");
    return;
  }
  const writer = port.writable.getWriter();
  // Accept raw bytes as well as text: commands are binary frames now.
  const bytes = (data instanceof Uint8Array) ? data : new TextEncoder().encode(data);
  writer.write(bytes);
  writer.releaseLock();
}

window.sWrite = function (data) {
  writeToPort(data);
}

// HUB-03: send an explicitly framed, checksummed command.
//
// The old call was `window.sWrite("reboot")`. The word never mattered -- the
// hub reset the fleet on ANY inbound byte, which is why line noise and stray
// terminal traffic could drop the suit mid-capture. The hub now requires a
// complete frame, so the browser builds one.
//
// COMPATIBILITY: against a hub still running Phase 1 firmware the first byte
// of this frame triggers the old reset, giving the same result. The browser
// can therefore be updated before the hubs are.
window.mesqSendCommand = function (cmd, payload) {
  writeToPort(MesqParser.encodeCommand(cmd, payload));
};

// Kept as a named helper so call sites read as intent, not as a magic string.
window.mesqRebootFleet = function () {
  window.mesqSendCommand(MesqParser.CMD_RESET_FLEET);
};

navigator.serial.addEventListener("connect", (e) => {
  console.log("A serial port has been connected to the system: ", e);
  port = e.target;

  // check if port matches the one in localStorage

  //console.log(port.getInfo().usbProductId);
  if (port.getInfo().usbProductId == localStorage.getItem("port")) {
    connectToPort(port);
  }
});

function toggleUIConnected(connected) {
  let lbl = 'Link Pods <i class="material-icons left">settings_ethernet</i>';
  if (connected) {
    lbl = 'Start Over <i class="material-icons right large">refresh</i>';
    $(butConnect).addClass('red white-text').removeClass('white black-text');
    M.Toast.dismissAll();
    M.toast({ html: 'Connected to Dongle', classes: 'green toastheader', displayLength: 500000 });
    M.toast({ html: "<p style='margin-right:20px;margin-top:0;margin-bottom:auto'>Continue with BOX CALIBRATION. Check your pod stats using the PODS button (top-right of the screen).</p><img style='width:80%' src='icons/bc.gif'>", displayLength: 500000, classes: "blue-grey lighten-4 black-text" });
    $("#linkPods").removeClass("animate__pulse animate__infinite");
  }
  else {
    window.location.reload();
  }
  butConnect.innerHTML = lbl;
}


navigator.serial.addEventListener("disconnect", (e) => {
  console.log("A serial port has been disconnected: ", e);
  window.location.reload();
});