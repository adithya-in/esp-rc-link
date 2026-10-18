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

constexpr int  kDeadzone = 25;
constexpr int  kPosSpan  = 1100;
constexpr int  kNegSpan  = 1500;
constexpr int  kAvg      = 8;

constexpr uint32_t kTxPeriodMs  = 20;
constexpr uint32_t kLogPeriodMs = 500;
constexpr uint32_t kCpuMhz      = 80;

constexpr wifi_power_t kTxPower = WIFI_POWER_15dBm;

struct __attribute__((packed)) RcPacket {
  uint32_t magic;
  uint16_t ch[6];
  uint16_t crc;
};

RcPacket pkt;
Preferences prefs;

uint8_t chMap[6] = {0, 1, 2, 3, 4, 5};
int centerMv[4] = {1650, 1650, 1650, 1650};
int rawMv[4] = {0, 0, 0, 0};

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

static int sampleMv(int pin) {
  uint32_t sum = 0;
  for (int i = 0; i < kAvg; i++) sum += analogReadMilliVolts(pin);
  return (int)(sum / kAvg);
}

static uint16_t axisToChannel(int idx) {
  const int d = rawMv[idx] - centerMv[idx];
  if (abs(d) < kDeadzone) return 1500;
  const int span = (d > 0) ? kPosSpan : kNegSpan;
  long out = 1500 + (long)d * 500 / span;
  return (uint16_t)(out < 1000 ? 1000 : out > 2000 ? 2000 : out);
}

static uint16_t switchToChannel(int pin) {
  return (digitalRead(pin) == LOW) ? 1000 : 2000;
}

static uint16_t readSource(uint8_t src) {
  if (src < 4) return axisToChannel(src);
  if (src == 4) return switchToChannel(kSw1Pin);
  if (src == 5) return switchToChannel(kSw2Pin);
  return 1500;
}

static void calibration() {
  uint32_t sum[4] = {0, 0, 0, 0};
  const int n = 64;
  for (int s = 0; s < n; s++) {
    for (int i = 0; i < 4; i++) sum[i] += analogReadMilliVolts(kAxisPin[i]);
    delay(3);
  }
  for (int i = 0; i < 4; i++) centerMv[i] = (int)(sum[i] / n);
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
    Serial.printf("centres: %d %d %d %d\n", centerMv[0], centerMv[1], centerMv[2], centerMv[3]);
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

static void updateChannels() {
  for (int i = 0; i < 4; i++) rawMv[i] = sampleMv(kAxisPin[i]);
  for (int i = 0; i < 6; i++) pkt.ch[i] = readSource(chMap[i]);
  pkt.magic = kMagic;
  pkt.crc = crc16((uint8_t *)&pkt, sizeof(pkt) - 2);
}

static void logStatus() {
  Serial.print("raw:");
  for (int i = 0; i < 4; i++) {
    Serial.printf(" %s=%d/%dmV", kSrcName[i], analogRead(kAxisPin[i]), rawMv[i]);
  }
  Serial.printf(" SW1=%d SW2=%d ch:", digitalRead(kSw1Pin), digitalRead(kSw2Pin));
  for (int i = 0; i < 6; i++) Serial.printf(" %u", pkt.ch[i]);
  uint32_t ok = txOk, fail = txFail;
  txOk = txFail = 0;
  Serial.printf(" tx=%lu|%lu\n", (unsigned long)ok, (unsigned long)fail);
}

}  // namespace

void setup() {
  setCpuFrequencyMhz(kCpuMhz);

  Serial.begin(115200);
  delay(150);

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
  delay(800);
  calibration();
  Serial.printf("centres mV: %d %d %d %d\n", centerMv[0], centerMv[1], centerMv[2], centerMv[3]);

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
}

void loop() {
  const uint32_t now = millis();

  handleSerial();

  if (now - lastTx >= kTxPeriodMs) {
    lastTx = now;
    updateChannels();
    esp_now_send(kRxMac, (uint8_t *)&pkt, sizeof(pkt));
  }

  if (now - lastLog >= kLogPeriodMs) {
    lastLog = now;
    logStatus();
  }
}
