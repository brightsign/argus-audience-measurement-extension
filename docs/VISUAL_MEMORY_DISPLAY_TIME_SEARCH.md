# Time-based display history lookup ("Search by time", "Get display frame by timestamp")

Extension of Phase 2 ([VISUAL_MEMORY_DISPLAY_HISTORY_PHASE2.md](VISUAL_MEMORY_DISPLAY_HISTORY_PHASE2.md)). A user picks a date and time and sees what the BrightSign presentation was playing then, according to the Proof-of-Play (PoP) database: the sessions the player reported, the stored Weston/VOP2 display frames nearest that time, and the verification results, with what the **player reported** kept apart from what the **stored frames show**.

It reads stored timestamps, stored vectors and the stored JPEG files. It runs no model, so it works while the NPU is busy and costs milliseconds.

## 1. What it does and does not claim

* It answers from **retained evidence only**. Display frames exist for registered PoP advertisement sessions (the three verification checkpoints and, while `visual_memory.pop.history.enabled`, about one frame per second). The idle presentation is never stored.
* An empty answer therefore says *"no retained display evidence for that time"* and gives the reasons that apply (older than the oldest retained frame, newer than the newest, in a gap between sessions, history not recording, nothing retained at all). It always ends with *"This does not establish that nothing was displayed."* It never says the screen showed nothing.
* If the player reported a session at that time but no frame of it is retained, the session is listed, marked *not visually corroborated by a stored frame*.
* "Player reported" (events from the player application: expected advertisement, reported advertisement, start, end, reason) and "visually corroborated" (the D5 verdict from the three checkpoints, plus the similarity of each stored frame to the expected advertisement) are separate fields and separate blocks in the UI. A session where the player reported the scheduled advertisement but the checkpoints show another one shows both facts and `visual_verdict: contradicts_expected`.
* The D5 three-checkpoint verification is unchanged. This feature only reads its results.

## 2. API

All three routes are `GET`, read-only, and sit behind the dashboard proxy's token and same-origin policy like every other `/api/display/` and `/api/pop/` route. Without Proof of Play enabled they answer `503 pop_disabled`.

### `GET /api/display/history/nearest`

| parameter | meaning |
|---|---|
| `time` | **required** ISO-8601 with an explicit offset: `2026-11-07T14:30:00Z`, `...T15:30:00.250+01:00`. A wall-clock time without `Z` or `+-HH:MM` is refused with 400 (it would be ambiguous). `utc_ms=<epoch ms>` is accepted instead of `time`. A `+` that an unencoded query string turned into a space is accepted. |
| `max_diff_ms` | tolerance, 0 to 3,600,000, default 2000 (inclusive) |
| `session_id` | optional: only frames of this session |

Selection: the frame whose `recorded_utc_ms` is nearest the requested time (smaller absolute difference wins; an exact tie resolves to the **earlier** frame, then history before checkpoint, then the lowest id), within the tolerance. Only Weston/VOP2 display frames (`capture_method = resilience_grpc`) are candidates: history rows plus verification checkpoints that history does not already link. A checkpoint that history links is reported once, as the history row. BrightScript screenshots and audience-camera frames are never returned.

Response when a frame is found (`found: true`, `evidence_state: frame_found` or `frame_found_image_expired`):

```json
{
  "found": true,
  "requested": {"utc_ms": 1794061800120, "utc": "2026-11-07T14:30:00.120Z"},
  "max_diff_ms": 2000,
  "frame": {
    "frame_id": 42, "evidence_id": null, "kind": "history_sample", "is_verification_checkpoint": false, "index": 17,
    "session_id": "pop-...",
    "requested_utc_ms": 1794061800120, "requested_utc": "2026-11-07T14:30:00.120Z",
    "recorded_utc_ms": 1794061800000, "recorded_utc": "2026-11-07T14:30:00.000Z",
    "time_diff_ms": -120,
    "time_uncertainty_ms": 252, "timestamp_quality": "bounded", "time_diff_range_ms": [-372, 132],
    "timing_note": "...",
    "playback_offset_ms": 16920,
    "expected_media_id": "ad-a",
    "width": 640, "height": 360, "bytes": 31822, "sha256": "...",
    "provenance": {"origin": "BrightSign display output (Weston/VOP2), not the audience camera", "source": "main_display",
                   "capture_method": "resilience_grpc", "detail": "weston_vop2_writeback ..."},
    "image": {"available": true, "state": "available", "url": "/api/pop/history/42/image", "message": ""},
    "image_url": "/api/pop/history/42/image",
    "similarity": {"available": true, "similarity_to_expected": 0.97, "best_reference_offset_ms": 16000, "reference_frames": 14,
                   "match_threshold": 0.8, "matches_expected_reference": true, "basis": "..."}
  },
  "session": {
    "session_id": "pop-...", "session_url": "/api/pop/sessions/pop-...",
    "playback": {"expected_media_id": "ad-a", "reported_media_id": "ad-a", "player_reported": true, "reported_matches_schedule": true,
                 "state": "ended", "end_reason": "slot_end", "start_utc_ms": ..., "end_utc_ms": ..., "start_utc": "...", "end_utc": "...",
                 "actual_play_ms": 30100, "slot_ms": 30000, "requested_offset_ms": 17000, "requested_within_reported_play": true,
                 "basis": "reported by the player application (playback events); not derived from the images"},
    "visual_verification": {"status": "playback_corroborated", "status_label": "Playback corroborated", "reason": "...",
                            "visual_verdict": "matches_expected", "visually_corroborated": true,
                            "checkpoints": {"ok": 3, "matched": 3, "mismatched": 0, "failed": 0}, "mean_sim_expected": 0.94, "min_sim_expected": 0.93,
                            "basis": "the three verification checkpoints (3, 15, 27 s) compared with the expected advertisement; unchanged by display history"}
  },
  "coverage": {"frames_retained": 1234, "oldest_utc": "...", "newest_utc": "...", "history_recording": true},
  "evidence_state": "frame_found", "message": "Nearest stored display frame returned."
}
```

* **Timing.** All storage is UTC epoch milliseconds; `*_utc` fields are ISO-8601 `Z`. `time_diff_ms` is recorded minus requested (negative = the frame is earlier). `time_uncertainty_ms` is the stated bound of the capture instant; `null` with `timestamp_quality: "unknown"` means the capture time has no stated bound and must be treated as approximate. When it is bounded, `time_diff_range_ms` gives the interval the true difference lies in. Matching uses the recorded instant, not the interval.
* **Offset.** `playback_offset_ms` is the stored offset of the frame into the play, relative to the player-reported start (about +/-0.25 s, see the Phase 2 risks); it is not a position in the video file.
* **Similarity.** Cosine similarity of the frame's stored vector to the expected advertisement's reference frames (display geometry for Weston frames). `available: false` with a `reason` when the vector is still pending. It is an observation, not proof.
* **Expired JPEG** (`frame_found_image_expired`): the row, vector and metadata are returned; `image.available` is false, `image.state` is `expired`, `image.url` is null, and the image endpoint answers `410 image_expired`.

Response when nothing is within the tolerance (HTTP 200, `found: false`, `evidence_state: no_evidence_in_tolerance`): `message` (reasons and the closing "does not establish that nothing was displayed"), `nearest_outside_tolerance` (the closest retained frame and how far away it is), `playback_sessions_at_time` (sessions the player reported at that instant, if any) and `coverage`. `store_unavailable` is returned when pop.db is schema 1 or closed.

### `GET /api/display/history/search`

`time`, `window_s` (1 to 3600, default 30, the window is +/- that many seconds around `time`), `limit` (frames per session, 1 to 600, default 60). Returns every PoP session overlapping the window (reported start..end; a session that never ended is bounded by start plus its slot; sessions scheduled in the window that never started are flagged `scheduled_but_not_started`) with the same `playback` and `visual_verification` blocks, and for each its stored frames in time order, each with the frame fields above. A session whose frames exceed `limit` keeps the frames nearest the requested time and sets `frames_truncated`. `nearest_frame_index` points at the nearest frame of the session; the top-level `nearest` names the overall nearest frame. `evidence_state` is `frames_retained`, `sessions_without_frames` or `no_evidence`.

### `GET /api/display/history/info`

Device timezone (`visual_memory.pop.device_timezone`, else `TZ`, else `/etc/timezone`, else `UTC` flagged as a default), retained range, retention settings and the limits.

### Image bytes

The JSON never carries image bytes. `image.url` is a same-origin path served by the existing authenticated routes: `/api/pop/history/<frame_id>/image` for history rows, `/api/pop/sessions/<session>/evidence/<id>/image` for a checkpoint without a history row. Both return the stored `image/jpeg` unchanged.

## 3. Time and timezones

* The database stores UTC epoch milliseconds everywhere (`utc_ms`); nothing local is stored.
* The player does not know its own timezone name reliably, so the UI offers: **device timezone** (from `visual_memory.pop.device_timezone`, e.g. `Europe/London`), **this browser's timezone**, **UTC**, and any IANA name. The browser converts the chosen wall-clock time to a UTC instant with `Intl` (DST aware) and sends only ISO-8601 `Z` to the player. The page shows the exact instant it will search.
* A local time that happens twice (clocks go back, e.g. 2026-10-25 01:30 in London) is flagged and the user picks the first or second occurrence. A local time that does not exist (clocks go forward) is flagged and the first instant after the change is used.

## 4. Storage and indexes

No table is added or changed. Reused: `playback_sessions`, `playback_events`, `playback_verifications`, `display_capture_evidence`, `display_history_frames`, and the JPEG files under `pop/evidence/` and `pop/history/`. `PRAGMA user_version` stays 2.

Lookups are SELECT-only. Each side of the requested time is found with an index probe on `utc_ms` (`ix_pop_hist_time`, `ix_pop_evd_time`, both existing) followed by a probe for the lowest id at that instant, so the choice is deterministic without sorting the table. Two optional indexes are created, idempotently, by `history_open()` (so never on a schema-1 file): `ix_pop_sess_start` on `playback_sessions(start_utc_ms)` and the partial index `ix_pop_hist_evidence` on `display_history_frames(evidence_id) WHERE evidence_id IS NOT NULL`. An older binary ignores them.

## 5. Tests

See section 6 for the results.

* libvisualmemory `PopTimeLookup` (7 tests): deterministic nearest and tie rule, tolerance inclusive, identical instants resolve to the lowest id, only Weston frames and a linked checkpoint reported once, expired JPEG keeps metadata, window ordering and truncation, similarity from stored vectors (and missing-vector reason), read-only (pop.db byte-identical after lookups) and query plans use the timestamp indexes.
* `tests/browser/display_history_cdp.py` section G (API and UI) on the real chain with synthetic advertisements.

## 6. Results

**Host** (mock embedder, real gRPC chain, synthetic advertisements)

* libvisualmemory: complete suite passes, including 7 new `PopTimeLookup` tests.
* `display_history_cdp.py`: 144/144 (the 84 earlier checks plus 60 new G checks: nearest selection and tie rule, UTC and ISO-offset parsing including an unencoded `+`, 400s, wrong-ad / interrupted / expired-JPEG sessions, window search, read-only pop.db hash, device timezone, and the UI: London to UTC, New York/London DST, ambiguous and non-existent local times, nearest card, no-evidence page, expired image, session link, lightbox).
* Regression: Phase 1 `display_search_cdp.py` 54/54, D5 `pop_demo_cdp.py` 43/43.

**XT5** (BrightSignOS 9.1.139, real player, package `argus-ext-1794063462.zip`, sha256 `1bc8c9bf3dff60b9...`)

* API 16/16 (`hw_time_api.py`) against the stored real frames and a fresh session `s-1794063317056-a45f` (Verify Correct Ad run just before): nearest returns the frame 130 ms off with the exact difference, UTC times, uncertainty (252 ms), `resilience_grpc` provenance, the session, offset 15.09 s, 640x360, similarity 0.99 from the stored vector; the JPEG is served by the same-origin route and its SHA-256 equals the stored one; four identical answers between two frames; no-evidence, before-oldest, future and missing-offset cases answer as specified.
* UI 11/11 (`hw_time_ui.py`, headless Chrome on the player's dashboard): a London local time resolves to the right UTC instant, the nearest frame card shows all metadata, the player-reported and visual blocks are separate, the viewer opens, the session window marks the nearest frame, "Open Proof-of-Play session" opens the session with its checkpoints, and a time 30 days back shows "No retained display evidence ... does not establish that nothing was displayed".
* The fresh session was **Playback corroborated** (3 of 3 checkpoints) with 29 of 29 history frames embedded: D5 verification and 1 FPS recording are unchanged.
* Latency (lookups use stored data only): nearest 9 to 27 ms, window search 54 to 57 ms (the first call after a boot, 130 ms).

**Install notes.** The package changes `vm_api_server` and the dashboard only. The active config gained one key, `visual_memory.pop.device_timezone = "Europe/London"` (the player's timezone from its DWS), and a backup of the previous file is `/storage/sd/configs/argus-config.json.pre-timesearch` (sha `3b52aaef...`). Autorun is unchanged. Rollback: reinstall `argus-ext-1793976273.zip` and copy the backup config back; the extra indexes are ignored by older binaries.

**Known limits.**

* A frame's time is its capture instant with the stated uncertainty (about 250 ms on the XT5; the figure is a configured budget, not a measurement). Differences smaller than that are not meaningful.
* With a wall-clock input the user must choose the right zone; the page always shows the exact UTC instant it searches. The device timezone is a config value, not read from the OS.
* The in-tolerance no-evidence message says "no frame of them is retained" for a session that has frames slightly outside the tolerance; the nearest-frame distance is stated in the same message.
* Frames exist only for registered Proof-of-Play sessions, so a time while the idle presentation played (or before history recording was enabled) has no evidence by design.
