# v0.7.0

First tagged release of the Argus Audience Measurement extension for BrightSign players.

## Highlights

- **Live analytics dashboard** on the player (default `http://<player>:20300`): live
  frame with per-track overlays, KPI tiles (present, gazing, attention rate, avg
  dwell, peak, NPU/FPS), and presence/gaze history — fed over MQTT-over-WebSockets
  from the bundled mosquitto broker, no internet required.
- **Reliable multi-person tracking**: ByteTrack + Kalman core, tracking at ~10 Hz,
  decoupled from face detection so people turned away or walking away are still
  counted, with stable IDs across brief exits.

## Notable fixes

- ByteTrack core now actually emits tracks (a track-confirmation bug left `people`
  stuck at 0).
- Duplicate-ID / double-count on movement resolved by sampling association at 10 Hz.
- Enter/exit visitor events no longer dropped at the 10 Hz tracking cadence.

## Changed defaults

- Dashboard port is now **20300** (was 8081). Override via `config.json`
  (`dashboard.port`) or the `networking.bs-argus-dashboard-port` registry key.
- Config file is `config.json` (the old `argus-config.json` still loads as a fallback).

## Install

Download `argus-ext-<build>.zip` and `install-on-player.sh`, copy the zip to the
player's `/storage/sd/`, and run the bundled installer from the player's root shell
(see the README's "Install" section). The extension serves the dashboard on
`http://<player-ip>:20300`.

See [CHANGELOG.md](CHANGELOG.md) for the full list.
