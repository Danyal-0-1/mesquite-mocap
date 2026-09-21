/* =========================================================================
   ARDUINO / ESP32 STUB LAYER  --  tools/arduino_stubs/mesq_stubs.h

   There is no arduino-cli on this machine, so the sketches cannot be built
   for the target. This layer declares just enough of the Arduino, FreeRTOS,
   ESP-NOW, TTGO and SparkFun ICM-20948 surface for g++ to TYPE-CHECK the
   sketch bodies: every call signature, every type, every member access.

   WHAT THIS PROVES: the sketch is syntactically valid C++ and every call the
   Phase 3 edits make matches the signature declared here.

   WHAT IT DOES NOT PROVE: that these signatures match the REAL libraries at
   their pinned versions, that the code links, or that it behaves correctly on
   a watch. Compilation is not verification. A real arduino-cli build and a
   flash remain outstanding -- see A1_07_OPEN_ITEMS_AND_HARDWARE_RUNBOOK.md.
   ========================================================================= */
#ifndef MESQ_STUBS_H
#define MESQ_STUBS_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>

// ---- Arduino core ------------------------------------------------------
typedef uint8_t byte;
#define PI 3.1415926535897932384626433832795
#define F(x) (x)
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define HIGH 1
#define LOW 0
#define TFT_RED 0xF800
#define TFT_GREEN 0x07E0
#define TFT_WHITE 0xFFFF
#define TFT_BLACK 0x0000
#define TOUCH_INT 38

static inline uint32_t millis() { return 0; }
static inline uint32_t micros() { return 0; }
static inline void delay(uint32_t) {}
static inline void pinMode(int, int) {}
static inline void digitalWrite(int, int) {}
static inline int  digitalRead(int) { return 1; }
static inline uint32_t getCpuFrequencyMhz() { return 240; }

struct String {
  std::string s;
  String() {}
  String(const char *c) : s(c ? c : "") {}
  String(int v) { char b[32]; snprintf(b, sizeof(b), "%d", v); s = b; }
  String(unsigned v) { char b[32]; snprintf(b, sizeof(b), "%u", v); s = b; }
  const char *c_str() const { return s.c_str(); }
  bool operator==(const char *o) const { return s == o; }
  String operator+(const String &o) const { String r; r.s = s + o.s; return r; }
};
static inline String operator+(const char *a, const String &b) { return String(a) + b; }

struct SerialClass {
  void begin(unsigned long) {}
  void println() {}
  void println(const char *) {}
  void println(const String &) {}
  void print(const char *) {}
  void print(const String &) {}
  template <typename... A> void printf(const char *, A...) {}
  int  available() { return 0; }
  String readString() { return String(); }
  size_t write(const uint8_t *, size_t) { return 0; }
};
static SerialClass Serial;

struct ESPClass {
  uint32_t getFreeHeap() { return 100000; }
  uint32_t getFreePsram() { return 100000; }
  void restart() {}
};
static ESPClass ESP;

// ---- FreeRTOS ----------------------------------------------------------
#define configTICK_RATE_HZ 1000
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(x) (x)
#define CONFIG_FREERTOS_UNICORE 0
typedef void *TaskHandle_t;
typedef struct { volatile uint32_t owner; volatile uint32_t count; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0, 0 }
static inline void portENTER_CRITICAL(portMUX_TYPE *) {}
static inline void portEXIT_CRITICAL(portMUX_TYPE *) {}
static inline void vTaskDelay(uint32_t) {}
static inline int xTaskCreatePinnedToCore(void (*)(void *), const char *, uint32_t,
                                          void *, uint32_t, TaskHandle_t *, int) { return 1; }
#define RTC_DATA_ATTR

// ---- ESP-IDF / ESP-NOW -------------------------------------------------
#define ESP_OK 0
typedef int esp_err_t;
typedef enum { ESP_NOW_SEND_SUCCESS = 0, ESP_NOW_SEND_FAIL = 1 } esp_now_send_status_t;
typedef struct { uint8_t peer_addr[6]; uint8_t channel; bool encrypt; } esp_now_peer_info_t;
typedef struct { int rssi; } wifi_pkt_rx_ctrl_t;
typedef struct { uint8_t src_addr[6]; wifi_pkt_rx_ctrl_t *rx_ctrl; } esp_now_recv_info_t;
typedef struct { uint8_t des_addr[6]; } wifi_tx_info_t;
static inline esp_err_t esp_now_init() { return ESP_OK; }
static inline esp_err_t esp_now_add_peer(const esp_now_peer_info_t *) { return ESP_OK; }
static inline esp_err_t esp_now_send(const uint8_t *, const uint8_t *, size_t) { return ESP_OK; }
// Templated so the stub accepts whichever callback signature the sketch's
// core-version #if selected, exactly as the real headers do.
template <typename F> static inline esp_err_t esp_now_register_send_cb(F) { return ESP_OK; }
template <typename F> static inline esp_err_t esp_now_register_recv_cb(F) { return ESP_OK; }
static inline const char *esp_get_idf_version() { return "stub"; }
static inline int esp_reset_reason() { return 1; }
static inline int64_t esp_timer_get_time() { return 0; }
#define WIFI_SECOND_CHAN_NONE 0
#define WIFI_PS_NONE 0
#define WIFI_STA 1
#define WIFI_AP_STA 3
static inline esp_err_t esp_wifi_set_channel(int, int) { return ESP_OK; }
static inline esp_err_t esp_wifi_set_ps(int) { return ESP_OK; }
static inline esp_err_t esp_wifi_set_max_tx_power(int) { return ESP_OK; }
struct WiFiClass {
  void mode(int) {}
  String macAddress() { return String("AA:BB:CC:DD:EE:FF"); }
  void softAP(const String &, const char *, int, int) {}
  int softAPgetStationNum() { return 0; }
};
static WiFiClass WiFi;

// ---- ADC calibration ---------------------------------------------------
#define ADC_UNIT_1 0
#define ADC_ATTEN_DB_11 3
#define ADC_WIDTH_BIT_12 3
typedef struct { int dummy; } esp_adc_cal_characteristics_t;
static inline void esp_adc_cal_characterize(int, int, int, int, esp_adc_cal_characteristics_t *) {}
static inline uint32_t esp_adc_cal_raw_to_voltage(int, esp_adc_cal_characteristics_t *) { return 0; }

// ---- TTGO T-Watch ------------------------------------------------------
#define AXP202_LDO3 2
#define AXP202_ON 1
#define AXP202_VBUS_VOL_ADC1 1
#define AXP202_VBUS_CUR_ADC1 2
#define AXP202_BATT_CUR_ADC1 4
#define AXP202_BATT_VOL_ADC1 8
struct TFT_eSPI {
  void fillScreen(uint16_t) {}
  void setTextColor(uint16_t, uint16_t) {}
  void setTextFont(int) {}
  void setTextSize(int) {}
  void drawCentreString(const String &, int, int, int) {}
  void fillRoundRect(int, int, int, int, int, uint16_t) {}
};
struct AXPPower {
  void setLDO3Voltage(int) {}
  void setPowerOutPut(int, int) {}
  bool isChargeing() { return false; }
  int  getBattPercentage() { return 80; }
  void adc1Enable(int, bool) {}
};
struct TTGOButton { void loop() {} void setPressedHandler(void (*)()) {} void setReleasedHandler(void (*)()) {} };
struct TTGOClass {
  TFT_eSPI  *tft;
  AXPPower  *power;
  TTGOButton *button;
  static TTGOClass *getWatch() { static TTGOClass w; static TFT_eSPI t; static AXPPower p;
                                 static TTGOButton b; w.tft = &t; w.power = &p; w.button = &b; return &w; }
  void begin() {}
  void openBL() {}
  void closeBL() {}
  void displayOff() {}
  void displayWakeup() {}
  void shutdown() {}
  bool getTouch(int16_t &, int16_t &) { return false; }
};

// ---- SparkFun ICM-20948 ------------------------------------------------
typedef enum { ICM_20948_Stat_Ok = 0, ICM_20948_Stat_Err = 1,
               ICM_20948_Stat_FIFOMoreDataAvail = 2 } ICM_20948_Status_e;
#define INV_ICM20948_SENSOR_GAME_ROTATION_VECTOR 1
#define INV_ICM20948_SENSOR_RAW_GYROSCOPE 2
#define INV_ICM20948_SENSOR_RAW_ACCELEROMETER 3
#define DMP_ODR_Reg_Quat6 1
#define DMP_header_bitmap_Quat6 0x0400
typedef struct {
  uint16_t header;
  struct { struct { int32_t Q1, Q2, Q3; } Data; } Quat6;
} icm_20948_DMP_data_t;
struct WireClass { void begin(int, int) {} void setClock(uint32_t) {} };
static WireClass Wire;
struct ICM_20948_I2C {
  ICM_20948_Status_e status = ICM_20948_Stat_Ok;
  void begin(WireClass &, int) {}
  const char *statusString() { return "Ok"; }
  ICM_20948_Status_e initializeDMP() { return ICM_20948_Stat_Ok; }
  ICM_20948_Status_e enableDMPSensor(int) { return ICM_20948_Stat_Ok; }
  ICM_20948_Status_e setDMPODRrate(int, int) { return ICM_20948_Stat_Ok; }
  ICM_20948_Status_e enableFIFO() { return ICM_20948_Stat_Ok; }
  ICM_20948_Status_e enableDMP() { return ICM_20948_Stat_Ok; }
  ICM_20948_Status_e resetDMP() { return ICM_20948_Stat_Ok; }
  ICM_20948_Status_e resetFIFO() { return ICM_20948_Stat_Ok; }
  void readDMPdataFromFIFO(icm_20948_DMP_data_t *) {}
};

// ---- AsyncWebServer / WebSocket (hub only) -----------------------------
#define WS_TEXT 1
#define ESP_IDF_VERSION_MAJOR 5
typedef enum { WS_EVT_CONNECT, WS_EVT_DISCONNECT, WS_EVT_DATA, WS_EVT_PONG, WS_EVT_ERROR } AwsEventType;
typedef struct { bool final; size_t index; size_t len; int opcode; } AwsFrameInfo;
struct IPAddr { String toString() { return String("0.0.0.0"); } };
struct AsyncWebSocketClient { uint32_t id() { return 0; } IPAddr remoteIP() { return IPAddr(); } };
struct AsyncWebSocket {
  AsyncWebSocket(const char *) {}
  void onEvent(void (*)(AsyncWebSocket *, AsyncWebSocketClient *, AwsEventType, void *, uint8_t *, size_t)) {}
  uint32_t count() { return 0; }
  void cleanupClients() {}
};
struct AsyncWebServer {
  AsyncWebServer(int) {}
  void addHandler(AsyncWebSocket *) {}
  void begin() {}
};

#endif /* MESQ_STUBS_H */
