# NodeMTCBridge (ESP8266)

RTP-MIDI (AppleMIDI) **MTC Quarter Frame** receiver for **NodeMCU ESP8266** that forwards **only MTC QF** to **DIN MIDI OUT**.

Includes:
- Auto Wi‑Fi captive portal (first boot / when no known Wi‑Fi is available)
- Configurable `Device name` (stored in LittleFS) for web UI + mDNS hostname
- Web status UI (`/`) + JSON API (`/api/status`)

## Documentation

- `docs/PROJECT.md`
- `docs/SETUP.md`
- `docs/HARDWARE.md`
- `docs/WEB_UI.md`
- `docs/TROUBLESHOOTING.md`

## Hardware

### Recommended MIDI OUT wiring (reliable)

- Use **UART1 TX-only** on ESP8266: `D4 / GPIO2` (`Serial1`) @ `31250 baud`
- Use a **74HCT125** powered from **5V** to get a proper 5V MIDI OUT signal

**Connections**
- `NodeMCU D4/GPIO2 (Serial1 TX)` → `74HCT125 1A`
- `74HCT125 1OE` → `GND` (enable output)
- `74HCT125 VCC` → `+5V` (USB 5V/VIN)
- `74HCT125 GND` → `GND`
- `74HCT125 1Y` → `220Ω` → `DIN pin 5`
- `+5V` → `220Ω` → `DIN pin 4`
- `GND` → `DIN pin 2`

Wiring diagram: `assets/wiring.png`.

## Arduino IDE setup

1. Boards Manager: install **ESP8266 by ESP8266 Community**
2. Library Manager: install
   - **WiFiManager** (tzapu)
   - **AppleMIDI** (lathoub)
   - **MIDI Library** (FortySevenEffects)
3. Open `NodeMTCBridge/NodeMTCBridge.ino`
4. Select board: e.g. `NodeMCU 1.0 (ESP-12E Module)`
5. Upload

## First boot / Wi‑Fi configuration

- If the device cannot connect to saved Wi‑Fi credentials, it starts an AP:
  - `NodeMTC-Setup-<chipid>`
- Connect to that AP and set:
  - Wi‑Fi SSID + password
  - Device name
- After saving, it reboots and starts the normal mode.

## Web UI

- Open `http://<device-ip>/`
- Optional: `http://<device-name>.local/` (mDNS, depends on your network)
- JSON: `http://<device-ip>/api/status`

## RTP-MIDI / MTC

- RTP‑MIDI session name is **fixed at compile time** to `NodeMTC`
- The bridge forwards **only** MTC Quarter Frame (`0xF1`) to DIN MIDI OUT

## Files

- Sketch: `NodeMTCBridge/NodeMTCBridge.ino`
- Diagram: `NodeMTCBridge/assets/wiring.png`
