// ESP32 PPM reader / decoder.
// Wire: ESP32 GND <-> RX (ESP8266) GND, and RX D2 (PPM) -> ESP32 PPM_PIN.
// Prints the decoded frame every 500 ms so you can prove the whole chain.

#include <Arduino.h>

#define PPM_PIN  4        // ESP32 GPIO that reads the PPM signal
#define MAXCH    16
#define SYNC_US  3000UL   // LOW gap longer than this = frame boundary
#define PPM_INVERT 0      // 1 = idle-high PPM (pulses LOW)

static volatile uint32_t riseUs     = 0;
static volatile uint32_t lastFallUs = 0;
static volatile uint16_t chBuf[MAXCH];
static volatile uint8_t  chIdx      = 0;
static volatile uint16_t chOut[MAXCH];
static volatile uint8_t  nOut       = 0;
static volatile uint32_t frames     = 0;

static void IRAM_ATTR ppmISR() {
  uint32_t now = micros();
  bool high = digitalRead(PPM_PIN);
#if PPM_INVERT
  bool start = !high;   // falling edge starts an active pulse
#else
  bool start = high;    // rising edge starts a high pulse
#endif
  if (start) {
    if (now - lastFallUs > SYNC_US && chIdx > 0) {   // frame boundary
      uint8_t n = chIdx < MAXCH ? chIdx : MAXCH;
      for (uint8_t i = 0; i < n; i++) chOut[i] = chBuf[i];
      nOut = n;
      chIdx = 0;
      frames++;
    }
    riseUs = now;
  } else {
    if (chIdx < MAXCH) chBuf[chIdx++] = (uint16_t)(now - riseUs);
    lastFallUs = now;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(PPM_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(PPM_PIN), ppmISR, CHANGE);
  Serial.printf("PPM reader: GPIO%d, CHANGE IRQ, sync>%luus, invert=%d\n",
                PPM_PIN, (unsigned long)SYNC_US, PPM_INVERT);
  Serial.println("Connect ESP32 GND to the RX (ESP8266) GND.");
}

void loop() {
  static uint32_t lastFrames = 0;
  static uint32_t lastPrint  = 0;

  if (millis() - lastPrint >= 500) {
    lastPrint = millis();
    uint32_t f = frames;
    if (f == lastFrames) {
      Serial.printf("NO PPM  (frames stuck at %lu) - check GND / wire / D2\n",
                    (unsigned long)f);
      return;
    }
    lastFrames = f;

    noInterrupts();
    uint8_t  n = nOut;
    uint16_t c[MAXCH];
    for (uint8_t i = 0; i < n; i++) c[i] = chOut[i];
    interrupts();

    Serial.printf("frame=%lu ch=%u :", (unsigned long)f, n);
    for (uint8_t i = 0; i < n; i++) Serial.printf(" %u", c[i]);
    Serial.println();
  }
}
