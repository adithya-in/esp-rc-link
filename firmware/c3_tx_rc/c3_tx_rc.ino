#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>

namespace {

constexpr uint32_t kMagic = 0x52C7A5E1UL;
constexpr uint8_t  kRxMac[6] = {0x84, 0xCC, 0xA8, 0xB4, 0xD6, 0xE8};

constexpr int  kAxisPin[4] = {0, 1, 3, 4};
constexpr char kSrcName[6][6] = {"J1.X", "J1.Y", "J2.X", "J2.Y", "SW1", "SW2"};
constexpr int  kSw1Pin = 5;
constexpr int  kSw2Pin = 6;

constexpr int  kDeadzone = 15;
constexpr int  kPosSpan  = 900;
constexpr int  kNegSpan  = 1200;
constexpr int  kAvg      = 2;

constexpr uint32_t kTxPeriodMs  = 10;
constexpr uint32_t kLogPeriodMs = 500;
constexpr uint32_t kCpuMhz      = 160;

constexpr wifi_power_t kTxPower = WIFI_POWER_19_5dBm;

struct __attribute__((packed)) RcPacket {
  uint32_t magic;
  uint16_t ch[6];
  uint16_t crc;
};

RcPacket pkt;
Preferences prefs;

uint8_t chMap[6] = {0, 1, 2, 3, 4, 5};
uint16_t centerAdc[4] = {2048, 2048, 2048, 2048};
uint16_t rawAdc[4] = {0, 0, 0, 0};

uint32_t lastTx = 0;
uint32_t lastLog = 0;
volatile uint32_t txOk = 0;
volatile uint32_t txFail = 0;

static uint16_t crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
  }
  return crc;
}

static inline uint16_t sampleAdcFast(int pin) {
  uint32_t sum = 0;
  for (int i = 0; i < kAvg; i++) {
    sum += analogRead(pin);
  }
  return (uint16_t)(sum / kAvg);
}

static inline uint16_t axisToChannel(int idx) {
  const int d = (int)rawAdc[idx] - (int)centerAdc[idx];
  if (abs(d) < kDeadzone) return 1500;
  const int span = (d > 0) ? kPosSpan : kNegSpan;
  long out = 1500 + (long)d * 500L / (long)span;
  if (out < 1000) out = 1000;
  if (out > 2000) out = 2000;
  return (uint16_t)out;
}

static inline uint16_t switchToChannel(int pin) {
  return (digitalRead(pin) == LOW) ? 1000 : 2000;
}

static inline uint16_t readSource(uint8_t src) {
  if (src < 4) return axisToChannel(src);
  if (src == 4) return switchToChannel(kSw1Pin);
  if (src == 5) return switchToChannel(kSw2Pin);
  return 1500;
}

static void calibration() {
  uint32_t sum[4] = {0, 0, 0, 0};
  const int n = 32;
  for (int s = 0; s < n; s++) {
    for (int i = 0; i < 4; i++) sum[i] += analogRead(kAxisPin[i]);
    delay(2);
  }
  for (int i = 0; i < 4; i++) centerAdc[i] = (uint16_t)(sum[i] / n);
}

static void loadMap() {
  prefs.begin("rc", true);
  if (prefs.getBytesLength("map") == 6) prefs.getBytes("map", chMap, 6);
  prefs.end();
  for (int i = 0; i < 6; i++) if (chMap[i] > 5) chMap[i] = i;
}

static void saveMap() {
  prefs.begin("rc", false);
  prefs.putBytes("map", chMap, 6);
  prefs.end();
}

static void printMap() {
  Serial.print("map:");
  for (int i = 0; i < 6; i++) Serial.printf(" %s", kSrcName[chMap[i]]);
  Serial.println();
}

static void processCmd(const String &raw) {
  String cmd = raw;
  cmd.trim();
  if (cmd.startsWith("map:")) {
    int v[6], n = 0, start = 4;
    for (int i = 4; i <= (int)cmd.length() && n < 6; i++) {
      if (i == (int)cmd.length() || cmd[i] == ',' || cmd[i] == ' ') {
        if (i > start) v[n++] = cmd.substring(start, i).toInt();
        start = i + 1;
      }
    }
    bool ok = (n == 6);
    for (int i = 0; i < 6 && ok; i++) if (v[i] < 0 || v[i] > 5) ok = false;
    if (ok) {
      for (int i = 0; i < 6; i++) chMap[i] = (uint8_t)v[i];
      saveMap();
      Serial.println("map saved");
      printMap();
    } else {
      Serial.println("map err: expect map:i,i,i,i,i,i (0-5)");
    }
  } else if (cmd == "map?") {
    printMap();
  } else if (cmd == "centres?") {
    Serial.printf("centres: %u %u %u %u\n", centerAdc[0], centerAdc[1], centerAdc[2], centerAdc[3]);
  }
}

static void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') { if (line.length()) processCmd(line); line = ""; }
    else if (c != '\r') line += c;
    if (line.length() > 80) line = "";
  }
}

static void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
  (void)mac;
  if (status == ESP_NOW_SEND_SUCCESS) txOk++; else txFail++;
}

static uint32_t gUpdLastUs = 0, gUpdMaxUs = 0;
static uint32_t gLoopLastUs = 0, gLoopMaxUs = 0;
static uint32_t gSendsPerWin = 0;

static void logStatus() {
  Serial.print("raw:");
  for (int i = 0; i < 4; i++) {
    Serial.printf(" %s=%d/%dmV", kSrcName[i], rawAdc[i], analogReadMilliVolts(kAxisPin[i]));
  }
  Serial.printf(" SW1=%d SW2=%d ch:", digitalRead(kSw1Pin), digitalRead(kSw2Pin));
  for (int i = 0; i < 6; i++) Serial.printf(" %u", pkt.ch[i]);
  uint32_t ok = txOk, fail = txFail;
  txOk = txFail = 0;
  Serial.printf(" tx=%lu|%lu rate=%luHz upd=%lu/%luus loop=%lu/%luus\n",
                (unsigned long)ok, (unsigned long)fail,
                (unsigned long)(gSendsPerWin * 2),
                (unsigned long)gUpdLastUs, (unsigned long)gUpdMaxUs,
                (unsigned long)gLoopLastUs, (unsigned long)gLoopMaxUs);
  gUpdMaxUs = 0; gLoopMaxUs = 0; gSendsPerWin = 0;
}

static void benchAdc() {
  const int N = 400;
  volatile int s = 0;
  uint32_t t0 = micros();
  for (int i = 0; i < N; i++) s += analogRead(kAxisPin[i & 3]);
  uint32_t t1 = micros();
  for (int i = 0; i < N; i++) s += analogReadMilliVolts(kAxisPin[i & 3]);
  uint32_t t2 = micros();
  (void)s;
  Serial.printf("ADC bench: analogRead=%luus/call analogReadMilliVolts=%luus/call\n",
                (unsigned long)((t1 - t0) / N), (unsigned long)((t2 - t1) / N));
}

static inline void updateChannels() {
  rawAdc[0] = sampleAdcFast(kAxisPin[0]);
  rawAdc[1] = sampleAdcFast(kAxisPin[1]);
  rawAdc[2] = sampleAdcFast(kAxisPin[2]);
  rawAdc[3] = sampleAdcFast(kAxisPin[3]);
  pkt.ch[0] = readSource(chMap[0]);
  pkt.ch[1] = readSource(chMap[1]);
  pkt.ch[2] = readSource(chMap[2]);
  pkt.ch[3] = readSource(chMap[3]);
  pkt.ch[4] = readSource(chMap[4]);
  pkt.ch[5] = readSource(chMap[5]);
  pkt.magic = kMagic;
  pkt.crc = crc16((uint8_t *)&pkt, sizeof(pkt) - 2);
}

} // namespace

void setup() {
  setCpuFrequencyMhz(kCpuMhz);
  Serial.begin(115200);
  delay(50);

  pinMode(kSw1Pin, INPUT_PULLUP);
  pinMode(kSw2Pin, INPUT_PULLUP);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  for (int i = 0; i < 4; i++) {
    analogSetPinAttenuation(kAxisPin[i], ADC_11db);
    pinMode(kAxisPin[i], INPUT);
  }

  loadMap();

  Serial.println("\nC3 TX: keep sticks centred for centre-cal...");
  delay(500);
  calibration();
  Serial.printf("centres ADC: %u %u %u %u\n", centerAdc[0], centerAdc[1], centerAdc[2], centerAdc[3]);
  benchAdc();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(kTxPower);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_send_cb(onDataSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kRxMac, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  esp_now_add_peer(&peer);

  Serial.printf("C3 TX ready. MAC %s  cpu=%luMHz\n",
                WiFi.macAddress().c_str(), (unsigned long)getCpuFrequencyMhz());
  printMap();

  lastTx = millis();
  lastLog = millis();
}

void loop() {
  static uint32_t prevLoopUs = 0;
  const uint32_t now = millis();
  const uint32_t t1Loop = micros();

  handleSerial();

  if ((now - lastTx) >= kTxPeriodMs) {
    lastTx = now;
    const uint32_t a0 = micros();
    updateChannels();
    gUpdLastUs = micros() - a0;
    if (gUpdLastUs > gUpdMaxUs) gUpdMaxUs = gUpdLastUs;
    esp_now_send(kRxMac, (uint8_t *)&pkt, sizeof(pkt));
    gSendsPerWin++;
  }

  if ((now - lastLog) >= kLogPeriodMs) {
    lastLog = now;
    logStatus();
  }

  const uint32_t t2Loop = micros();
  gLoopLastUs = t2Loop - prevLoopUs;
  prevLoopUs = t2Loop;
  if (gLoopLastUs > gLoopMaxUs) gLoopMaxUs = gLoopLastUs;
  (void)t1Loop;
}
