# Project documentation

## What it does

`NodeMTCBridge` is a small ESP8266 (NodeMCU) firmware that:
- joins Wi‑Fi (with a captive portal for first-time setup),
- receives **RTP‑MIDI (AppleMIDI)** packets,
- forwards **only MTC Quarter Frame** messages (`0xF1`) to a **DIN MIDI OUT** connector.

It also exposes a web UI with live status and a JSON API.

## Scope / non-goals

- **Only MTC Quarter Frame** is forwarded (no MIDI notes, CC, clock, etc.).
- MIDI OUT is the only hardware MIDI port (no MIDI IN).
- The AppleMIDI **session name is fixed** to `NodeMTC` (compile-time macro in the library).

## Safety / legal

This project is intended for timecode distribution and monitoring in your own setups.

## Repository layout

- `NodeMTCBridge/NodeMTCBridge.ino` — firmware source (Arduino IDE sketch)
- `NodeMTCBridge/assets/wiring.png` — wiring diagram (recommended MIDI OUT circuit)
- `NodeMTCBridge/docs/` — documentation

