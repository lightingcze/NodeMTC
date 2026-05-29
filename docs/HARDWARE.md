# Hardware

## ESP8266 board

Tested conceptually for **NodeMCU (ESP8266) with CP2102**.

## MIDI OUT (DIN‑5) — recommended (reliable)

Use the ESP8266 **UART1 TX-only** output:
- `Serial1` TX pin = **GPIO2** (NodeMCU: **D4**)
- Baud rate = **31250**

### Parts

- `74HCT125` (or any **HCT** buffer with 5V supply)
- 2× `220Ω` resistors
- `DIN‑5 female` connector
- +5V (from USB/VIN) and common GND

### Wiring

- `NodeMCU D4 / GPIO2 (Serial1 TX)` → `74HCT125 1A`
- `74HCT125 1OE` → `GND` (enable, active‑low)
- `74HCT125 VCC` → `+5V`
- `74HCT125 GND` → `GND`
- `74HCT125 1Y` → `220Ω` → `DIN pin 5`
- `+5V` → `220Ω` → `DIN pin 4`
- `GND` → `DIN pin 2`

Diagram: `assets/wiring.png`.

### Notes

- `GPIO2` is a boot strap pin (must be HIGH on boot). UART idle is HIGH, so this is OK. Avoid pull-downs.
- If you see garbage on MIDI at boot, remove debug prints on UART0 and/or avoid powering the target MIDI device during flashing.

