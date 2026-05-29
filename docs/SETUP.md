# Setup (Arduino IDE)

## Requirements

- Arduino IDE
- ESP8266 board support
- Libraries:
  - **WiFiManager** (tzapu)
  - **AppleMIDI** (lathoub)
  - **MIDI Library** (FortySevenEffects)

## Steps

1. Arduino IDE → **Boards Manager** → install **ESP8266 by ESP8266 Community**
2. Arduino IDE → **Library Manager** → install:
   - `WiFiManager`
   - `AppleMIDI`
   - `MIDI Library`
3. Open `NodeMTCBridge/NodeMTCBridge.ino`
4. Select board: `NodeMCU 1.0 (ESP-12E Module)` (or your exact NodeMCU variant)
5. Select correct COM port and upload.

## First boot (captive portal)

If the device cannot connect to previously stored Wi‑Fi credentials, it will start an AP:

- `NodeMTC-Setup-<chipid>`

Connect to it and configure:
- Wi‑Fi SSID / password
- `Device name` (used for web UI + mDNS hostname)

After saving, the device reboots into normal mode.

