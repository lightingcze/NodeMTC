# Web UI

## Endpoints

- `/` — status dashboard (live-updating)
- `/api/status` — JSON status
- `/wifi` — start Wi‑Fi/device-name configuration portal
- `/api/reboot` (POST) — reboot device
- `/api/wifi_reset` (POST) — wipe stored Wi‑Fi credentials and reboot

## Timecode display preferences

On `/`, the timecode display has preferences:
- Size: Small / Medium / Large / Huge
- Style: Mono / Rounded / Digital
- Refresh rate: 100–1000ms

Preferences are saved in the browser via `localStorage`.

## MTC state indicator

The API returns `mtc_state`:
- `none` — no MTC received yet
- `running` — fresh MTC updates
- `stale` — updates are delayed (possible Wi‑Fi jitter / sender stopped)
- `stopped` — no updates for a longer time

