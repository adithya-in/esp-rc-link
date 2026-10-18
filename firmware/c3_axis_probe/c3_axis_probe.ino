#include <Arduino.h>

const int PINS[4] = {0, 1, 3, 4};
const char *NAMES[4] = {"J1.X", "J1.Y", "J2.X", "J2.Y"};
const int SW1 = 5;
const int SW2 = 6;

int mn[4];
int mx[4];

static void wiringTest() {
  Serial.println("=== WIRING TEST (connected pot = small swing, open wire = big swing) ===");
  for (int i = 0; i < 4; i++) {
    pinMode(PINS[i], INPUT_PULLDOWN);
    delay(250);
    int dn = analogReadMilliVolts(PINS[i]);
    pinMode(PINS[i], INPUT_PULLUP);
    delay(250);
    int up = analogReadMilliVolts(PINS[i]);
    pinMode(PINS[i], INPUT);
    delay(250);
    int fl = analogReadMilliVolts(PINS[i]);
    const char *verdict = (up - dn > 2000) ? "OPEN/FLOATING" : "CONNECTED";
    Serial.printf("WIRE %s: pulldown=%4dmV pullup=%4dmV float=%4dmV  swing=%4d  -> %s\n",
                  NAMES[i], dn, up, fl, up - dn, verdict);
  }
  Serial.println("=== end wiring test ===");
}

void setup() {
  Serial.begin(115200);
  delay(400);
  analogSetAttenuation(ADC_11db);
  pinMode(SW1, INPUT_PULLUP);
  pinMode(SW2, INPUT_PULLUP);
  for (int i = 0; i < 4; i++) {
    mn[i] = 5000;
    mx[i] = -1;
  }
  Serial.println();
  Serial.println("=== AXIS PROBE ready ===");
  wiringTest();
  Serial.println("Move each axis to BOTH extremes, then center.");
}

void loop() {
  static uint32_t lastFast = 0;
  static uint32_t lastRange = 0;

  if (millis() - lastFast >= 100) {
    lastFast = millis();
    Serial.print("raw:");
    for (int i = 0; i < 4; i++) {
      int r = analogRead(PINS[i]);
      int mv = analogReadMilliVolts(PINS[i]);
      Serial.printf(" %s=%d/%dmV", NAMES[i], r, mv);
    }
    Serial.printf(" SW1=%d SW2=%d\n", digitalRead(SW1), digitalRead(SW2));
  }

  for (int i = 0; i < 4; i++) {
    int mv = analogReadMilliVolts(PINS[i]);
    if (mv < mn[i]) mn[i] = mv;
    if (mv > mx[i]) mx[i] = mv;
  }

  if (millis() - lastRange >= 2000) {
    lastRange = millis();
    Serial.print("range:");
    for (int i = 0; i < 4; i++) Serial.printf(" %s=%d..%d", NAMES[i], mn[i], mx[i]);
    Serial.println();
  }
}
