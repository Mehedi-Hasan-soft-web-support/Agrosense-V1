/* =====================================================================
   AgroSense Node  —  ESP32 Smart Agriculture Monitoring Node
   Firmware v1.2  (ESP32 Arduino core 3.x compatible)
   ---------------------------------------------------------------------
   Sensors : BH1750 (lux, I2C 0x23)
             DHT11  (temp/humidity, GPIO 4)
             Capacitive Soil Moisture v2.0 (ADC1, GPIO 34)
   Display : SSD1306 0.96" OLED (I2C 0x3C)
   Cloud   : Supabase (PostgREST)

   CHANGELOG v1.2  (2-second upload mode)
     - Sample + upload interval = 2 s
     - TLS connection reuse (setReuse) -> proti upload e handshake nai
     - Ring buffer 450 slot (~15 min offline @ 2 s)
     - bufHead/bufCount uint16_t kora hoyeche (450 > 255)

   CHANGELOG v1.1
     - FIX: String(float, uint8_t) ambiguous overload on core 3.x
     - FIX: snprintf based number formatting + inf/nan guard
     - FIX: buffered flag now tracked per-sample (was wrong expression)
     - IMPROVED: flushBuffer() loop instead of recursion (stack safe)
     - IMPROVED: static WiFiClientSecure (kom stack usage)
     - ADDED: forward declarations, ADC attenuation core-3.x safe

   WIRING
     BH1750  VCC->3V3  GND->GND  SDA->GPIO21  SCL->GPIO22  ADDR->GND
     OLED    VCC->3V3  GND->GND  SDA->GPIO21  SCL->GPIO22
     DHT11   VCC->3V3  GND->GND  DATA->GPIO4  (10k pull-up DATA->3V3)
     SOIL    VCC->3V3  GND->GND  AOUT->GPIO34   <-- ADC1 only!
                                  ADC2 pin (0,2,4,12-15,25-27) WiFi
                                  on thakle analogRead fail kore.

   LIBRARIES (Library Manager theke install koro):
     "BH1750"                  by Christopher Laws
     "DHT sensor library"      by Adafruit
     "Adafruit Unified Sensor" by Adafruit
     "Adafruit SSD1306"        by Adafruit
     "Adafruit GFX Library"    by Adafruit

   BOARD: ESP32 Dev Module | Flash 4MB | Partition: Default
   ===================================================================== */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <BH1750.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <math.h>

// =====================================================================
//  USER CONFIG
// =====================================================================
#define WIFI_SSID     "admin"
#define WIFI_PASS     "12345678"

#define SUPABASE_URL  "https://ekzvuqxnhrxalcvyjxdd.supabase.co"
#define SUPABASE_KEY  "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImVrenZ1cXhuaHJ4YWxjdnlqeGRkIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODY4NzU4NTIsImV4cCI6MjEwMjQ1MTg1Mn0.bAENZpVEwtBcOaYTBL_0tEQjQq1U4tWZKpfVpFGkNBU"

#define DEVICE_ID     "agrosense-01"
#define FIRMWARE_VER  "v1.2.0"

// ---- Timing (ms) ----
const uint32_t SENSOR_INTERVAL    = 2000UL;                 // 2 s  sensor read
                                                            //      (DHT11 er minimum, er kome namio na)
const uint32_t UPLOAD_INTERVAL    = 2000UL;                 // 2 s  Supabase insert
const uint32_t PAGE_INTERVAL      = 4000UL;                 // 4 s  OLED page switch
const uint32_t KEEPALIVE_INTERVAL = 6UL * 3600UL * 1000UL;  // 6 h  anti-pause ping
const uint32_t WIFI_RETRY         = 20000UL;                // 20 s reconnect

// ---- Soil calibration (NIJER sensor diye measure kore boshao) ----
//  Serial Monitor e raw value dekho:
//    shukno batashe   -> SOIL_DRY_RAW
//    glass of water e -> SOIL_WET_RAW
const int SOIL_DRY_RAW = 3200;
const int SOIL_WET_RAW = 1350;

// ---- Local alert thresholds ----
const float SOIL_DRY_PCT = 30.0f;
const float TEMP_MAX_C   = 38.0f;
const float LUX_LOW      = 200.0f;

// =====================================================================
//  PINS
// =====================================================================
#define I2C_SDA   21
#define I2C_SCL   22
#define DHT_PIN   4
#define DHT_TYPE  DHT11
#define SOIL_PIN  34

#define SCREEN_W  128
#define SCREEN_H  64
#define OLED_ADDR 0x3C

// =====================================================================
//  OBJECTS
// =====================================================================
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
BH1750 lightMeter(0x23);
DHT dht(DHT_PIN, DHT_TYPE);

// =====================================================================
//  DATA MODEL
// =====================================================================
struct Sample {
  float    lux;
  float    temperature;
  float    humidity;
  int      soilRaw;
  float    soilPercent;
  int16_t  rssi;
  uint32_t uptimeS;
  time_t   ts;          // epoch UTC (0 = NTP sync hoy ni)
  bool     buffered;    // offline queue theke ashche kina
};

#define BUFFER_SIZE 450                 // ~15 min offline data @ 2 s
#define BATCH_MAX   30                  // ek request e max row (backlog flush)

Sample   ringBuf[BUFFER_SIZE];
uint16_t bufHead  = 0;          // 450 slot -> uint8_t e dhorbe na
uint16_t bufCount = 0;

Sample   current;
bool     bh1750OK = false;
bool     oledOK   = false;
bool     timeOK   = false;
uint32_t uploadOK = 0, uploadFail = 0;

uint32_t tSensor = 0, tUpload = 0, tPage = 0, tWifi = 0, tKeep = 0;
uint8_t  page = 0;

// =====================================================================
//  FORWARD DECLARATIONS
// =====================================================================
void   connectWiFi();
void   syncTime();
void   readSensors();
void   pushToBuffer(const Sample &s);
void   flushBuffer();
int    supabasePost(const char *path, const String &body, const char *prefer);
void   supabasePing();
void   logEvent(const char *level, const char *code, const char *msg);
String sampleToJson(const Sample &s);
String jsonNum(float v, unsigned int dec);
String isoTime(time_t t);
void   splash(const char *l1, const char *l2);
void   drawHeader(const char *title);
void   drawScreen();

// =====================================================================
//  SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("\n=== AgroSense Node " FIRMWARE_VER " ==="));

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);

  // ---- OLED ----
  oledOK = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledOK) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    splash("AgroSense", "booting...");
  } else {
    Serial.println(F("[OLED] not found at 0x3C"));
  }

  // ---- BH1750 ----
  bh1750OK = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  Serial.printf("[BH1750] %s\n", bh1750OK ? "OK" : "FAIL");

  // ---- DHT11 ----
  dht.begin();

  // ---- Soil ADC ----
  analogReadResolution(12);
  analogSetPinAttenuation(SOIL_PIN, ADC_11db);   // 0 - ~3.3 V

  connectWiFi();
  syncTime();

  readSensors();
  logEvent("info", "BOOT", "Device started " FIRMWARE_VER);

  tSensor = tUpload = tPage = tKeep = millis();
  Serial.printf("[MEM] free heap = %u bytes\n", ESP.getFreeHeap());
}

// =====================================================================
//  LOOP  (fully non-blocking)
// =====================================================================
void loop() {
  uint32_t now = millis();

  if (now - tSensor >= SENSOR_INTERVAL) {
    tSensor = now;
    readSensors();
  }

  if (now - tPage >= PAGE_INTERVAL) {
    tPage = now;
    page = (page + 1) % 4;
  }
  drawScreen();

  if (now - tUpload >= UPLOAD_INTERVAL) {
    tUpload = now;
    pushToBuffer(current);
    if (WiFi.status() == WL_CONNECTED) flushBuffer();
  }

  // Supabase anti auto-pause heartbeat
  if (now - tKeep >= KEEPALIVE_INTERVAL) {
    tKeep = now;
    if (WiFi.status() == WL_CONNECTED) supabasePing();
  }

  // WiFi auto-reconnect
  if (WiFi.status() != WL_CONNECTED && now - tWifi >= WIFI_RETRY) {
    tWifi = now;
    Serial.println(F("[WiFi] reconnecting..."));
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

// =====================================================================
//  SENSOR READ
// =====================================================================
void readSensors() {
  Sample s;

  // ---- BH1750 ----
  s.lux = NAN;
  if (bh1750OK && lightMeter.measurementReady()) {
    float l = lightMeter.readLightLevel();
    if (l >= 0) s.lux = l;
  }

  // ---- DHT11 ----
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  s.temperature = t;      // NAN thakle NAN e thakbe
  s.humidity    = h;

  // ---- Soil: median of 9 samples (noise reduction) ----
  int raw[9];
  for (int i = 0; i < 9; i++) {
    raw[i] = analogRead(SOIL_PIN);
    delayMicroseconds(500);
  }
  for (int i = 1; i < 9; i++) {                 // insertion sort
    int k = raw[i], j = i - 1;
    while (j >= 0 && raw[j] > k) { raw[j + 1] = raw[j]; j--; }
    raw[j + 1] = k;
  }
  s.soilRaw = raw[4];

  float pct = 100.0f * (float)(SOIL_DRY_RAW - s.soilRaw) /
                       (float)(SOIL_DRY_RAW - SOIL_WET_RAW);
  s.soilPercent = constrain(pct, 0.0f, 100.0f);

  s.rssi     = (WiFi.status() == WL_CONNECTED) ? (int16_t)WiFi.RSSI() : 0;
  s.uptimeS  = millis() / 1000UL;
  s.ts       = timeOK ? time(nullptr) : 0;
  s.buffered = false;

  current = s;

  // 2 s interval e proti line print korle Serial bhore jai -> proti 5 tay ekbar
  static uint8_t printSkip = 0;
  if (++printSkip >= 5) {
    printSkip = 0;
    Serial.printf("[READ] lux=%.1f  T=%.1fC  RH=%.0f%%  soil=%d (%.1f%%)  rssi=%d\n",
                  s.lux, s.temperature, s.humidity,
                  s.soilRaw, s.soilPercent, s.rssi);
  }

  // ---- Local alerts (hysteresis diye, spam kombe) ----
  static bool dryFlag = false, hotFlag = false;

  if (s.soilPercent < SOIL_DRY_PCT && !dryFlag) {
    dryFlag = true;
    logEvent("warning", "SOIL_DRY", "Soil moisture below threshold");
  } else if (s.soilPercent > SOIL_DRY_PCT + 8.0f) {
    dryFlag = false;
  }

  if (!isnan(s.temperature) && s.temperature > TEMP_MAX_C && !hotFlag) {
    hotFlag = true;
    logEvent("warning", "TEMP_HIGH", "Temperature above threshold");
  } else if (!isnan(s.temperature) && s.temperature < TEMP_MAX_C - 2.0f) {
    hotFlag = false;
  }
}

// =====================================================================
//  RING BUFFER
// =====================================================================
void pushToBuffer(const Sample &s) {
  Sample tmp = s;
  // WiFi na thakle mark kore rakhi -> Supabase e buffered=true jabe
  tmp.buffered = (WiFi.status() != WL_CONNECTED);

  ringBuf[bufHead] = tmp;
  bufHead = (bufHead + 1) % BUFFER_SIZE;

  if (bufCount < BUFFER_SIZE) bufCount++;
  else Serial.println(F("[BUF] overflow - oldest sample dropped"));
}

// =====================================================================
//  JSON HELPERS
// =====================================================================

// FIX v1.1: 'unsigned int' param + snprintf
// ESP32 core 3.x e String(float, uint8_t) ambiguous overload error dey.
String jsonNum(float v, unsigned int dec) {
  if (isnan(v) || isinf(v)) return String("null");
  char buf[20];
  snprintf(buf, sizeof(buf), "%.*f", (int)dec, (double)v);
  return String(buf);
}

String isoTime(time_t t) {
  if (t == 0) return String("");
  struct tm tmv;
  gmtime_r(&t, &tmv);
  char b[25];
  strftime(b, sizeof(b), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  return String(b);
}

String sampleToJson(const Sample &s) {
  String j;
  j.reserve(256);
  j  = "{";
  j += "\"device_id\":\"" DEVICE_ID "\",";
  j += "\"lux\":"          + jsonNum(s.lux, 1)         + ",";
  j += "\"temperature\":"  + jsonNum(s.temperature, 1) + ",";
  j += "\"humidity\":"     + jsonNum(s.humidity, 1)    + ",";
  j += "\"soil_raw\":"     + String(s.soilRaw)         + ",";
  j += "\"soil_percent\":" + jsonNum(s.soilPercent, 1) + ",";
  j += "\"rssi\":"         + String(s.rssi)            + ",";
  j += "\"uptime_s\":"     + String(s.uptimeS)         + ",";
  j += "\"buffered\":";
  j += (s.buffered ? "true" : "false");

  String ts = isoTime(s.ts);
  if (ts.length()) j += ",\"recorded_at\":\"" + ts + "\"";

  j += "}";
  return j;
}

// =====================================================================
//  SUPABASE UPLOAD
// =====================================================================

// Batch insert: POST /rest/v1/readings  with a JSON array
// v1.1: recursion sorano hoyeche -> while loop (stack safe)
void flushBuffer() {
  while (bufCount > 0 && WiFi.status() == WL_CONNECTED) {

    uint16_t start = (uint16_t)((bufHead + BUFFER_SIZE - bufCount) % BUFFER_SIZE);
    uint16_t n     = (bufCount > BATCH_MAX) ? BATCH_MAX : bufCount;

    String payload;
    payload.reserve((size_t)n * 260 + 8);
    payload = "[";
    for (uint16_t i = 0; i < n; i++) {
      if (i) payload += ",";
      payload += sampleToJson(ringBuf[(start + i) % BUFFER_SIZE]);
    }
    payload += "]";

    int code = supabasePost("/rest/v1/readings", payload, "return=minimal");

    if (code >= 200 && code < 300) {
      uploadOK++;
      bufCount -= n;
      if (n > 1 || (uploadOK % 30) == 0)      // 2 s mode e log kom rakhi
        Serial.printf("[UP] %u rows OK  (queue left = %u)\n", n, bufCount);
    } else {
      uploadFail++;
      Serial.printf("[UP] FAIL http=%d  (queued = %u, will retry)\n", code, bufCount);
      break;                           // porer cycle e abar try korbe
    }
  }
}

int supabasePost(const char *path, const String &body, const char *prefer) {
  if (WiFi.status() != WL_CONNECTED) return -100;

  // static -> stack e boro object banano lage na
  static WiFiClientSecure client;
  static HTTPClient       http;
  static bool             inited = false;

  // v1.2: ek baar setup, tarpor connection reuse.
  // 2 s interval e proti bar TLS handshake korle ESP32 kulabe na
  // (handshake ekai ~1-2 s ney). setReuse(true) socket khola rakhe.
  if (!inited) {
    client.setInsecure();               // production e: client.setCACert(root_ca)
    client.setTimeout(8);               // seconds
    http.setReuse(true);
    inited = true;
  }

  String url = String(SUPABASE_URL) + path;
  if (!http.begin(client, url)) {
    Serial.println(F("[HTTP] begin() failed"));
    client.stop();
    return -1;
  }

  http.setTimeout(8000);                // ms
  http.setConnectTimeout(6000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey",        SUPABASE_KEY);
  http.addHeader("Authorization", "Bearer " SUPABASE_KEY);
  if (prefer) http.addHeader("Prefer", prefer);

  int code = http.POST(body);

  if (code < 200 || code >= 300) {
    Serial.printf("[HTTP] %d -> %s\n", code, http.getString().c_str());
    http.end();
    client.stop();                      // connection nosto -> notun kore hobe
    return code;
  }

  http.end();                           // setReuse(true) -> socket khola thake
  return code;
}

void logEvent(const char *level, const char *code, const char *msg) {
  if (WiFi.status() != WL_CONNECTED) return;

  String body;
  body.reserve(200);
  body  = "{\"device_id\":\"" DEVICE_ID "\",";
  body += "\"level\":\""   + String(level) + "\",";
  body += "\"code\":\""    + String(code)  + "\",";
  body += "\"message\":\"" + String(msg)   + "\"}";

  supabasePost("/rest/v1/events", body, "return=minimal");
}

// Keep-alive: Supabase free project ke "active" rakhe (auto-pause thekano)
void supabasePing() {
  int c = supabasePost("/rest/v1/rpc/ping", "{\"p_source\":\"esp32\"}", nullptr);
  Serial.printf("[PING] keep-alive http=%d\n", c);
}

// =====================================================================
//  WIFI / NTP
// =====================================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print(F("[WiFi] connecting"));
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(400);
    Serial.print('.');
    splash("WiFi", "connecting...");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("[WiFi] connected  IP=%s  RSSI=%d\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else
    Serial.println(F("[WiFi] failed -> offline buffering mode"));
}

void syncTime() {
  if (WiFi.status() != WL_CONNECTED) return;

  configTime(0, 0, "pool.ntp.org", "time.google.com");   // UTC
  uint32_t t0 = millis();
  while (time(nullptr) < 1700000000 && millis() - t0 < 10000) delay(200);

  timeOK = (time(nullptr) > 1700000000);
  Serial.printf("[NTP] %s\n", timeOK ? "synced (UTC)" : "failed - server time use hobe");
}

// =====================================================================
//  OLED UI
// =====================================================================
void splash(const char *l1, const char *l2) {
  if (!oledOK) return;
  display.clearDisplay();
  display.setTextSize(2); display.setCursor(0, 16); display.println(l1);
  display.setTextSize(1); display.setCursor(0, 46); display.println(l2);
  display.display();
}

void drawHeader(const char *title) {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(title);
  display.setCursor(98, 0);
  display.print(WiFi.status() == WL_CONNECTED ? F("NET") : F("OFF"));
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
}

void drawScreen() {
  if (!oledOK) return;

  static uint32_t last = 0;
  if (millis() - last < 200) return;          // ~5 fps
  last = millis();

  display.clearDisplay();

  switch (page) {

    case 0: {                                  // ---- CLIMATE ----
      drawHeader("CLIMATE");
      display.setTextSize(2);
      display.setCursor(0, 18);
      if (isnan(current.temperature)) display.print(F("--.-"));
      else                            display.print(current.temperature, 1);
      display.setTextSize(1); display.print(F(" C"));

      display.setTextSize(2);
      display.setCursor(0, 42);
      if (isnan(current.humidity)) display.print(F("--"));
      else                         display.print(current.humidity, 0);
      display.setTextSize(1); display.print(F(" %RH"));
      break;
    }

    case 1: {                                  // ---- LIGHT ----
      drawHeader("LIGHT");
      display.setTextSize(2);
      display.setCursor(0, 22);
      if (isnan(current.lux)) display.print(F("---"));
      else                    display.print(current.lux, 0);
      display.setTextSize(1); display.print(F(" lx"));

      display.setCursor(0, 52);
      if (isnan(current.lux))             display.print(F("Sensor: NO DATA"));
      else if (current.lux < LUX_LOW)      display.print(F("Status: LOW LIGHT"));
      else                                 display.print(F("Status: OK"));
      break;
    }

    case 2: {                                  // ---- SOIL ----
      drawHeader("SOIL");
      display.setTextSize(2);
      display.setCursor(0, 18);
      display.print(current.soilPercent, 0);
      display.setTextSize(1); display.print(F(" %"));

      display.drawRect(0, 42, 128, 12, SSD1306_WHITE);
      int w = (int)(current.soilPercent * 1.26f);
      display.fillRect(1, 43, constrain(w, 0, 126), 10, SSD1306_WHITE);

      display.setCursor(0, 56);
      display.print(current.soilPercent < SOIL_DRY_PCT ? F("DRY - irrigate!")
                                                       : F("Moisture OK"));
      break;
    }

    case 3: {                                  // ---- SYSTEM ----
      drawHeader("SYSTEM");
      display.setTextSize(1);

      display.setCursor(0, 14);
      display.print(F("ID  : ")); display.print(F(DEVICE_ID));

      display.setCursor(0, 24);
      display.print(F("Up  : "));
      display.print(current.uptimeS / 3600UL);       display.print(F("h "));
      display.print((current.uptimeS % 3600UL) / 60); display.print(F("m"));

      display.setCursor(0, 34);
      display.print(F("RSSI: ")); display.print(current.rssi); display.print(F(" dBm"));

      display.setCursor(0, 44);
      display.print(F("Sent: ")); display.print(uploadOK);
      display.print(F(" Err: ")); display.print(uploadFail);

      display.setCursor(0, 54);
      display.print(F("Queue:")); display.print(bufCount);
      display.print(F(" Heap:")); display.print(ESP.getFreeHeap() / 1024); display.print(F("k"));
      break;
    }
  }

  display.display();
}
