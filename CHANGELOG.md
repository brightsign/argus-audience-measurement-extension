# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project aims to
follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html) (pre-1.0: minor
versions may include breaking changes).

## [0.7.0] - 2026-10-05

First tagged release.

### Added
- **Analytics dashboard**: a companion `dashboard-server` (Go) serves a live
  "mission-control" UI from `dashboard/` on its own port and reverse-proxies the
  MJPEG frame from the image-stream server so it is same-origin. The browser
  receives analytics over MQTT-over-WebSockets directly from the bundled mosquitto
  broker (a `websockets` listener on 9001, vendored MQTT.js, no internet/CDN).
- **ByteTrack tracker core** (`tracker_core = "byte"`) with a Kalman predictor, so a
  person briefly out of frame re-attaches to the same ID instead of being counted
  as a new visitor.
- Regression test `tests/test_byte_repro.cpp` guarding end-to-end track production
  by the ByteTrack core.

### Changed
- **Default dashboard port is now `20300`** (was `8081`), aligning with the player's
  `202xx` service-port range. Override via `config.json` (`dashboard.port`) or the
  `networking.bs-argus-dashboard-port` registry key.
- **Tracking now runs at ~10 Hz** (the supervisor-loop cadence) instead of once per
  analytics publish (1 Hz). At 1 Hz a moving person outran the tracker's association
  gates and spawned duplicate IDs; sampling at 10 Hz keeps per-update motion small
  enough to stay matched.
- **Person tracking is decoupled from face detection**: a person box is tracked and
  counted on its own merits (class/score/area/height gates). A face is no longer
  required, so someone turned away or walking away is still counted; faces are used
  only to attach gaze to a track.
- Published `fw_version` is now `0.7.0` to match the release.
- Config file standardized to `config.json` (from `argus-config.json`), with the old
  name accepted as a legacy fallback.
- Build system migrated to the shared `brightsign-sdk-builder` cache pattern.

### Fixed
- **ByteTrack never emitted tracks.** `update_with_bytetrack()` promoted a track to
  `Confirmed` only via `if (!bt_tr.confirmed && hits >= confirm_hits)`; since
  ByteTrack sets `confirmed` exactly at `hits >= n_init` and `n_init == confirm_hits`,
  the guard never fired and tracks stayed `Tentative` forever, so the tracker output
  (confirmed-only) was always empty and `people`/`tracks` published as 0. Changed to
  `if (bt_tr.confirmed || hits >= confirm_hits)`.
- **Enter/exit events were lost at 10 Hz.** The tracker's one-shot enter/exit flags
  fire on a single update and clear; they are now latched per track ID across each
  1 Hz publish window so `argus_visitors_total` / `argus_exits_total` are not
  undercounted.
- Updated the stale `ConfigTest` default-`log_level` expectation to match the shipped
  `"warn"` default.

[0.7.0]: https://github.com/brightsign/argus-audience-measurement-extension/releases/tag/v0.7.0
