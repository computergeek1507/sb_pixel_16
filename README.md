# SBPixel16

A 16-port WS2812 pixel controller built on the **ESP32-P4**. It receives lighting
data over Ethernet (E1.31/sACN or DDP) and drives all 16 LED strips simultaneously
using the ESP32-P4's PARLIO peripheral for glitch-free parallel DMA output.

> **⚠️ WIP — Do not use.** Both the hardware and firmware are under active
> development and have not been fully validated.

This repository contains both the open-source **hardware** (KiCad 10) and the
**firmware** (Arduino / ESP-IDF).

---

## Features

- **16 parallel pixel outputs** — all strips clocked out together via PARLIO DMA,
  no RMT channel juggling.
- **Network input** — E1.31 (sACN, UDP 5568) or DDP (UDP 4048), selectable at runtime.
- **Single-universe DMX512 output** — optional, generated on UART1 (250 kbaud, 8N2).
- **Web UI + REST API** — configure network, protocol, and per-port settings from a
  browser; config persists in LittleFS as `/config.json`.
- **Over-the-air updates** — firmware and filesystem images can be flashed from the web UI.
- **Per-port configuration** — pixel count, start channel, color order, null/sacrificial
  pixels, and pixel grouping.
- **On-board 128×64 SSD1306 OLED** — shows IP, protocol, FPS, packet count, and input
  voltages.
- **Two input-voltage monitors** — oversampled ADC readings (V1/V2) shown on the OLED.
- **Built-in test patterns** — cycle red / green / blue / rainbow with the on-board
  buttons; power-up RGB self-test on every boot.

---

## Repository layout

```
.
├── firmware/SBPixel16/     Arduino sketch (ESP32-P4)
│   ├── SBPixel16.ino        main app: setup/loop, config, Ethernet, parsers
│   ├── SBPixel16.h          board pin map & config structs
│   ├── parlio_ws2812.h      PARLIO parallel WS2812 DMA driver
│   ├── dmx_output.h         single-universe DMX512 output
│   └── page_*.h             web UI pages + REST API handlers
├── hardware/                KiCad 8 project (schematics, PCB, libraries)
│   └── ESP32-P4_16.*        board design files
└── LICENSE                  CC BY-NC 4.0 (firmware)
```

---

## Firmware

### Target

- **Board:** ESP32-P4 (Arduino-ESP32 3.x), e.g. Waveshare ESP32-P4-ETH
- **Ethernet PHY:** IP101GRI (RMII)
- **Partition:** Default 4MB with SPIFFS (1.2 MB app / 1.5 MB filesystem)

### Required libraries

Install via the Arduino Library Manager:

| Library | Source |
| --- | --- |
| ESPAsyncWebServer | github.com/me-no-dev/ESPAsyncWebServer |
| AsyncTCP | github.com/me-no-dev/AsyncTCP |
| ArduinoJson | arduinojson.org |
| Adafruit GFX + Adafruit SSD1306 | Adafruit (OLED display) |

### Build & flash

1. Install the ESP32 board package (Arduino-ESP32 **3.x**) and select an **ESP32-P4** board.
2. Set the partition scheme to **Default 4MB with SPIFFS**.
3. Open `firmware/SBPixel16/SBPixel16.ino`, compile, and upload.
4. Upload the web UI: flash the LittleFS filesystem image (the web pages served from
   `/`) — a prebuilt `SBPixel16.littlefs.bin` is provided, or re-flash it later over
   the air from the web UI.

On first boot with no `/config.json`, the firmware writes sensible defaults
(DHCP on, E1.31, 170 pixels/port, RGB order).

### REST API

| Method | Endpoint | Purpose |
| --- | --- | --- |
| GET | `/api/status` | Runtime stats (IP, FPS, packet counts, voltages) |
| GET | `/api/config` | Current configuration as JSON |
| POST | `/api/config` | Update configuration |
| POST | `/api/reboot` | Reboot the controller |
| POST | `/api/ota/firmware` | Upload a firmware image |
| POST | `/api/ota/filesystem` | Upload a filesystem (web UI) image |

### Key defaults

- Refresh rate: ~40 fps (25 ms frame interval)
- Max pixels per port: 170 (configurable in `SBPixel16.h`)
- Data timeout: outputs blank after 1 s with no packets
- E1.31 UDP port 5568 · DDP UDP port 4048 · HTTP port 80

---

## Hardware

The board is a KiCad 10 open-hardware design (`hardware/ESP32-P4_16.*`) with 16 buffered
pixel outputs, dual power inputs with voltage monitoring, an SSD1306 OLED, two buttons,
and RMII Ethernet.

Design files, interactive BOM, and part list are published on the upstream project:

- **Interactive BOM:** https://computergeek1507.github.io/PB_16/ESP32-P4_16/bom/ibom
- **Part BOM (.ods):** https://github.com/computergeek1507/PB_16/raw/master/ESP32-P4_16/ESP32-P4_16_BOM.ods

![ESP32-P4 16 board](hardware/ESP32-P4_16.png)

---

## Licensing

- **Firmware** is licensed under **CC BY-NC 4.0** (see [LICENSE](LICENSE)) — free to
  share and adapt for non-commercial use with attribution.
- **Hardware** is licensed under the **CERN-OHL-S v2** (Strongly Reciprocal); derivative
  work must be publicly released. See [`hardware/README.md`](hardware/README.md).

Copyright © 2025 Scott Hanson.
