#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <user_interface.h>

#define MAGIC 0x52C7A5E1UL
#define CH_COUNT 6

struct __attribute__((packed)) RcPacket {
  uint32_t magic;
  uint16_t ch[CH_COUNT];
  uint16_t crc;
};

// PPM output on GPIO4 (D2), link LED on GPIO13 (D7)
#define PPM_PIN 4
#define LED_PIN 13

// 1 = inverted / negative-pulse PPM (idle high, channel = LOW pulse).
//    APM 2.x (3DR PPM encoder) expects negative-pulse PPM.
// 0 = standard positive CPPM (pulses high, idle low).
#define PPM_INVERT 1

// ArduPPM / APM2 format: the ATmega2560 decoder measures the period between
// RISING edges and treats that as the channel value. So each channel must be
// a short fixed pulse plus a gap whose SUM equals the channel value:
//     period = pulse + gap = channel value (1000..2000us)
// A frame is N channels followed by one long (>4ms) sync period.
#define FRAME_US 20000UL     // 20 ms -> 50 Hz
#define GAP_US   400         // fixed gap portion of each channel
#define PPM_MIN  800
#define PPM_MAX  2200

static volatile uint16_t curCh[CH_COUNT] = {1500, 1500, 1500, 1500, 1000, 1000};
static const   uint16_t failsafeCh[CH_COUNT] = {1500, 1500, 1500, 1500, 1000, 1000};

static volatile uint32_t pktGood = 0;
static volatile uint32_t pktBad  = 0;
static volatile uint8_t  havePkt = 0;
static volatile uint8_t  pktBuf[sizeof(RcPacket)];

static uint16_t ppmCh[CH_COUNT] = {1500, 1500, 1500, 1500, 1000, 1000};
static uint32_t lastPktMs = 0;

// ---- PPM generator (ArduPPM-compatible), timer1 ISR ----
// Segment order matches the canonical 3DR ArduPPM encoder so that the period
// between rising edges equals the channel value:
//   [sync][gap][ch0][gap][ch1][gap]...[chN-1][gap]
// Even segments are LOW, odd segments are HIGH (PPM_INVERT=1).
#define PPM_NSEG (2 * CH_COUNT + 2)
static volatile uint16_t gDur[PPM_NSEG];
static volatile uint8_t  gSeg = 0;

static void ICACHE_RAM_ATTR ppmISR() {
  uint8_t i = gSeg;
#if PPM_INVERT
  if (i & 1) GPOS = (1 << PPM_PIN); else GPOC = (1 << PPM_PIN);
#else
  if (i & 1) GPOC = (1 << PPM_PIN); else GPOS = (1 << PPM_PIN);
#endif
  uint32_t d = gDur[i];
  if (++i >= PPM_NSEG) i = 0;
  gSeg = i;
  timer1_write(d * clockCyclesPerMicrosecond());
}

static void buildDur() {
  uint32_t used = 0;
  for (int i = 0; i < CH_COUNT; i++) {
    uint16_t v = ppmCh[i];
    if (v < PPM_MIN) v = PPM_MIN;
    if (v > PPM_MAX) v = PPM_MAX;
    gDur[2 * i + 1] = GAP_US;          // fixed gap AFTER each channel
    gDur[2 * i + 2] = v - GAP_US;      // channel pulse: pulse + gap == v
    used += v;                         // rising-edge-to-rising-edge period
  }
  gDur[PPM_NSEG - 1] = GAP_US;         // final gap before the sync
  int32_t sync = (int32_t)FRAME_US - (int32_t)used - (int32_t)GAP_US;
  if (sync < 4000) sync = 4000;        // APM2 needs a >4000us sync period
  gDur[0] = sync;                      // long LOW sync at the frame start
}

static uint16_t crc16(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) {
      if (crc & 1) crc = (crc >> 1) ^ 0xA001;
      else crc >>= 1;
    }
  }
  return crc;
}

void ICACHE_RAM_ATTR recvCb(uint8_t *mac, uint8_t *data, uint8_t len) {
  if (len != sizeof(RcPacket) || havePkt) return;
  const uint8_t *s = data;
  for (uint8_t i = 0; i < sizeof(RcPacket); i++) pktBuf[i] = s[i];
  havePkt = 1;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PPM_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(PPM_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  wifi_set_channel(1);
  delay(100);

  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_SLAVE);
  esp_now_register_recv_cb(recvCb);
  wifi_set_channel(1);

  // Start the PPM generator.
  buildDur();
  gSeg = 0;
  timer1_isr_init();
  timer1_attachInterrupt(ppmISR);
  timer1_enable(TIM_DIV1, TIM_EDGE, TIM_SINGLE);
  ppmISR();  // kick the first segment

  Serial.printf("RX channel=%u\n", (unsigned)wifi_get_channel());
  Serial.print("RX ready. MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println("PPM out on GPIO4 (D2), LED on GPIO13 (D7)");
}

void loop() {
  if (havePkt) {
    havePkt = 0;
    const RcPacket *pkt = (const RcPacket *)(const void *)pktBuf;
    if (pkt->magic == MAGIC && crc16((const uint8_t *)pkt, sizeof(RcPacket) - 2) == pkt->crc) {
      for (int i = 0; i < CH_COUNT; i++) curCh[i] = pkt->ch[i];
      pktGood++;
      lastPktMs = millis();
    } else {
      pktBad++;
    }
  }

  const bool lost = (millis() - lastPktMs) > 300;
  digitalWrite(LED_PIN, lost ? LOW : HIGH);

  static uint32_t lastApply = 0;
  if (millis() - lastApply >= 20) {   // refresh PPM channels at ~50 Hz
    lastApply = millis();
    if (lost)
      memcpy(ppmCh, failsafeCh, sizeof(ppmCh));
    else
      for (int i = 0; i < CH_COUNT; i++) ppmCh[i] = curCh[i];
    buildDur();
  }

  static uint32_t lastLog = 0;
  if (millis() - lastLog >= 500) {
    lastLog = millis();
    Serial.printf("good=%u bad=%u link=%s ch:",
                  (unsigned)pktGood, (unsigned)pktBad, lost ? "LOST" : "OK");
    for (int i = 0; i < CH_COUNT; i++) Serial.printf(" %u", ppmCh[i]);
    Serial.println();
  }
}
