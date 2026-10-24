# FPV camera (ESP32-CAM) — optional module

This is the camera module that was tested alongside the RC link: an
**AI-Thinker ESP32-CAM** running the Espressif *CameraWebServer* sketch adapted
for AP mode. It gives you a **live MJPEG feed** in a browser, independent of the
ESP-NOW control link.

> Status: **tested and working** as a standalone live feed. It is **not yet
> integrated** with the RC link (no shared packet, no OSD, no gimbal control).

## Files

`firmware/esp32cam_livefeed/`

| File | Purpose |
|------|---------|
| `esp32cam_livefeed.ino` | Setup: camera init, AP mode, start HTTP server |
| `app_httpd.cpp` | HTTP handlers: `/`, `/stream`, `/capture`, `/control`, `/status` |
| `board_config.h` | Selects the camera model — currently `CAMERA_MODEL_AI_THINKER` |
| `camera_pins.h` | GPIO map per camera model + flash LED pin |
| `camera_index.h` | Embedded web UI (gzipped HTML) |
| `partitions.csv` | Custom partition table (3 MB app) |
| `ci.yml` | FQBN matrix used for CI builds |

## Hardware

| Item | Value |
|------|-------|
| Board | AI-Thinker ESP32-CAM (ESP32 + OV2640) |
| PSRAM | Required for UXGA; falls back to SVGA without it |
| Flash LED | GPIO4 (PWM dimmable via `/control`) |
| Power | Stable **5 V**, ~300–500 mA peaks |
| Programming | USB-TTL adapter — the board has no USB |

## How it behaves

- Boots into **AP mode**: SSID `ESP32-CAM`, password `12345678`.
- Serves the capture UI at `http://192.168.4.1/`.
- Starts at **QVGA** for a high frame rate; you can raise it from the UI.
- With PSRAM: UXGA frames, JPEG quality 10, double-buffered, `CAMERA_GRAB_LATEST`.
- Without PSRAM: SVGA, single frame buffer in DRAM.

### HTTP endpoints

| Path | What it does |
|------|--------------|
| `/` | Embedded control UI (resolution, quality, flip, etc.) |
| `/stream` | **MJPEG stream** — point `<img>` or VLC at this |
| `/capture` | Single JPEG snapshot |
| `/control` | GET with `var=…&val=…` to change a sensor setting |
| `/status` | JSON with the current sensor settings |
| `/bmp` | BMP snapshot (no JPEG) |

Quick test once connected to the AP:

```sh
open http://192.168.4.1/          # UI
curl -o shot.jpg http://192.168.4.1/capture
```

## Build & flash

Use the classic ESP32 target with PSRAM and the custom partition table:

```sh
arduino-cli compile \
  --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=custom,FlashMode=dio" \
  firmware/esp32cam_livefeed

arduino-cli upload \
  -p /dev/cu.usbserial-XXXX \
  --fqbn "esp32:esp32:esp32:PSRAM=enabled,PartitionScheme=custom,FlashMode=dio" \
  firmware/esp32cam_livefeed
```

To program the AI-Thinker board:

1. Connect `5V`, `GND`, `U0R` (RX), `U0T` (TX) to the USB-TTL adapter.
2. Jumper **GPIO0 → GND** to enter download mode, then power-cycle / reset.
3. Upload, remove the GPIO0 jumper, and reset again.

If the board browns out or the camera fails to init (`Camera init failed with
error 0x…`), power it from a solid 5 V supply — USB-TTL 3.3 V pins are not
enough.

## Interaction with the RC link

Both the ESP-NOW link and the camera's Wi-Fi AP use the **2.4 GHz** band, so
they contend for airtime:

- The ESP-NOW link runs on **channel 1**.
- The camera AP picks a channel; keep it on the same channel as ESP-NOW to
  avoid hopping, or accept some packet loss.
- Expect the control link's range and the video frame rate to drop when both
  are active.
- MJPEG over Wi-Fi adds roughly **100–300 ms** of latency. That is fine for
  framing and recording, **not** for low-latency manual flying.

For real FPV, a dedicated 5.8 GHz analog/digital VTX is the better tool; this
module is best treated as a **situational-awareness / recording camera**.

## Not included

- No OSD overlay of channel values.
- No camera control from the configurator or the RC link.
- No integration into the ESP-NOW packet.
