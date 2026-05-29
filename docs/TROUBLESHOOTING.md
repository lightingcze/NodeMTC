# Troubleshooting

## RTP‑MIDI session not visible

- The AppleMIDI session name is `NodeMTC`.
- Make sure ESP8266 and your computer are on the same LAN (no client isolation).
- Try connecting by IP address from your RTP‑MIDI tool.

## Web UI not reachable

- Check the assigned IP in your router.
- If mDNS does not work (`.local`), use the direct IP address.

## MTC shows but DIN MIDI OUT does not work

- Verify `Serial1` is used (GPIO2 / D4) and your wiring matches `docs/HARDWARE.md`.
- Use a proper 5V MIDI OUT driver (recommended `74HCT125` + 220Ω resistors).
- Confirm the receiving device expects **MTC Quarter Frame** (not LTC).

## Boot issues / stuck boot

- GPIO2 must not be pulled LOW on boot. Remove any pull-down or short to GND on the MIDI TX line.

