# The PPM format the APM2 expects

This is the detail that made the difference between "all channels 0" and a
working link, so it's worth spelling out.

## How the APM2 decodes PPM

The APM 2.x uses an ATmega2560 whose RC decoder (`RCInput_APM2.cpp`) captures on
input **ICP5 / PL1 / Arduino pin 48** using Timer5:

- It captures on the **rising edge**.
- It measures the **period from the previous rising edge** (in 0.5 µs ticks).
- If that period is **> 8000 ticks (4000 µs)** it is treated as the **frame
  sync**, otherwise it is stored as the next **channel value**.
- It needs **at least 5 channels** per frame (`AVR_RC_INPUT_MIN_CHANNELS = 5`,
  `AVR_RC_INPUT_NUM_CHANNELS = 8`).

So for the APM: **channel value == rising-edge-to-rising-edge period**, and it
must be ~1000–2000 µs.

> Note: the APM2 also has an onboard ATmega32u2 that acts as a PPM encoder
> (USB + RC passthrough). A receiver's PPM sum goes to **Input 1**, with the
> **signal pins of Inputs 2 and 3 jumpered** so the stream reaches the 2560.

## The canonical ArduPPM encoder

The 3DR / davidbuzz `ArduPPM` encoder (used by APM) toggles the output at each
entry of an array shaped like:

```
[ PPM_PRE_PULSE(400us), CH1, PPM_PRE_PULSE, CH2, ..., PPM_PRE_PULSE, CH8, PPM_PERIOD ]
```

- Each channel = a **fixed 400 µs pulse** plus the variable part, such that
  `period = 400 + (value - 400) = value`.
- `PPM_SERVO_CENTER = 1100` (= 1500 − 400), `PPM_SERVO_MAX = 1700`,
  `PPM_SERVO_MIN = 500`.
- The frame ends with a long `PPM_PERIOD` sync.

Encoded as alternating HIGH/LOW segments (starting LOW), this is:

```
LOW(sync) HIGH(400) LOW(ch1-400) HIGH(400) LOW(ch2-400) ... HIGH(400)
   ▲ rise          ▲ rise            ▲ rise
   └──── period = ch1 ──────────────┘
```

![PPM waveform](images/ppm_waveform.svg)

## What this project emits

The ESP8266 uses timer1 to walk a 14-segment table (`2*N + 2`):

```
index : 0      1    2        3    4        5   ...  12        13
value : sync   400  ch0-400  400  ch1-400  400  ...  ch5-400   400
level : LOW    HIGH LOW      HIGH LOW      HIGH ...  LOW       HIGH   (PPM_INVERT=1)
```

- `sync = FRAME_US - Σch - 400`, clamped to ≥ 4000 µs.
- Channel period = `400 + (ch - 400) = ch` → exactly what the APM measures.
- Frame = 20 000 µs → 50 Hz, 6 channels.

The `esp8266_ppm_loopback` sketch generates this and measures the periods back,
confirming `1006 1206 1406 …` for test values `1000 1200 1400 …`.
