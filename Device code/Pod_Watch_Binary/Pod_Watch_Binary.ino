#define LILYGO_WATCH_2019_WITH_TOUCH
#include <LilyGoWatch.h> \\https://github.com/Xinyuan-LilyGO/TTGO_TWatch_Library/tree/master

TTGOClass *watch;
TFT_eSPI *tft;
bool isCharging = false;


// PROTOCOL: this is the hub's STA MAC and the pod sends UNICAST to it. It was
// called `broadcastAddress`, which is simply wrong and cost review time every
// time someone read the radio path looking for a broadcast that isn't there.
const char *mac_address_str = "DC:DA:0C:17:10:A0";
uint8_t hubAddress[6];

// Array of arrays containing 2 strings each
String boneName[][2] = {
  { "HEAD", "" },
  { "SPINE", "" },
  { "HIPS", "" },
  { "LEFT", "UP ARM" },
  { "LEFT", "FOREARM" },
  { "LEFT", "HAND" },
  { "RIGHT", "UP ARM" },
  { "RIGHT", "FOREARM" },
  { "RIGHT", "HAND" },
  { "LEFT", "UP LEG" },
  { "LEFT", "LOW LEG" },
  { "LEFT", "FOOT" },
  { "RIGHT", "UP LEG" },
  { "RIGHT", "LOW LEG" },
  { "RIGHT", "FOOT" },
  { "LEFT",  "SHOULDER" },   // 15
  { "RIGHT", "SHOULDER" },   // 16
};


// =========================================================================
//  POD IDENTITY  --  Phase 2, addresses NODE-05 (promoted to BLOCKER in §4.2)
//
//  The bone id is now supplied by the BUILD, not by editing comments here.
//  Phase 1 found the id was chosen by uncommenting one of 17 mutually
//  exclusive lines; flashing a 17-device fleet three times that way will
//  eventually produce a duplicate sendID, which is SILENT at runtime and
//  presents as "a node didn't connect".
//
//  Build one image per pod:
//      arduino-cli compile \
//        --build-property "build.extra_flags=-DMESQ_POD_ID=3" ...
//  or run  tools/build_pods.sh  which generates all 17.
//
//  If MESQ_POD_ID is not defined the build FAILS. That is deliberate: a
//  build error is recoverable in seconds, a duplicate id costs a session.
// =========================================================================
#ifndef MESQ_POD_ID
#error "MESQ_POD_ID is not defined. Build with -DMESQ_POD_ID=<0..16> (see tools/build_pods.sh). Phase 2 removed the comment-toggle identity block; see system_assessment_2/ROLLOUT.md"
#endif
#if (MESQ_POD_ID < 0) || (MESQ_POD_ID > 16)
#error "MESQ_POD_ID out of range - valid bone ids are 0..16 (see boneName[] above)"
#endif

// Screen colours per bone id, transcribed verbatim from the Phase 1
// comment block so the on-watch appearance is unchanged.
static const uint16_t POD_BG[17] = {
  0xffff, 0xffff, 0xffff,          //  0 Head, 1 Spine, 2 HipsAlt
  0x62d6, 0x62d6, 0x62d6,          //  3-5   left arm chain
  0xf720, 0xf720, 0xf720,          //  6-8   right arm chain
  0xc086, 0xc086, 0xc086,          //  9-11  left leg chain
  0x3d89, 0x3d89, 0x3d89,          // 12-14  right leg chain
  0x62d6, 0xf720                   // 15 LeftShoulder, 16 RightShoulder
};
static const uint16_t POD_FG[17] = {
  0x0000, 0x0000, 0x0000,
  0xffff, 0xffff, 0xffff,
  0x0000, 0x0000, 0x0000,
  0xffff, 0xffff, 0xffff,
  0xffff, 0xffff, 0xffff,
  0xffff, 0x0000
};

const int sendID = MESQ_POD_ID;
uint16_t BG = POD_BG[MESQ_POD_ID];
uint16_t FG = POD_FG[MESQ_POD_ID];




#include <esp_now.h>
#include <esp_system.h>  // Phase 2 W0: esp_reset_reason(), esp_get_idf_version()
#include <esp_wifi.h>   // Needed for esp_wifi_set_channel / esp_wifi_set_ps /
                        // esp_wifi_set_max_tx_power. Without these the radio
                        // floats to whatever channel the environment pushes
                        // it to, which is exactly how the Tempe->Boston
                        // regression happened.
#include <EEPROM.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>  //https://github.com/Links2004/arduinoWebSockets
#include <ESPmDNS.h>
#include "ICM_20948.h"  // Click here to get the library: http://librarymanager/All#SparkFun_ICM_20948_IMU
#define AD0_VAL 0

// Phase 3 cores. Both headers also compile on a desktop with -DMESQ_HOST_TEST
// so tools/firmware_tests.cpp exercises this exact code under real threads.
#include "mesq_pod_core.h"
#include "mesq_packet.h"

// ===========================================================================
//  RADIO CONFIG -- MUST MATCH THE DONGLE
//  Both pods and dongle must run on the same WiFi channel for ESP-NOW to
//  deliver any packets. Without an explicit lock, the radio uses whatever
//  channel WiFi.mode(WIFI_STA) defaulted to (usually 1), but the dongle's
//  softAP can land on a different channel depending on local 2.4 GHz noise.
//  Lock both sides to a single, fixed channel. Channel 1 is a safe default
//  but if your client site has a heavy 2.4 GHz AP on ch 1, you can move to
//  6 or 11 -- the value just has to match the dongle.
// ===========================================================================
#define ESPNOW_WIFI_CHANNEL 1

// =========================================================================
//  PHASE 2 INSTRUMENTATION  (W1)  -- observational only, no behaviour change
//  Build with -DMESQ_INSTR=1 to enable. With it 0 (default) every hook
//  compiles to nothing and the image is behaviourally identical to Phase 1.
//
//  I9  read duration + Quat6/s   -> SENS-01 (~55 Hz DMP ceiling), SENS-02
//  I4  send-interval histogram   -> NODE-03 (tick quantisation, S2)
//  I8  reset reason + boot count -> NODE-04 (init hang, S3/S4)
//  I11 free heap                 -> fragmentation
//  N1  unit-norm violations      -> NODE-01 (torn cross-core quaternion)
//  N2  negative sqrt radicand    -> SENS-03 (NaN -> zero quaternion)
//  N3  sample-to-send age        -> NODE-02 / SYNC-02 (stamp at transmit)
// =========================================================================
#ifndef MESQ_INSTR
#define MESQ_INSTR 0
#endif

// =========================================================================
//  SENS-02 -- the two raw DMP streams that are enabled and never read.
//
//  RAW_ACCELEROMETER and RAW_GYROSCOPE are enabled in setupIMU() and no code
//  anywhere reads data.Raw_Accel or data.Raw_Gyro. Every FIFO packet still
//  carries them, so every I2C read is longer than it needs to be.
//
//  This is left ON by default ON PURPOSE. It is a PERFORMANCE change and the
//  master prompt's Gate 5 requires a measured effect before one is made; no
//  pod has been on a bench this phase. Build with
//  -DMESQ_DISABLE_UNUSED_DMP_STREAMS=1 to run the A/B described in
//  A1_07_OPEN_ITEMS_AND_HARDWARE_RUNBOOK.md (M-SENS02), then decide.
// =========================================================================
#ifndef MESQ_DISABLE_UNUSED_DMP_STREAMS
#define MESQ_DISABLE_UNUSED_DMP_STREAMS 0
#endif

#if MESQ_INSTR
#include <esp_timer.h>
RTC_DATA_ATTR uint32_t mesq_bootCount = 0;   // survives reset, not power loss

// I9
static volatile uint32_t mesq_readN = 0, mesq_readMin = 0xFFFFFFFF,
                         mesq_readMax = 0; static volatile uint64_t mesq_readSum = 0;
static volatile uint32_t mesq_quat6N = 0;    // Quat6 FIFO packets this second
static volatile uint32_t mesq_fifoMoreN = 0; // reads reporting FIFOMoreDataAvail
// I4  - send intervals bucketed in ms: <20,20-29,30-39,40-49,50-59,60+
static volatile uint32_t mesq_sendBuckets[6] = {0,0,0,0,0,0};
static volatile uint32_t mesq_sendN = 0, mesq_sendMin = 0xFFFFFFFF, mesq_sendMax = 0;
// N1/N2/N3
static volatile uint32_t mesq_normBad = 0, mesq_radNeg = 0;
static volatile uint32_t mesq_ageN = 0, mesq_ageMax = 0; static volatile uint64_t mesq_ageSum = 0;
static volatile int64_t  mesq_lastSampleUs = 0;
// instrumentation self-cost (Rule 2)
static volatile uint64_t mesq_instrCostUs = 0;

static inline void mesq_bucketSend(uint32_t ms) {
  uint8_t b = (ms < 20) ? 0 : (ms < 30) ? 1 : (ms < 40) ? 2
            : (ms < 50) ? 3 : (ms < 60) ? 4 : 5;
  mesq_sendBuckets[b]++;
}
#endif

//#include "soc/rtc_wdt.h"
ICM_20948_I2C myICM;  // Otherwise create an ICM_20948_I2C object

//#include "Button2.h"
#define BUTTON_PIN 5
//Button2 button;

#include "esp_adc_cal.h"
#define BAT_ADC 35

// ---- Haptic (T-Watch 2019 Standard base plate) ----
// Motor lives on the base plate, GPIO 33. Power rail is AXP202 LDO3.
#define MOTOR_PIN 33
#define MOTOR_PULSE_MS 80

// ---- Power button ----
// T-Watch 2019 physical side button routed to GPIO 36 (input-only on ESP32).
#define PWR_BTN_PIN 36
#define LONG_PRESS_MS 2000

int fcount = 0;
int dccount = 0;
int count = 0;

void mac_string_to_uint8_array(const char *mac_str, uint8_t *mac_array) {
  if (mac_str == NULL || mac_array == NULL) {
    return;
  }

  int values[6];  // Temporary storage for parsed hexadecimal values
  int result = sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
                      &values[0], &values[1], &values[2],
                      &values[3], &values[4], &values[5]);

  if (result != 6) {
    return;
  }

  // Convert parsed integer values to uint8_t
  for (int i = 0; i < 6; ++i) {
    mac_array[i] = (uint8_t)values[i];
  }

  return;
}




int lastOn = millis();
bool isOn = true;

int lastTouch = millis();

// =========================================================================
//  BINARY WIRE FORMAT -- see mesq_packet.h for the full layout table and the
//  field-semantics notes. Layout is UNCHANGED from Phase 1; `ms_lo` now
//  carries the sample-decode time rather than the transmit time (NODE-02).
// =========================================================================
static uint8_t txBuf[MESQ_PACKET_LEN];

// CONCURRENCY: the one channel between the two cores.
//   producer: TaskReadIMU  (core 1)
//   consumer: TaskWifi     (core 0)
// Phase 1 shared four loose floats here with no synchronisation at all
// (NODE-01). See mesq_pod_core.h for why this is a critical section and not
// a queue or a seqlock.
MesqSampleChannel g_sampleCh;

// Counters surfaced by the 1 Hz instrumentation line.
static volatile uint32_t g_quatRepaired = 0;   // SENS-03: roundoff clamped
static volatile uint32_t g_quatRejected = 0;   // SENS-03: sample thrown away
static volatile uint32_t g_heldSends    = 0;   // SYNC-08: no fresh sample to send

// Create peer interface
esp_now_peer_info_t peerInfo;

// callback when data is sent
// NOTE: ESP-NOW callback signature changed between ESP32 Arduino core 2.x and 3.x.
// 2.x: void(const uint8_t *mac_addr, esp_now_send_status_t status)
// 3.x: void(const wifi_tx_info_t *tx_info, esp_now_send_status_t status)
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
void OnDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
#else
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
#endif
  // Serial.print("\r\nLast Packet Send Status:\t");
 // Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");


  if (status == ESP_NOW_SEND_SUCCESS) {
    dccount = 0;
  } else {
    dccount++;
    //digitalWrite(3, HIGH);
    //Serial.println(dccount);
    // No-link auto-shutdown. Was 960 (~30 s @ 32 fps) which is brutal in a
    // congested 2.4 GHz environment -- a brief WiFi storm could shut every
    // pod down mid-capture. Raised to ~5 minutes; if the dongle is really
    // gone the pod still shuts itself down to save the battery, but normal
    // hiccups don't cascade into "the whole suit went dead". 32 fps * 300 s
    // = 9600.
    if (dccount > 9600) {
      //esp_deep_sleep_start();
      watch->shutdown();
    }
  }
}

String mac_address;


int fps = 32;

int batt_v = 0;
// (quatI/quatJ/quatK/quatReal removed -- declared in Phase 1, never assigned
//  or read anywhere in the sketch.)

uint32_t readADC_Cal(int ADC_Raw) {
  esp_adc_cal_characteristics_t adc_chars;

  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars);
  return (esp_adc_cal_raw_to_voltage(ADC_Raw, &adc_chars));
}

bool calibrated = false;


// NODE-01: the Phase 1 shared state lived here --
//     struct Quat { float x, y, z, w; } quat;
// written field-by-field on core 1, read field-by-field on core 0, with no
// synchronisation. It is DELETED rather than left unused, so the
// unsynchronised path cannot be reintroduced by accident. The replacement is
// g_sampleCh (MesqSampleChannel), declared with the wire format above.

#define NB_RECS 5


char buff[256];
bool rtcIrq = false;
bool initial = 1;
bool otaStart = false;

uint8_t func_select = 0;
uint8_t omm = 99;
uint8_t xcolon = 0;
uint32_t targetTime = 0;  // for next 1 second timeout
uint32_t colour = 0;
int vref = 1100;

bool pressed = false;
uint32_t pressedTime = 0;
bool charge_indication = false;

uint8_t hh, mm, ss;
int pacnum = 0;



void hexdump(const void *mem, uint32_t len, uint8_t cols = 16) {
  const uint8_t *src = (const uint8_t *)mem;
  Serial.printf("\n[HEXDUMP] Address: 0x%08X len: 0x%X (%d)", (ptrdiff_t)src, len, len);
  for (uint32_t i = 0; i < len; i++) {
    if (i % cols == 0) {
      Serial.printf("\n[0x%08X] 0x%08X: ", (ptrdiff_t)src, i);
    }
    Serial.printf("%02X ", *src);
    src++;
  }
  Serial.printf("\n");
}



// define two tasks for Blink & AnalogRead
void TaskWifi(void *pvParameters);
void TaskReadIMU(void *pvParameters);

#if CONFIG_FREERTOS_UNICORE
#define ARDUINO_RUNNING_CORE 0
#else
#define ARDUINO_RUNNING_CORE 1
#endif


// =========================================================================
//  NODE-04 -- bounded initialisation with an observable error state
//
//  Phase 1:
//      while (!initialized) { myICM.begin(...); ... delay(500); }   // forever
//      ...
//      if (!success) { Serial.println("Enable DMP failed!"); while (1) ; }
//
//  A pod with a marginal I2C connection sat in one of those two loops for the
//  whole session. The screen still showed its bone name, the battery bar
//  still updated, and it transmitted nothing. To the operator it looked
//  identical to a radio problem, which is why Phase 1 listed it as one of the
//  latching causes of symptoms S3/S4.
//
//  RECOVERY: bounded attempts with backoff, a boot stage recorded at every
//  step, the failure PAINTED ON THE WATCH so it is visible across the room,
//  and a slow retry afterwards instead of either a hang or a reboot loop. A
//  reboot loop was rejected deliberately: it would re-enter setup(), re-run
//  the radio init, and make a dead pod look intermittent rather than dead.
// =========================================================================
enum MesqBootStage : uint8_t {
  MESQ_BOOT_START      = 0,
  MESQ_BOOT_I2C_OK     = 1,
  MESQ_BOOT_IMU_OK     = 2,
  MESQ_BOOT_DMP_OK     = 3,
  MESQ_BOOT_FIFO_OK    = 4,
  MESQ_BOOT_RUNNING    = 5,
  MESQ_BOOT_FAIL_IMU   = 200,
  MESQ_BOOT_FAIL_DMP   = 201
};
volatile uint8_t g_bootStage = MESQ_BOOT_START;

#define MESQ_IMU_MAX_ATTEMPTS 10
#define MESQ_IMU_RETRY_MS     20000   /* re-attempt every 20 s once failed */

static const char *mesqBootStageName(uint8_t st) {
  switch (st) {
    case MESQ_BOOT_START:    return "START";
    case MESQ_BOOT_I2C_OK:   return "I2C_OK";
    case MESQ_BOOT_IMU_OK:   return "IMU_OK";
    case MESQ_BOOT_DMP_OK:   return "DMP_OK";
    case MESQ_BOOT_FIFO_OK:  return "FIFO_OK";
    case MESQ_BOOT_RUNNING:  return "RUNNING";
    case MESQ_BOOT_FAIL_IMU: return "FAIL_IMU";
    case MESQ_BOOT_FAIL_DMP: return "FAIL_DMP";
  }
  return "?";
}

// Paint the failure where a human will see it. A pod that cannot reach its
// IMU must not look like a working pod with a radio problem.
static void mesqShowFault(const char *what) {
  tft->fillScreen(TFT_RED);
  tft->setTextColor(TFT_WHITE, TFT_RED);
  tft->setTextSize(1);
  tft->drawCentreString("SENSOR FAULT", 120, 60, 4);
  tft->drawCentreString(what, 120, 110, 4);
  tft->drawCentreString(String("POD ") + sendID, 120, 160, 4);
}

// Returns true when the IMU and DMP are both up.
// WHY: GPIO21 = SDA, GPIO22 = SCL at 400 kHz. These are the pins the audited
// code selects; the physical wiring and what else shares this bus have NOT
// been confirmed against a schematic or a continuity test, so do not remap
// them on the strength of this comment alone (see A1_07, item U-I2C).
bool setupIMU() {
  Wire.begin(21, 22);
  delay(500);
  Wire.setClock(400000);
  g_bootStage = MESQ_BOOT_I2C_OK;

  bool initialized = false;
  for (int attempt = 1; attempt <= MESQ_IMU_MAX_ATTEMPTS && !initialized; attempt++) {
    myICM.begin(Wire, AD0_VAL);
    Serial.printf("IMU_ATTEMPT   : %d/%d -> %s\n",
                  attempt, MESQ_IMU_MAX_ATTEMPTS, myICM.statusString());
    if (myICM.status == ICM_20948_Stat_Ok) {
      initialized = true;
    } else {
      // Linear backoff: 200, 400, ... 2000 ms. A cold ICM-20948 can need a
      // few hundred ms; ten attempts spans ~11 s, long enough to ride out a
      // slow power rail and short enough that the operator is not left
      // guessing whether the pod is alive.
      delay(200 * (attempt < 10 ? attempt : 10));
    }
  }

  if (!initialized) {
    g_bootStage = MESQ_BOOT_FAIL_IMU;
    Serial.println(F("INIT_RESULT   : IMU_FAIL"));
    mesqShowFault("IMU NOT FOUND");
    return false;                       // caller decides; no hang here
  }

  g_bootStage = MESQ_BOOT_IMU_OK;
  Serial.println(F("Device connected."));

  bool success = true;
  success &= (myICM.initializeDMP() == ICM_20948_Stat_Ok);

  // WHY Quat6 / Game Rotation Vector: 6-axis fusion, accelerometer plus
  // gyroscope, magnetometer deliberately disabled. That means there is NO
  // Earth-referenced yaw -- the heading at boot is arbitrary. Phase 1's
  // 71-99 deg session-to-session heading error is that arbitrary origin, NOT
  // gyro drift, and enabling the magnetometer is a calibrated, disturbance-
  // tested architecture decision, not a one-line change. See
  // A1_01_SOLUTION_DECISIONS.md, "heading".
  success &= (myICM.enableDMPSensor(INV_ICM20948_SENSOR_GAME_ROTATION_VECTOR) == ICM_20948_Stat_Ok);

#if !MESQ_DISABLE_UNUSED_DMP_STREAMS
  // SENS-02: enabled, never read. Kept on by default pending the bench A/B.
  success &= (myICM.enableDMPSensor(INV_ICM20948_SENSOR_RAW_GYROSCOPE) == ICM_20948_Stat_Ok);
  success &= (myICM.enableDMPSensor(INV_ICM20948_SENSOR_RAW_ACCELEROMETER) == ICM_20948_Stat_Ok);
#endif

  // UNITS: the ODR register holds a DIVIDER, not a rate.
  //     value = (DMP running rate / desired ODR) - 1
  // 0 therefore means "every cycle", i.e. the maximum this DMP configuration
  // produces. Phase 1 recorded ~55 Hz for the stock SparkFun path. That is a
  // property of THIS initialisation, not a hardware ceiling of the
  // ICM-20948 -- SparkFun ships Example10_DMP_FastMultipleSensors with a
  // higher-rate custom init. Whether it works with the pinned library version
  // is UNVERIFIED and needs a bench (A1_07, M-SENS01).
  success &= (myICM.setDMPODRrate(DMP_ODR_Reg_Quat6, 0) == ICM_20948_Stat_Ok);

  success &= (myICM.enableFIFO() == ICM_20948_Stat_Ok);
  success &= (myICM.enableDMP()  == ICM_20948_Stat_Ok);
  success &= (myICM.resetDMP()   == ICM_20948_Stat_Ok);
  success &= (myICM.resetFIFO()  == ICM_20948_Stat_Ok);

  if (!success) {
    g_bootStage = MESQ_BOOT_FAIL_DMP;
    Serial.println(F("INIT_RESULT   : IMU_OK DMP_FAIL"));
    Serial.println(F("Check that #define ICM_20948_USE_DMP is uncommented in ICM_20948_C.h"));
    mesqShowFault("DMP INIT FAILED");
    return false;                       // was: while (1) ;
  }

  g_bootStage = MESQ_BOOT_FIFO_OK;
  Serial.println(F("IMU enabled"));
  Serial.println(F("INIT_RESULT   : IMU_OK DMP_OK"));
  calibrated = true;
  return true;
}


void setup() {


  // Get TTGOClass instance
  watch = TTGOClass::getWatch();

  // Initialize the hardware, the BMA423 sensor has been initialized internally
  watch->begin();

  // ---- Haptic motor setup ----
  // Motor/speaker rail on the Standard base plate is AXP202 LDO3. Enable it,
  // otherwise toggling GPIO 33 does nothing (pin flips, motor has no power).
// Set LDO3 voltage explicitly before enabling (motor needs power)
watch->power->setLDO3Voltage(3300);                              // 3.3V
watch->power->setPowerOutPut(AXP202_LDO3, AXP202_ON);
delay(50);                                                        // let rail settle

pinMode(MOTOR_PIN, OUTPUT);
digitalWrite(MOTOR_PIN, LOW);

// Power button (GPIO 36) — input-only on ESP32
pinMode(PWR_BTN_PIN, INPUT);

// Boot indicator: two buzzes to confirm the pod has powered up
motorPulse(2);



  // Turn on the backlight
  watch->openBL();

  pinMode(TOUCH_INT, INPUT);


  watch->button->setPressedHandler(pressedB);
  watch->button->setReleasedHandler(released);


  //Receive objects for easy writing
  tft = watch->tft;
  tft->fillScreen(BG);
  tft->setTextColor(FG, BG);


  tft->setTextFont(7); 
  tft->drawCentreString("MESQUITE.cc", 120, 10, 4);


  if (boneName[sendID][1] == "") {
    tft->setTextSize(3);
    tft->drawCentreString(boneName[sendID][0], 120, 80, 4);
  } else {
    tft->setTextSize(3);
    tft->drawCentreString(boneName[sendID][0], 120, 65, 4);
    tft->setTextSize(2);
    tft->drawCentreString(boneName[sendID][1], 120, 130, 4);
  }

  tft->setTextSize(1);

  mac_string_to_uint8_array(mac_address_str, hubAddress);


  // pinMode(3, OUTPUT);
  Serial.begin(115200);
  delay(500);

  // ===== Phase 2 W0: provenance banner (resolves U2) =====
  // Printed once at boot on every pod. configTICK_RATE_HZ decides whether
  // vTaskDelay(1) is 1 ms or 10 ms, which decides whether the real transmit
  // rate is ~32 Hz or ~25 Hz (NODE-03). Three Phase 1 reports depend on it.
  Serial.println();
  Serial.println(F("===== MESQUITE POD BOOT ====="));
  Serial.printf("FW_BUILD      : %s %s\n", __DATE__, __TIME__);
  Serial.printf("POD_ID        : %d\n", sendID);
#ifdef ESP_ARDUINO_VERSION_STR
  Serial.printf("ARDUINO_CORE  : %s\n", ESP_ARDUINO_VERSION_STR);
#else
  Serial.println(F("ARDUINO_CORE  : <2.0.0 (macro absent)"));
#endif
  Serial.printf("IDF_VERSION   : %s\n", esp_get_idf_version());
  Serial.printf("TICK_RATE_HZ  : %d\n", (int)configTICK_RATE_HZ);
  Serial.printf("TICK_PERIOD_MS: %d\n", (int)portTICK_PERIOD_MS);
  Serial.printf("RESET_REASON  : %d\n", (int)esp_reset_reason());
  Serial.printf("CPU_FREQ_MHZ  : %d\n", (int)getCpuFrequencyMhz());
  Serial.printf("HEAP_FREE     : %u\n", (unsigned)ESP.getFreeHeap());
  Serial.printf("PSRAM_FREE    : %u\n", (unsigned)ESP.getFreePsram());
  Serial.printf("NOMINAL_FPS   : %d  (gate = 1000/%d = %d ms)\n", fps, fps, 1000/fps);
#if MESQ_INSTR
  mesq_bootCount++;                                  // I8
  Serial.printf("BOOT_COUNT    : %u  (RTC, survives reset)\n", mesq_bootCount);
  Serial.println(F("INSTR         : ENABLED (MESQ_INSTR=1)"));
#else
  Serial.println(F("INSTR         : disabled"));
#endif
  Serial.println(F("============================="));

  lastOn = millis();
  lastTouch = millis();


  WiFi.mode(WIFI_STA);

  // -- Radio hardening (root cause of the Tempe->Boston regression) --
  //  1) Pin to a known channel so we are guaranteed to share airwaves with
  //     the dongle no matter how noisy the local 2.4 GHz band is.
  //  2) Disable WiFi power-save: in PS modes the radio sleeps between beacons
  //     and ESP-NOW packets can be dropped during sleep windows. Pods are
  //     mains-/battery-powered with a 60 mA budget; PS savings don't justify
  //     the missing frames.
  //  3) Set TX power to the regulatory max (80 = 20 dBm). With pods worn at
  //     hip / arm height and the dongle 1-3 m away in a busy mocap volume,
  //     every dB helps.
  esp_wifi_set_channel(ESPNOW_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_max_tx_power(80);

  // esp_deep_sleep_enable_gpio_wakeup(BIT(36), ESP_GPIO_WAKEUP_GPIO_LOW);


  Serial.println("Connecting");

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // Once ESPNow is successfully Init, we will register for Send CB to
  // get the status of Trasnmitted packet
  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);


  // Register peer. `channel = 0` used to mean "use current channel" but that
  // is fragile -- if the STA roams the peer's channel becomes stale and ESP-
  // NOW silently drops sends. Be explicit.
  memcpy(peerInfo.peer_addr, hubAddress, 6);
  peerInfo.channel = ESPNOW_WIFI_CHANNEL;
  peerInfo.encrypt = false;

  // Add peer
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    return;
  }


  mac_address = WiFi.macAddress();
  Serial.println(mac_address);
  delay(100);


  // RECOVERY: a failed init no longer hangs. The tasks still start, the fault
  // is on the screen and in the boot banner, and TaskReadIMU retries slowly.
  if (!setupIMU()) {
    Serial.printf("BOOT_STAGE    : %s (pod will retry every %d ms)\n",
                  mesqBootStageName(g_bootStage), MESQ_IMU_RETRY_MS);
  } else {
    g_bootStage = MESQ_BOOT_RUNNING;
  }



  xTaskCreatePinnedToCore(
    TaskWifi, "TaskWifi"  // A name just for humans
    ,
    10000  // This stack size can be checked & adjusted by reading the Stack Highwater
    ,
    NULL, 1  // Priority, with 3 (configMAX_PRIORITIES - 1) being the highest, and 0 being the lowest.
    ,
    NULL, 0);

  // delay(1000);
  xTaskCreatePinnedToCore(
    TaskReadIMU, "TaskReadIMU", 10000  // Stack size
    ,
    NULL, 1  // Priority
    ,
    NULL, 1);

  handleBattDisplay();
}


void handleBattDisplay() {
  batt_v = getBattery();
  if (isCharging) {
    tft->fillRoundRect(0, 205, 240, 35, 0, TFT_GREEN);
    tft->setTextColor(TFT_BLACK, TFT_GREEN);
    tft->drawCentreString(String((int)batt_v) + "%, CHARGING", 120, 212, 4);
  } else {
    tft->fillRoundRect(0, 205, 240, 35, 0, TFT_RED);
    tft->setTextColor(TFT_WHITE, TFT_RED);
    tft->drawCentreString(String((int)batt_v) + "%, NOT Charging", 120, 212, 4);
  }
}

void loop() {
}

// Haptic helper: pulse the motor N times with a gap between pulses.
// Blocking: total time = count * MOTOR_PULSE_MS + (count-1) * 150 ms
void motorPulse(int count) {
  for (int i = 0; i < count; i++) {
    if (i > 0) delay(150);
    digitalWrite(MOTOR_PIN, HIGH);
    delay(MOTOR_PULSE_MS);
    digitalWrite(MOTOR_PIN, LOW);
  }
}

// ---- Screen-only helpers (no buzz) ----
// Used by touchscreen taps and the 5s idle auto-sleep.
void sleepScreen() {
  if (!isOn) return;
  watch->closeBL();
  watch->displayOff();
  isOn = false;
}

void wakeScreen() {
  if (isOn) return;
  watch->openBL();
  watch->displayWakeup();
  lastOn = millis();
  isOn = true;
  // Defensive: re-assert LDO3 (motor power rail) in case anything disabled it
  watch->power->setPowerOutPut(AXP202_LDO3, AXP202_ON);
}

void toggleScreen() {
  if (isOn) sleepScreen();
  else wakeScreen();
}

// ---- Long-press button state ----
// Short press does nothing (protects against accidental shutdowns during mocap).
// Hold for LONG_PRESS_MS to fully power down the pod via AXP202.
//
// We poll GPIO 36 (the physical power button) directly instead of using the
// TTGO button library's event handlers, because the library's timing is
// entangled with AXP202's built-in PEK handling and can swallow the event.
// Constants PWR_BTN_PIN and LONG_PRESS_MS are defined at the top of the file.

void pressedB() {
  // Kept as a registered handler for library compatibility. No-op.
}

void released() {
  // No-op. All long-press logic lives in TaskReadIMU's GPIO poll.
}


int getBattery() {
  watch->power->adc1Enable(AXP202_VBUS_VOL_ADC1 | AXP202_VBUS_CUR_ADC1 | AXP202_BATT_CUR_ADC1 | AXP202_BATT_VOL_ADC1, true);
  // get the values
  isCharging = watch->power->isChargeing();
  int per = watch->power->getBattPercentage();
  return per;
}

bool touchoff = false;

void TaskWifi(void *pvParameters) {
  // NODE-03 / NET-03: a fixed-grid deadline in MICROSECONDS, seeded with a
  // deterministic per-node phase offset. See mesq_pod_core.h for the three
  // defects this replaces and why the offset matters for a 17-pod fleet.
  // UNITS: 1000000 / 32 = 31250 us exactly. The Phase 1 gate computed
  // 1000 / 32 = 31 ms in integer arithmetic and then compared with `>`.
  MesqDeadline dl;
  mesqDeadlineInit(&dl, 1000000u / (uint32_t)fps, (uint32_t)micros(),
                   (uint32_t)sendID, MESQ_NUM_BONES);

  for (;;) {
    if (mesqDeadlineDue(&dl, (uint32_t)micros())) {
      fcount++;

      // CONCURRENCY: one coherent record crosses the core boundary here.
      // quaternion, sample time, sequence and validity are copied together
      // under a critical section, so they cannot come from different samples.
      bool isFresh = false;
      MesqSample smp = g_sampleCh.take(&isFresh);
      if (!isFresh) g_heldSends++;      // SYNC-08: this packet repeats a pose

      int b = batt_v;
      if (b < 0)   b = 0;
      if (b > 100) b = 100;

      // PROTOCOL: ms_lo carries smp.sample_ms -- the pod clock when the DMP
      // sample was DECODED, not when this packet is being sent. Two packets
      // with the same ms_lo therefore provably carry the same sensor sample,
      // which is how the browser tells a measurement from a held pose. The
      // 16-byte layout is unchanged; only this field's meaning is.
      // `count` stays the PACKET counter, so gap detection keeps measuring
      // radio loss rather than the intended ~55 -> ~32 Hz decimation.
      mesq_pack(txBuf, (uint8_t)sendID, (uint8_t)b,
                smp.w, smp.x, smp.y, smp.z,
                (uint16_t)count, (uint16_t)smp.sample_ms);

#if MESQ_INSTR
      {
        int64_t _ic0 = esp_timer_get_time();
        // N1: with the coherent channel this must stay at zero. A non-zero
        // count here means the handoff regressed.
        if (!mesqSampleCoherent(smp)) mesq_normBad++;
        // N3: age of the sample at the moment we transmit it. Same clock
        // domain (this pod's), so this is a valid duration, not a
        // cross-device latency (master prompt rule 16).
        if (mesq_lastSampleUs != 0) {
          uint32_t _age = (uint32_t)((_ic0 - mesq_lastSampleUs) / 1000);
          mesq_ageN++; mesq_ageSum += _age;
          if (_age > mesq_ageMax) mesq_ageMax = _age;
        }
        static uint32_t _lastSend = 0;
        uint32_t _nowMs = millis();
        if (_lastSend) {
          uint32_t _iv = _nowMs - _lastSend;
          mesq_sendN++; mesq_bucketSend(_iv);
          if (_iv < mesq_sendMin) mesq_sendMin = _iv;
          if (_iv > mesq_sendMax) mesq_sendMax = _iv;
        }
        _lastSend = _nowMs;
        mesq_instrCostUs += (uint64_t)(esp_timer_get_time() - _ic0);
      }
#endif

      // esp_now_send() returning ESP_OK means the frame was ACCEPTED for
      // transmission. It is not delivery, and the MAC-layer callback's
      // ESP_NOW_SEND_SUCCESS is not application receipt either. Only the
      // hub's per-node counters can say a packet arrived.
      esp_now_send(hubAddress, txBuf, MESQ_PACKET_LEN);

      count++;

#if MESQ_INSTR
      // ---- 1 Hz instrumentation report, on core 0 (TaskWifi) so the sample
      // ---- task on core 1 is not disturbed. The pod's Serial is its own USB
      // ---- port and is not the hub's binary stream.
      {
        static uint32_t _lastRep = 0;
        uint32_t _n2 = millis();
        if (_n2 - _lastRep >= 1000) {
          _lastRep = _n2;
          Serial.printf(
            "[INSTR] id=%d stage=%s quat6/s=%u read_us(min/mean/max)=%u/%u/%u fifoMore=%u "
            "send(n=%u min=%u max=%u b=%u/%u/%u/%u/%u/%u) age_ms(mean/max)=%u/%u "
            "normBad=%u radNeg=%u qRep=%u qRej=%u held=%u drop=%u late_us=%u resync=%u "
            "heap=%u boots=%u instr_us/s=%llu\n",
            sendID, mesqBootStageName(g_bootStage), mesq_quat6N,
            mesq_readN ? mesq_readMin : 0,
            mesq_readN ? (uint32_t)(mesq_readSum / mesq_readN) : 0,
            mesq_readMax, mesq_fifoMoreN,
            mesq_sendN, mesq_sendN ? mesq_sendMin : 0, mesq_sendMax,
            mesq_sendBuckets[0], mesq_sendBuckets[1], mesq_sendBuckets[2],
            mesq_sendBuckets[3], mesq_sendBuckets[4], mesq_sendBuckets[5],
            mesq_ageN ? (uint32_t)(mesq_ageSum / mesq_ageN) : 0, mesq_ageMax,
            mesq_normBad, mesq_radNeg, g_quatRepaired, g_quatRejected,
            g_heldSends, g_sampleCh.dropped(), dl.late_us_max, dl.resyncs,
            (unsigned)ESP.getFreeHeap(), mesq_bootCount,
            (unsigned long long)mesq_instrCostUs);
          mesq_quat6N = 0; mesq_readN = 0; mesq_readSum = 0;
          mesq_readMin = 0xFFFFFFFF; mesq_readMax = 0; mesq_fifoMoreN = 0;
          mesq_sendN = 0; mesq_sendMin = 0xFFFFFFFF; mesq_sendMax = 0;
          for (int _b = 0; _b < 6; _b++) mesq_sendBuckets[_b] = 0;
          mesq_ageN = 0; mesq_ageSum = 0; mesq_ageMax = 0;
          mesq_instrCostUs = 0;
          g_heldSends = 0; dl.late_us_max = 0;
        }
      }
#endif
    }
    vTaskDelay(1);
  }
}

// (ax/ay/az removed -- declared in Phase 1, never assigned or read. The raw
//  accelerometer stream they were presumably meant for is SENS-02.)

void TaskReadIMU(void *pvParameters) {
  // Local state for direct GPIO-based long-press detection.
  // `static` inside a task is fine — persists across iterations.
  static bool btnWasPressed = false;
  static uint32_t btnPressStart = 0;
  static bool longPressDone = false;

  for (;;) {
    watch->button->loop();  // kept because other library internals use it

    // --- Direct GPIO long-press shutdown ---
    // Button is active-low: pressed = 0, released = 1.
    bool btnNowPressed = (digitalRead(PWR_BTN_PIN) == LOW);

    if (btnNowPressed && !btnWasPressed) {
      // Rising edge of a press
      btnPressStart = millis();
      longPressDone = false;
    }
    if (btnNowPressed && !longPressDone
        && (millis() - btnPressStart >= LONG_PRESS_MS)) {
      longPressDone = true;
      motorPulse(1);       // confirmation — user can release now
      delay(100);
      watch->shutdown();
      while (1) delay(1000);  // should never reach here
    }
    btnWasPressed = btnNowPressed;

    // --- Screen-only idle auto-sleep (no buzz) ---
    if (isOn && millis() - lastOn > 5000) {
      sleepScreen();
    }

    // --- Touchscreen: toggle screen only, no buzz ---
    int16_t x, y;
    if (watch->getTouch(x, y)) {
      if (millis() - lastTouch > 600) {
        toggleScreen();
      }
      lastTouch = millis();
    }

    // PWR-01: this runs every 3 SECONDS, not "every minute" as the Phase 1
    // comment claimed -- a 20x error that made the cost look negligible.
    // handleBattDisplay() does an AXP202 ADC read AND a TFT redraw, both on
    // the sample task, so whatever they cost is time the FIFO is not being
    // drained. The cost is UNMEASURED; moving this work is a performance
    // change and needs the bench A/B in A1_07 (M-PWR01) first. The comment is
    // corrected now because a wrong comment is worse than none.
    static uint32_t prev_ms1 = millis();
    if (millis() > (prev_ms1 + 1000 * 3)) {
      handleBattDisplay();
      prev_ms1 = millis();
    }


    icm_20948_DMP_data_t data;
#if MESQ_INSTR
    int64_t _t0 = esp_timer_get_time();
#endif
    myICM.readDMPdataFromFIFO(&data);
#if MESQ_INSTR
    {
      uint32_t _d = (uint32_t)(esp_timer_get_time() - _t0);
      mesq_readN++; mesq_readSum += _d;
      if (_d < mesq_readMin) mesq_readMin = _d;
      if (_d > mesq_readMax) mesq_readMax = _d;
      if (myICM.status == ICM_20948_Stat_FIFOMoreDataAvail) mesq_fifoMoreN++;
    }
#endif

    if ((myICM.status == ICM_20948_Stat_Ok) || (myICM.status == ICM_20948_Stat_FIFOMoreDataAvail))  // Was valid data available?
    {
      //Serial.print(F("Received data! Header: 0x")); // Print the header in HEX so we can see what data is arriving in the FIFO
      //if ( data.header < 0x1000) Serial.print( "0" ); // Pad the zeros
      //if ( data.header < 0x100) Serial.print( "0" );
      //if ( data.header < 0x10) Serial.print( "0" );
      //Serial.println( data.header, HEX );

      if ((data.header & DMP_header_bitmap_Quat6) > 0)  // We asked for GRV, so we get Quat6
      {
#if MESQ_INSTR
        mesq_quat6N++;   // I9: this count per second IS the DMP output rate
#endif
        // MATH: the DMP ships x, y, z as Q30 fixed point and omits w, which
        // is recovered from the unit-norm constraint. mesqReconstructQuat()
        // owns the whole policy -- clamp true roundoff, reject a genuinely
        // bad sample, renormalise, and never produce the zero quaternion.
        // Phase 1 called sqrt() on the raw radicand; when it went negative
        // the result was NaN, q_to_i16(NaN) was 0, and the pod transmitted
        // (0,0,0,0), which is not a rotation at all (SENS-03).
        double q1 = mesqQ30ToDouble(data.Quat6.Data.Q1);
        double q2 = mesqQ30ToDouble(data.Quat6.Data.Q2);
        double q3 = mesqQ30ToDouble(data.Quat6.Data.Q3);

#if MESQ_INSTR
        if ((1.0 - (q1*q1 + q2*q2 + q3*q3)) < 0.0) mesq_radNeg++;   // N2
#endif
        double w, x, y, z;
        MesqQuatStatus qst = mesqReconstructQuat(q1, q2, q3, &w, &x, &y, &z);

        if (qst == MESQ_Q_REJECTED) {
          // RECOVERY: hold the previous orientation. The channel keeps its
          // last good sample and TaskWifi will mark the next packet HELD,
          // which is honest. Inventing a plausible rotation is not.
          g_quatRejected++;
        } else {
          if (qst == MESQ_Q_REPAIRED) g_quatRepaired++;

          // CONCURRENCY: the ONLY place the sample crosses to core 0. One
          // call publishes quaternion, sample time and sequence together.
          // UNITS: millis() here is the pod's own clock at FIFO DECODE. It
          // is a `sample_observed_time`, not the DMP's internal sample
          // instant -- no interrupt edge is wired, so the true instant is
          // unverified and must not be claimed (master prompt rule 16).
          g_sampleCh.publish((float)w, (float)x, (float)y, (float)z, millis());
#if MESQ_INSTR
          // N3 marks when a sample was PUBLISHED, so sample-to-send age is
          // measured against the pose actually transmitted. Stamping it for a
          // rejected sample too would report an age for data that never left.
          mesq_lastSampleUs = esp_timer_get_time();
#endif
        }

        // NODE-06: Phase 1 computed roll/pitch/yaw here with two atan2 and
        // one asin on every sample. Nothing read them -- they were local
        // variables, never stored, never transmitted. Removed.
      }
    }

    // WHY the conditional delay: when the FIFO reports more data we loop
    // immediately and drain it, because a backlog is latency. Only when the
    // FIFO is empty do we yield.
    if (myICM.status != ICM_20948_Stat_FIFOMoreDataAvail)
    {
      delay(10);
    }

    //vTaskDelay(1/portTICK_PERIOD_MS);  // one tick delay (15ms) in between reads for stability
    vTaskDelay(1);
  }
}




// NOTE: Recv callback signature also changed between core 2.x and 3.x.
// 2.x: void(const uint8_t *mac_addr, const uint8_t *data, int len)
// 3.x: void(const esp_now_recv_info_t *info, const uint8_t *data, int len)
// Binary control packet from the dongle:
//   [0xAA][0x55][0xFF][cmd]
// where cmd = 0x01 -> reboot. Anything else is ignored. The 0xFF in the id
// slot is the marker that distinguishes a control packet from a normal pod
// data frame (which has id 0..16).
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
#else
void OnDataRecv(const uint8_t *mac_addr, const uint8_t *incomingData, int len) {
#endif
  if (len >= 4
      && incomingData[0] == 0xAA
      && incomingData[1] == 0x55
      && incomingData[2] == 0xFF) {
    if (incomingData[3] == 0x01) {
      ESP.restart();
    }
  }
}
