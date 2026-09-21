// ============================================================
//  MESQUITE DONGLE — T-Display-S3 (DISPLAY DISABLED)
//  Hardware : LilyGo T-Display-S3 (screen left dark, board still used)
//  Core     : ESP32 Arduino core 3.x (IDF 5.x)
//
//  LIBRARIES REQUIRED:
//    - ESPAsyncWebServer  (by mathieucarbou)
//    - AsyncTCP           (by mathieucarbou)
//
//  ARDUINO IDE SETTINGS (critical):
//    Board               : LilyGo T-Display-S3
//    USB CDC On Boot     : Enabled
//    USB Mode            : Hardware CDC and JTAG
//    PSRAM               : OPI PSRAM
//    Flash Size          : 16MB (128Mb)
//    Partition Scheme    : 16M Flash (3MB APP/9.9MB FATFS)
//    Upload Mode         : UART0 / Hardware CDC
//    Arduino Runs On     : Core 1
//    Events Run On       : Core 1
//
//  MAC NOTE:
//    WiFi.macAddress() returns the STA MAC of this ESP32-S3.
//    In WIFI_AP_STA mode the AP MAC = STA MAC + 1.
//    Use the STA MAC printed at boot when registering this
//    dongle as a peer in your pod firmware.
// ============================================================

#include "Arduino.h"
#include <esp_now.h>
#include <esp_system.h>   // Phase 2 W0: esp_reset_reason(), esp_get_idf_version()
#include <esp_wifi.h>     // for esp_wifi_set_channel / set_ps / set_max_tx_power
#include "WiFi.h"
#include "ESPAsyncWebServer.h"
#include "AsyncTCP.h"

// Phase 3 hub core: command framer, single-owner USB queue, per-node
// integrity counters. Also compiles on a desktop with -DMESQ_HOST_TEST so
// tools/firmware_tests.cpp exercises the queue under two real producer
// threads and the framer against 100k random bytes.
#include "mesq_hub_core.h"

// ============================================================
//  RADIO CONFIG -- MUST MATCH THE POD FIRMWARE
//  ESP-NOW only delivers packets when sender and receiver are on the same
//  WiFi channel. The original code left the channel implicit, which meant
//  the dongle's softAP would pick whatever the local 2.4 GHz environment
//  allowed -- it landed on ch 1 in Tempe but could end up on a different
//  channel in Boston, silently breaking the link.
//
//  Pinning to ch 1 here AND in the pod firmware (ESPNOW_WIFI_CHANNEL)
//  guarantees co-channel operation independent of the deployment site.
// ============================================================
#define ESPNOW_WIFI_CHANNEL 1

// ============================================================
//  PINS  (display-only pins kept as defines but unused)
// ============================================================

#define BTN_MAC      14   // KEY1 — was: hold = show MAC
#define BTN_STATUS    0   // BOOT — was: toggle pod status screen
#define LCD_POWER_ON 15   // was: drive HIGH to power LCD rail

// ============================================================
//  POD CONFIG  (17 pods)
// ============================================================

#define NUM_PODS       17
#define POD_TIMEOUT_MS 5000

// =========================================================================
//  BINARY WIRE FORMAT -- must match Pod_Watch_Binary's mesq_packet.h and
//  js/mesq_parser.js.
//
//  These were declared BELOW the instrumentation block in Phase 2, but
//  mesq_emitStatus() referenced SYNC0/SYNC1 -- and a macro is textual, so it
//  must be defined before it is used. Any -DMESQ_INSTR=1 build of this sketch
//  therefore failed to compile. Phase 2's own report records that nothing was
//  ever compiled (P2-B0-02), which is exactly how this survived. Found by
//  tools/check_sketches.sh.
// =========================================================================
#define POD_PACKET_LEN 16
#define SYNC0          0xAA
#define SYNC1          0x55

// =========================================================================
//  PHASE 2 INSTRUMENTATION (W1) -- observational only.
//  Build with -DMESQ_INSTR=1. Default 0 compiles to nothing.
//
//  ROUTING DECISION (required by §4.4, see 02_hub_instrumentation/REPORT.md):
//  Status output uses a DISTINCT FRAMED MESSAGE TYPE on the same USB stream,
//  not raw text and not the WebSocket.
//    - raw text  -> rejected: HUB-02, and the browser's JSON branch would
//                   swallow following binary packets (measured: WEB-02)
//    - WebSocket -> rejected: that is the phone's path and shares the
//                   corruption problem this is meant to measure
//    - 2nd UART  -> rejected: needs a second cable to every deployment
//  Frame:  [0xAA][0x55][0xFE][len][payload...]   id 0xFE is outside the
//  valid bone range 0..16 and outside the 0xFF control marker, so the
//  existing browser parser skips it harmlessly on old builds.
//
//  I1  per-id rx counts + drop reasons -> HUB-05 (the key measurement)
//  I2  RSSI per id                     -> NET-05, C7 body absorption
//  I3  Serial.write duration           -> does it block in the callback?
//  I7  softAP station count            -> HUB-01, NET-06
//  I11 free heap + ws.count()          -> HUB-04
//  H1  sendReset() invocations         -> HUB-03
//  H2  esp_now_add_peer failures       -> HUB-07
//  H3  batt byte decoded from packet   -> per-node battery for free (§4.4)
// =========================================================================
#ifndef MESQ_INSTR
#define MESQ_INSTR 0
#endif

#define MESQ_STATUS_MARKER 0xFE

// ============================================================
//  HUB SHARED STATE
//  Declared here, above hubEmitStatus(), because that function pushes into
//  g_usb. Order matters in a single translation unit.
// ============================================================

// HUB-02: the single owner of the USB stream. Every producer pushes a whole
// record here; usbWriterTask is the only thing that calls Serial.write().
MesqUsbQueue  g_usb;

// HUB-05 / SYNC-03: per-node integrity, ALWAYS ON (not behind MESQ_INSTR).
// Phase 1's finding was that dropped packets are silent -- a node losing half
// its traffic looked exactly like a healthy one. Two framed status records a
// second is a negligible cost for making that visible during every capture.
MesqNodeStats g_nodeStats[NUM_PODS];

// HUB-03: the host -> hub command parser.
MesqCmdParser g_cmdParser;

// HUB-07: peer registration outcomes, so a failure is not silent.
uint32_t g_peerAddFail = 0;


// Emit one framed status record. ALWAYS AVAILABLE, not instrumentation-only.
//
// PROTOCOL: [0xAA][0x55][0xFE][len][payload...]. 0xFE sits outside the bone
// range 0..16 and outside the 0xFF/0xFC control markers, so it cannot be
// confused with a pose frame. js/mesq_parser.js decodes it explicitly.
//
// WHY this exists at all: the hub has things to tell the operator (a pod
// went quiet, a peer add failed, the USB queue is overflowing) and the only
// cable available is the one already carrying binary poses. Writing that text
// RAW into the stream is HUB-02 -- it splits a pose frame in half. Framing it
// makes hub speech a first-class record instead of corruption.
static void hubEmitStatus(const char *payload) {
    size_t n = strlen(payload);
    if (n > 250) n = 250;
    uint8_t rec[255];
    rec[0] = SYNC0; rec[1] = SYNC1; rec[2] = MESQ_STATUS_MARKER; rec[3] = (uint8_t)n;
    memcpy(rec + 4, payload, n);
    // Through the queue like everything else, so a status line can never
    // interleave with a pose frame.
    mesqUsbPush(&g_usb, rec, (uint8_t)(4 + n));
}

#if MESQ_INSTR
#include <esp_timer.h>

static volatile uint32_t mesq_rx[NUM_PODS]      = {0};
static volatile int32_t  mesq_rssiSum[NUM_PODS] = {0};
static volatile int8_t   mesq_rssiMin[NUM_PODS];
static volatile int8_t   mesq_rssiMax[NUM_PODS];
static volatile uint8_t  mesq_batt[NUM_PODS]    = {0};   // H3
static volatile uint32_t mesq_dropLen = 0, mesq_dropSync = 0, mesq_dropId = 0;
static volatile uint32_t mesq_wrN = 0, mesq_wrMax = 0; static volatile uint64_t mesq_wrSum = 0;
static volatile uint32_t mesq_resetCalls = 0;            // H1
static volatile uint32_t mesq_peerFail = 0;              // H2

// Phase 2's mesq_emitStatus() wrote straight to Serial from the timeout task,
// which is a second writer on the stream -- the very HUB-02 collision it was
// meant to measure. It now goes through the queue like everything else.
#define mesq_emitStatus hubEmitStatus
#endif

const char* const POD_ABBR[NUM_PODS] = {
    "HD",   //  0  Head
    "SPN",  //  1  Spine
    "HIP",  //  2  HipsAlt
    "LUA",  //  3  LeftArm
    "LFA",  //  4  LeftForeArm
    "LH",   //  5  LeftHand
    "RUA",  //  6  RightArm
    "RFA",  //  7  RightForeArm
    "RH",   //  8  RightHand
    "LUL",  //  9  LeftUpLeg
    "LLL",  // 10  LeftLeg
    "LF",   // 11  LeftFoot
    "RUL",  // 12  RightUpLeg
    "RLL",  // 13  RightLeg
    "RF",   // 14  RightFoot
    "LS",   // 15  Left Shoulder
    "RS",   // 16  Right Shoulder
};

// ============================================================
//  SHARED STATE
// ============================================================

volatile bool          podConnected[NUM_PODS] = {false};
volatile unsigned long podLastSeen[NUM_PODS]  = {0};
portMUX_TYPE           stateMux = portMUX_INITIALIZER_UNLOCKED;


// ============================================================
//  DATA STRUCT + PEER MANAGEMENT
// ============================================================

// Receive-side mirror of pod_watch.ino's pod_packet_t. Kept here as a hard
// reference; OnDataRecv does not actually decode fields -- it just validates
// length+sync and forwards the raw bytes to USB serial. The browser unpacks.
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
static_assert(sizeof(pod_packet_t) == POD_PACKET_LEN,
              "pod_packet_t must match POD_PACKET_LEN");

esp_now_peer_info_t peerMacs[NUM_PODS];
bool                peerMacsInit[NUM_PODS] = {false};
esp_now_peer_info_t peerInfo;

// ============================================================
//  WEB SERVER / WEBSOCKET
// ============================================================

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// ============================================================
//  TASK HANDLES
// ============================================================

TaskHandle_t webSocketTaskHandle = NULL;
TaskHandle_t espNowTaskHandle    = NULL;

// Forward declaration so loop() can call sendReset()
void sendReset();

// ============================================================
//  POD TIMEOUT TASK
//  (was the timeout sweep inside displayTask — kept so
//   podConnected[] still flips false after POD_TIMEOUT_MS)
// ============================================================

void podTimeoutTask(void *pvParameters) {
    for (;;) {
        unsigned long now = millis();
        for (int i = 0; i < NUM_PODS; i++) {
            portENTER_CRITICAL(&stateMux);
            bool          wasOn = podConnected[i];
            unsigned long last  = podLastSeen[i];
            portEXIT_CRITICAL(&stateMux);

            if (wasOn && (now - last > POD_TIMEOUT_MS)) {
                portENTER_CRITICAL(&stateMux);
                podConnected[i] = false;
                portEXIT_CRITICAL(&stateMux);
                char b[64];
                snprintf(b, sizeof(b), "POD %s (%d) LOST after %lu ms",
                         POD_ABBR[i], i, (unsigned long)(now - last));
                hubEmitStatus(b);
            }
        }

        // HUB-04: AsyncWebSocket keeps disconnected client objects until this
        // is called. Phase 1 never called it, so a session that saw phones
        // come and go leaked until the heap ran out -- which presents as the
        // hub degrading over a long shoot rather than failing outright.
        ws.cleanupClients();

        // ---- 1 Hz integrity line, ALWAYS ON (HUB-05 / SYNC-03) ----
        // Two framed records a second. This is the measurement Phase 1 called
        // the cheapest path to knowing anything, and it is worthless if it
        // only exists in a special build nobody flashes.
        {
            char buf[256];
            int off = 0;
            off += snprintf(buf + off, sizeof(buf) - off, "N rx=");
            for (int i = 0; i < NUM_PODS && off < 150; i++)
                off += snprintf(buf + off, sizeof(buf) - off, "%u,", (unsigned)g_nodeStats[i].received);
            off += snprintf(buf + off, sizeof(buf) - off, " lost=");
            for (int i = 0; i < NUM_PODS && off < 230; i++)
                off += snprintf(buf + off, sizeof(buf) - off, "%u,", (unsigned)g_nodeStats[i].lost);
            hubEmitStatus(buf);

            off = 0;
            off += snprintf(buf + off, sizeof(buf) - off,
                            "Q depth=%u high=%u/%u dropped=%u pushed=%u popped=%u "
                            "peerFail=%u cmd(ok=%u badsum=%u badlen=%u rst=%u) heap=%u ws=%u",
                            (unsigned)g_usb.count, (unsigned)g_usb.highWater, MESQ_USB_QUEUE_LEN,
                            (unsigned)g_usb.dropped, (unsigned)g_usb.pushed, (unsigned)g_usb.popped,
                            (unsigned)g_peerAddFail,
                            (unsigned)g_cmdParser.accepted, (unsigned)g_cmdParser.badChecksum,
                            (unsigned)g_cmdParser.badLen, (unsigned)g_cmdParser.resets,
                            (unsigned)ESP.getFreeHeap(), (unsigned)ws.count());
            hubEmitStatus(buf);
        }
#if MESQ_INSTR
        // ---- 1 Hz framed status. Emitted here, NOT from OnDataRecv. ----
        {
            char buf[256];
            int  off = 0;
            off += snprintf(buf + off, sizeof(buf) - off, "I1 rx=");
            for (int i = 0; i < NUM_PODS && off < 200; i++) {
                off += snprintf(buf + off, sizeof(buf) - off, "%u,", mesq_rx[i]);
            }
            off += snprintf(buf + off, sizeof(buf) - off,
                            " drop=%u/%u/%u wr_us(mean/max)=%u/%u sta=%d heap=%u "
                            "ws=%u rst=%u peerFail=%u",
                            mesq_dropLen, mesq_dropSync, mesq_dropId,
                            mesq_wrN ? (uint32_t)(mesq_wrSum / mesq_wrN) : 0,
                            mesq_wrMax,
                            (int)WiFi.softAPgetStationNum(),      // I7
                            (unsigned)ESP.getFreeHeap(),          // I11
                            (unsigned)ws.count(),                 // I11 / HUB-04
                            mesq_resetCalls, mesq_peerFail);
            mesq_emitStatus(buf);

            // second line: RSSI + battery per id (I2, H3)
            off = 0;
            off += snprintf(buf + off, sizeof(buf) - off, "I2 rssi=");
            for (int i = 0; i < NUM_PODS && off < 150; i++) {
                int32_t mean = mesq_rx[i] ? (mesq_rssiSum[i] / (int32_t)mesq_rx[i]) : 0;
                off += snprintf(buf + off, sizeof(buf) - off, "%d,", (int)mean);
            }
            off += snprintf(buf + off, sizeof(buf) - off, " batt=");
            for (int i = 0; i < NUM_PODS && off < 240; i++) {
                off += snprintf(buf + off, sizeof(buf) - off, "%u,", mesq_batt[i]);
            }
            mesq_emitStatus(buf);

            for (int i = 0; i < NUM_PODS; i++) {
                mesq_rx[i] = 0; mesq_rssiSum[i] = 0;
                mesq_rssiMin[i] = 127; mesq_rssiMax[i] = -128;
            }
            mesq_dropLen = mesq_dropSync = mesq_dropId = 0;
            mesq_wrN = 0; mesq_wrSum = 0; mesq_wrMax = 0;
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ============================================================
//  ESP-NOW CALLBACKS
//  Note: recv callback signature changed in IDF 5.x.
//  Compile-time switch handles both old and new core versions.
// ============================================================

void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    // Was: per-send Serial.println. Removed -- with binary forwarding, the
    // serial line is hot data only, not log noise. Re-enable only when
    // debugging the reboot path.
    (void)mac_addr; (void)status;
}

#if ESP_IDF_VERSION_MAJOR >= 5
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
    const uint8_t *mac_addr = recv_info->src_addr;
#else
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
#endif
    // Validate framing first -- a stray non-protocol packet must not be
    // forwarded to USB or it desynchronizes the host parser.
    if (len != POD_PACKET_LEN
        || incomingData[0] != SYNC0
        || incomingData[1] != SYNC1) {
#if MESQ_INSTR
        if (len != POD_PACKET_LEN) mesq_dropLen++; else mesq_dropSync++;
#endif
        return;
    }

    uint8_t id = incomingData[2];
    if (id >= NUM_PODS) {
        // Unknown bone id (or a control marker echoed back) -- drop.
#if MESQ_INSTR
        mesq_dropId++;
#endif
        return;
    }

    uint32_t nowMs = millis();

    // HUB-05 / SYNC-03: gaps, duplicates, reordering, wraps and pod reboots,
    // always on. UNITS: `count` is the pod's uint16 PACKET sequence; all the
    // arithmetic in mesqStatsOnPacket is wrap-safe, and a pod restart is
    // classified as a resync rather than ~65,000 lost packets.
    uint16_t cnt = (uint16_t)(incomingData[12] | ((uint16_t)incomingData[13] << 8));
    mesqStatsOnPacket(&g_nodeStats[id], cnt, nowMs);

#if MESQ_INSTR
    mesq_rx[id]++;
    mesq_batt[id] = incomingData[3];
  #if ESP_IDF_VERSION_MAJOR >= 5
    int8_t _rssi = (int8_t)recv_info->rx_ctrl->rssi;
    mesq_rssiSum[id] += _rssi;
    if (_rssi < mesq_rssiMin[id]) mesq_rssiMin[id] = _rssi;
    if (_rssi > mesq_rssiMax[id]) mesq_rssiMax[id] = _rssi;
  #endif
    int64_t _w0 = esp_timer_get_time();
#endif

    // HUB-02: COPY the borrowed bytes into owned storage and return. Phase 1
    // called Serial.write() here, inside the ESP-NOW receive callback, while
    // the AsyncTCP task could be writing a phone JSON line to the same
    // stream. Phase 2 measured that collision: one interleave destroys BOTH
    // records. It also means `incomingData` -- which the framework owns and
    // may reuse the moment this returns -- is no longer referenced after the
    // callback exits.
    mesqUsbPush(&g_usb, incomingData, POD_PACKET_LEN);

#if MESQ_INSTR
    {   // I3: the push is a bounded memcpy, so this should now be flat.
        uint32_t _d = (uint32_t)(esp_timer_get_time() - _w0);
        mesq_wrN++; mesq_wrSum += _d;
        if (_d > mesq_wrMax) mesq_wrMax = _d;
    }
#endif

    // Track liveness for the local connection map.
    portENTER_CRITICAL(&stateMux);
    podConnected[id] = true;
    podLastSeen[id]  = nowMs;
    portEXIT_CRITICAL(&stateMux);

    // HUB-07: Phase 1 set peerMacsInit[id] BEFORE calling esp_now_add_peer()
    // and ignored the return value. If the add failed, the flag said the peer
    // was registered, nothing ever retried, and sendReset() silently skipped
    // that pod forever. Set the flag only on success, and count failures.
    if (!peerMacsInit[id]) {
        esp_now_peer_info_t p;
        memset(&p, 0, sizeof(p));
        memcpy(p.peer_addr, mac_addr, 6);
        p.channel = ESPNOW_WIFI_CHANNEL;   // never 0: "current channel" races
        p.encrypt = false;
        if (esp_now_add_peer(&p) == ESP_OK) {
            peerMacs[id] = p;
            peerMacsInit[id] = true;
        } else {
            g_peerAddFail++;
#if MESQ_INSTR
            mesq_peerFail++;               // H2
#endif
        }
    }
}

// ============================================================
//  WEBSOCKET
// ============================================================

void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 &&
        info->len == len && info->opcode == WS_TEXT) {
        // HUB-02: this used to be Serial.println() straight from the AsyncTCP
        // task, racing the ESP-NOW callback's Serial.write(). The line now
        // goes through the same queue, so it is emitted whole or not at all.
        //
        // PROTOCOL: still a bare newline-terminated JSON line on the wire --
        // the browser's phone path is unchanged. The newline is appended here
        // because the queue carries records, not a stream.
        if (len + 1 > MESQ_USB_REC_MAX) return;      // bounded, never truncate
        uint8_t rec[MESQ_USB_REC_MAX];
        memcpy(rec, data, len);
        rec[len] = '\n';
        mesqUsbPush(&g_usb, rec, (uint8_t)(len + 1));
    }
}

void onEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
             AwsEventType type, void *arg, uint8_t *data, size_t len) {
    switch (type) {
    // These were raw Serial.printf() into the live binary stream -- HUB-02
    // every time a phone connected or dropped. Framed now.
    case WS_EVT_CONNECT: {
        char b[96];
        snprintf(b, sizeof(b), "WS connect #%u from %s",
                 client->id(), client->remoteIP().toString().c_str());
        hubEmitStatus(b);
        break;
    }
    case WS_EVT_DISCONNECT: {
        char b[64];
        snprintf(b, sizeof(b), "WS disconnect #%u", client->id());
        hubEmitStatus(b);
        break;
    }
    case WS_EVT_DATA:
        handleWebSocketMessage(arg, data, len);
        break;
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
        break;
    }
}

void initWebSocket() {
    ws.onEvent(onEvent);
    server.addHandler(&ws);
}

// ============================================================
//  FREERTOS WORKER TASKS
// ============================================================

// HUB-06: Phase 1 shipped these two as empty no-op loops. They are kept as
// named placeholders only because removing task handles other code refers to
// is a wider change than this phase warrants; they do nothing and cost one
// tick each. Recorded in the backlog rather than silently deleted.
void webSocketTask(void *pvParameters) {
    for (;;) vTaskDelay(pdMS_TO_TICKS(100));
}

void espNowTask(void *pvParameters) {
    for (;;) vTaskDelay(pdMS_TO_TICKS(100));
}

// ============================================================
//  USB WRITER  -- HUB-02
//  THE ONLY PLACE Serial.write() IS CALLED FOR STREAM DATA.
//
//  CONCURRENCY: producers are the ESP-NOW receive callback (WiFi task, core
//  0) and the WebSocket handler (AsyncTCP task, core 1). Both copy a whole
//  record into g_usb and return. This task drains it. That is why a record
//  can no longer be split by another writer.
//
//  It runs at priority 2 -- above the placeholder tasks -- because the queue
//  is the system's latency buffer: 17 pods x 32 Hz is ~544 records/s and the
//  ring is 128 deep, so roughly 235 ms of slack before records start being
//  dropped. Draining promptly is what keeps that slack available.
// ============================================================
void usbWriterTask(void *pvParameters) {
    uint8_t rec[MESQ_USB_REC_MAX];
    uint8_t len;
    for (;;) {
        bool any = false;
        while (mesqUsbPop(&g_usb, rec, &len)) {
            Serial.write(rec, len);
            any = true;
        }
        // Yield one tick when the queue drained. Busy-spinning here would
        // starve the very callbacks that feed it.
        if (!any) vTaskDelay(1);
    }
}

// ============================================================
//  HOST COMMAND HANDLER  -- HUB-03
//  Replaces:  if (Serial.available() > 0) { Serial.readString(); sendReset(); }
//  See mesq_hub_core.h for the frame layout and for what the checksum does
//  and does not protect against.
// ============================================================
void hostCmdTask(void *pvParameters) {
    mesqCmdInit(&g_cmdParser);
    MesqCmd cmd;
    for (;;) {
        while (Serial.available() > 0) {
            int b = Serial.read();
            if (b < 0) break;
            if (mesqCmdFeed(&g_cmdParser, (uint8_t)b, &cmd)) {
                char note[96];
                switch (cmd.cmd) {
                case MESQ_CMD_RESET_FLEET:
                    g_cmdParser.resets++;
                    snprintf(note, sizeof(note), "CMD reset_fleet accepted (#%u)",
                             (unsigned)g_cmdParser.resets);
                    hubEmitStatus(note);      // record WHY the fleet restarted
                    sendReset();
                    break;
                case MESQ_CMD_PING:
                    hubEmitStatus("CMD ping");
                    break;
                default:
                    // Fail closed on an unknown command: acknowledge that it
                    // was well-formed, do nothing, and do not guess.
                    snprintf(note, sizeof(note), "CMD unknown id=0x%02X ignored", cmd.cmd);
                    hubEmitStatus(note);
                    break;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ============================================================
//  SETUP
// ============================================================

void setup() {
    Serial.begin(921600);
    mesqUsbInit(&g_usb);
    mesqStatsInit(g_nodeStats, NUM_PODS);
    mesqCmdInit(&g_cmdParser);

    // ===== Phase 2 W0: provenance banner (resolves U2 for the hub) =====
    // NOTE: this prints TEXT on the same stream that later carries binary
    // pod frames. It is safe only because it runs once, before ESP-NOW is
    // initialised, so no binary frame can interleave with it (HUB-02).
    // Do NOT add further prints after esp_now_init() without reading
    // system_assessment_2/02_hub_instrumentation/REPORT.md first.
    delay(200);
    Serial.println();
    Serial.println("===== MESQUITE DONGLE BOOT =====");
    Serial.printf("FW_BUILD      : %s %s\n", __DATE__, __TIME__);
#ifdef ESP_ARDUINO_VERSION_STR
    Serial.printf("ARDUINO_CORE  : %s\n", ESP_ARDUINO_VERSION_STR);
#else
    Serial.println("ARDUINO_CORE  : <2.0.0 (macro absent)");
#endif
    Serial.printf("IDF_VERSION   : %s\n", esp_get_idf_version());
    Serial.printf("TICK_RATE_HZ  : %d\n", (int)configTICK_RATE_HZ);
    Serial.printf("RESET_REASON  : %d\n", (int)esp_reset_reason());
    Serial.printf("NUM_PODS      : %d\n", NUM_PODS);
    Serial.printf("ESPNOW_CHANNEL: %d\n", ESPNOW_WIFI_CHANNEL);
    Serial.printf("PKT_LEN       : %d\n", POD_PACKET_LEN);
    Serial.printf("HEAP_FREE     : %u\n", (unsigned)ESP.getFreeHeap());
    Serial.println("================================");

    // WiFi mode must come before macAddress()
    WiFi.mode(WIFI_AP_STA);
    String mac = WiFi.macAddress();
    Serial.println("STA MAC: " + mac);

    // Buttons — INPUT_PULLUP because KEY1 (IO14) has no external pullup
    pinMode(BTN_MAC,    INPUT_PULLUP);
    pinMode(BTN_STATUS, INPUT_PULLUP);

    // WiFi AP + WebSocket. The 3rd arg to softAP is the channel -- we lock
    // it explicitly so the AP can't drift off ESPNOW_WIFI_CHANNEL when a
    // client (phone) joins, and so two dongles at the same client site
    // can't end up on different channels by accident.
    initWebSocket();
    WiFi.softAP("MM-" + mac, "12345678", ESPNOW_WIFI_CHANNEL, /*hidden=*/0);
    server.begin();

    // Belt and suspenders: even after softAP() returns, force the radio
    // onto our channel, disable power save, and crank TX power. These three
    // calls are the actual fix for the Tempe -> Boston regression.
    esp_wifi_set_channel(ESPNOW_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(80);

    // ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("Error initializing ESP-NOW");
        return;
    }
    esp_now_register_send_cb(OnDataSent);
    esp_now_register_recv_cb(OnDataRecv);

    // Make the broadcast peer explicit. Without this entry, the dongle could
    // not previously have sent broadcasts -- and the pod-channel here also
    // gets pinned so it cannot disagree with the radio's channel.
    memset(&peerInfo, 0, sizeof(peerInfo));
    static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    memcpy(peerInfo.peer_addr, BCAST, 6);
    peerInfo.channel = ESPNOW_WIFI_CHANNEL;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("Failed to add broadcast peer");
    }

    Serial.println("Setup complete!");

    // The USB writer runs at a higher priority than everything else: it is
    // the only consumer of the queue every other path feeds.
    xTaskCreatePinnedToCore(usbWriterTask,  "usbWriterTask",  4096, NULL, 2, NULL,                 1);
    xTaskCreatePinnedToCore(hostCmdTask,    "hostCmdTask",    4096, NULL, 1, NULL,                 1);
    xTaskCreatePinnedToCore(espNowTask,     "espNowTask",     4096, NULL, 1, &espNowTaskHandle,    0);
    xTaskCreatePinnedToCore(webSocketTask,  "webSocketTask",  4096, NULL, 1, &webSocketTaskHandle, 1);
    xTaskCreatePinnedToCore(podTimeoutTask, "podTimeoutTask", 4096, NULL, 1, NULL,                 1);
}

// ============================================================
//  LOOP
// ============================================================

// HUB-03 -- Phase 1 was:
//
//     void loop() {
//       if (Serial.available() > 0) { Serial.readString(); sendReset(); }
//       vTaskDelay(pdMS_TO_TICKS(100));
//     }
//
// ANY inbound byte rebooted all 17 pods. A terminal probe, line noise, an
// `echo` into the wrong tty, or the browser's own startup traffic dropped the
// suit mid-capture. Phase 1 listed it as one of the latching causes behind
// symptoms S3/S4. Inbound bytes are now handled by hostCmdTask, which only
// acts on a complete, checksummed, explicitly framed command.
void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// ============================================================
//  SEND RESET TO ALL PODS
// ============================================================

// Binary control packet -- pod_watch.ino's OnDataRecv decodes it as:
//   [0xAA][0x55][0xFF][cmd]   cmd 0x01 = reboot.
void sendReset() {
#if MESQ_INSTR
    mesq_resetCalls++;   // H1 -> HUB-03: does a stray byte really reboot the fleet?
#endif
    uint8_t reboot_pkt[4] = { SYNC0, SYNC1, 0xFF, 0x01 };
    for (int i = 0; i < NUM_PODS; i++) {
        if (peerMacsInit[i]) {
            esp_now_send(peerMacs[i].peer_addr, reboot_pkt, sizeof(reboot_pkt));
        }
    }
}
