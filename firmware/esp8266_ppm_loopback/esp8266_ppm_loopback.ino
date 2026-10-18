// ESP8266 PPM loopback self-test (ArduPPM / APM2 format).
// Generates PPM on GPIO4 (D2) and reads it back on GPIO14 (D5). Jumper D2 -> D5.
// Measures the period between RISING edges, which is what the APM2 decoder
// treats as the channel value. Expected: periods == the test values below.

#include <Arduino.h>

#define PPM_PIN   4      // D2 output
#define LOOP_PIN  14     // D5 input (jumper D2 -> D5)

#define CH        6
#define GAP_US    400
#define FRAME_US  20000UL
#define SYNC_MIN  4000UL
#define PPM_INVERT 1     // even segment LOW, odd HIGH (ArduPPM negative pulse)
#define NSEG      (2 * CH + 2)

static uint16_t testCh[CH] = {1000, 1200, 1400, 1600, 1800, 2000};

static volatile uint16_t gDur[NSEG];
static volatile uint8_t  gSeg = 0;

static void buildDur() {
  uint32_t used = 0;
  for (int i = 0; i < CH; i++) {
    uint16_t v = testCh[i];
    if (v < 800) v = 800;
    if (v > 2200) v = 2200;
    gDur[2 * i + 1] = GAP_US;          // fixed gap after each channel
    gDur[2 * i + 2] = v - GAP_US;      // channel pulse: pulse + gap == v
    used += v;
  }
  gDur[NSEG - 1] = GAP_US;
  int32_t sync = (int32_t)FRAME_US - (int32_t)used - (int32_t)GAP_US;
  if (sync < (int32_t)SYNC_MIN) sync = SYNC_MIN;
  gDur[0] = sync;
}

static void ICACHE_RAM_ATTR ppmISR() {
  uint8_t i = gSeg;
#if PPM_INVERT
  if (i & 1) GPOS = (1 << PPM_PIN); else GPOC = (1 << PPM_PIN);
#else
  if (i & 1) GPOC = (1 << PPM_PIN); else GPOS = (1 << PPM_PIN);
#endif
  uint32_t d = gDur[i];
  if (++i >= NSEG) i = 0;
  gSeg = i;
  timer1_write(d * clockCyclesPerMicrosecond());
}

static volatile uint32_t rRise = 0;
static volatile uint16_t rBuf[16];
static volatile uint8_t  rIdx = 0;
static volatile uint16_t rOut[16];
static volatile uint8_t  rN = 0;
static volatile uint32_t rFrames = 0;
static volatile uint32_t rFrameStart = 0, rFrameUs = 0;

// Rising edge = start of the measured period. Period > 4 ms marks the sync.
static void ICACHE_RAM_ATTR loopISR() {
  uint32_t now = micros();
  if (rRise != 0) {
    uint32_t period = now - rRise;
    if (period > SYNC_MIN) {
      uint8_t n = rIdx;
      for (uint8_t k = 0; k < n; k++) rOut[k] = rBuf[k];
      rN = n; rIdx = 0; rFrames++;
      rFrameUs = now - rFrameStart;
      rFrameStart = now;
    } else if (rIdx < 16) {
      rBuf[rIdx++] = (uint16_t)period;
    }
  } else {
    rFrameStart = now;
  }
  rRise = now;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(PPM_PIN, OUTPUT);
  digitalWrite(PPM_PIN, LOW);
  pinMode(LOOP_PIN, INPUT);

  buildDur();
  gSeg = 0;
  timer1_isr_init();
  timer1_attachInterrupt(ppmISR);
  timer1_enable(TIM_DIV1, TIM_EDGE, TIM_SINGLE);
  ppmISR();

  attachInterrupt(digitalPinToInterrupt(LOOP_PIN), loopISR, RISING);

  Serial.printf("PPM loopback: out GPIO%d (D2) -> in GPIO%d (D5)\n", PPM_PIN, LOOP_PIN);
}

void loop() {
  static uint32_t last = 0, lf = 0;
  if (millis() - last >= 500) {
    last = millis();
    uint32_t f = rFrames;
    if (f == lf) {
      Serial.printf("NO EDGES (frames=%lu) - check jumper D2<->D5\n", (unsigned long)f);
      return;
    }
    lf = f;
    noInterrupts();
    uint8_t  n  = rN;
    uint16_t c[16];
    for (uint8_t i = 0; i < n; i++) c[i] = rOut[i];
    uint32_t fu = rFrameUs;
    interrupts();
    Serial.printf("frame=%lu period=%luus ch=%u :",
                  (unsigned long)f, (unsigned long)fu, n);
    for (uint8_t i = 0; i < n; i++) Serial.printf(" %u", c[i]);
    Serial.println();
  }
}
