# ESP8266 D1 mini — RC PPM Receiver Pinout

## Board layout (USB up, silkscreen labels)

```
                             WeMos D1 mini (ESP8266)
        ┌──────────────────────────────────────────────────────────┐
        │                         [  USB  ]                        │
        │      ╔═══════════ D1 mini GPIO legend ═══════════╗       │
        │      ║  → PPM OUT  =  D2 (GPIO4)               ║       │
        │      ║  → LINK LED  =  D7 (GPIO13)              ║       │
        │      ╚══════════════════════════════════════════╝       │
        │                                                          │
   ┌────┴───────────┬───────────────────────────┬──────────────────┴───┐
   │  RST           │                           │  TX                  │
   │  A0            │     ESP8266               │  RX                  │
   │  D0 (GPIO16)   │                           │  D1 (GPIO5)          │
   │  D5 (GPIO14)   │                           │  D2 (GPIO4)  →→→ PPM │
   │  D6 (GPIO12)   │                           │  D3 (GPIO0)          │
   │  D7 (GPIO13)  → LED                        │  D4 (GPIO2)          │
   │  D8 (GPIO15)   │                           │  GND                 │
   │  3V3           │                           │  3V3                 │
   │  5V (VIN)      │                           │  5V (VIN)            │
   │  GND           │                           │  GND                 │
   └────────────────┴───────────────────────────┴──────────────────────┘
```

## Connections

| Function          | D1 mini pin | Notes                                              |
|-------------------|-------------|----------------------------------------------------|
| **PPM signal OUT**| **D2 · GPIO4** | goes to flight controller / FC PPM input           |
| **Link LED (+)**   | **D7 · GPIO13** | 100 Ω series resistor to LED red anode (optional) |
| Link LED (−)       | GND          | LED cathode to GND                                  |
| Power +            | 5V / VIN     | from battery BEC (5–6V); NOT 5V of servo supply if noisy |
| Power −            | GND          | common ground with servos/BEC                      |

Servo power (5–6 V) comes from the BEC/ESC, **not** from the ESP8266.
Only the **signal** wires use the GPIOs; ESP8266 can be powered from the same BEC via 5V/VIN or its own USB.

## Wiring to a Flight Controller (PPM in)

```
D1 mini D2 (GPIO4) ────── FC PPM input pin
          GND     ────── FC GND
  5V / VIN       ────── BEC 5V (optional, if no separate supply)
```

## Programmer hookup (FTDI, one-time flash)

| D1 mini | FTDI         |
|---------|--------------|
| TX      | RXD          |
| RX      | TXD          |
| GND     | GND          |
| GPIO0   | GND (only while flashing; remove after) |
| 3V3     | 3.3V (if no battery yet) |