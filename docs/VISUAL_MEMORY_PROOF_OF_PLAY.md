# D5 — Proof of Play (display-output evidence)

Evidence that an advertisement was shown on a BrightSign player's **presentation output**. Three signals are combined:

1. **Playback events** reported by the player application (start, completion, stop, error, actual play time).
2. **Display screenshots** of the presentation output (not the audience camera), taken during the play.
3. A **reference library** of the expected advertisement (MobileNetV2 embeddings + a 64-bit perceptual hash of reference frames).

It is optional and **off by default** (`visual_memory.pop.enabled`). It does not touch the detector, the tracker or the audience pipeline. It is
not a certified impression measurement: *Playback corroborated* means the player's report and the screenshots agree with the expected media;
it says nothing about whether anyone watched. Similarity scores are cosine similarities, not probabilities. The audience camera is never evidence.

## Architecture
```
player application (BrightScript)                 extension (root process on the player)
  roVideoPlayer.PlayFile(...)                        vm_api_server
  events  ──► SD:/pop/events.jsonl  ───tail───►        PopService.adapter  ──► libvisualmemory PopStore (pop.db)
  roVideoMode.Screenshot() ─► SD:/pop/shots/*.jpg ─►     (stamps UTC, waits for the complete JPEG, embeds on the NPU,
  jobs  ◄── SD:/pop/jobs/*.json ◄── POST /api/pop/sessions   stores the image in pop/evidence/YYYY/MM/DD/HH/<source>/,
                                                            verifies the session when it ends)
dashboard-server ──proxy /api/pop/*──► vm_api_server                  dashboard: Proof of Play page
```
* `libvisualmemory` (`include/visualmemory/pop.h`, `src/pop_store.cpp`, `src/pop_verify.cpp`) holds the schema, reference library, evidence storage,
  retention and the pure decision logic. It knows nothing about BrightScript or any player API: the adapter (`src/tools/pop_service.cpp`) is the only
  code that understands the event file. A different player only needs a different adapter.
* The BrightScript side is `scripts/pop/pop_autorun.brs` (development XT5 only; see Rollback).

## Playback mechanisms investigated
| Mechanism | Result on the XT5 (BrightSignOS 9.1.139) |
|---|---|
| **BrightScript `roVideoPlayer`, local MP4** | **Works and is the validated path.** `PlayFile({Filename:"x.mp4"})`; events through `roMessagePort`: code 3 = playing (arrives ~270 ms after the request), 8 = media ended; `roVideoEvent` has `GetInt()`/`GetData()` only (no `GetMessage()`). 1080p25 and 2160p30 H.264 both play and are captured. |
| **BrightScript `roVideoPlayer`, RTSP** | **Works for the one source tested (an H.264 1080p RTSP publisher on the LAN), with a firmware quirk:** `PlayFile({Url:"rtsp://…"})` returns true; `{Filename:"rtsp://…"}` and a plain string both return **false** (the documentation example is wrong for this firmware). `GetStreamInfo()` then reports H264 1920x1080, 29.97 fps, encapsulation RTSP. |
| **HTML5 / JS (`roHtmlWidget`, `<video>`)** | **Not validated.** The resilience extension's autorun notes that enabling `brightsign_js_objects_enabled` suppressed widget events on this firmware family; an HTML playback path would need its own event bridge and its own capture test. Treated as unsupported until tested (VM-19). |
| **Screenshot of the output** | See below. |

## Display capture (mandatory hardware validation)
* **`roVideoMode.Screenshot({filename, filetype:"jpeg", quality, width, height})` works** and captures the **video plane**: the saved image is the frame being
  played (first test: the BrightSign neon-logo animation at 3 s, then the exhibition-booth footage; Ad B: a winter landscape; RTSP: the live street scene), not a black image and
  not a graphics-only layer. It takes 290-380 ms and a 640x400 JPEG is 19-71 KB. The output is 1280x800x60p, so 16:9 media is stretched to 16:10; reference frames are
  generated at the same 640x400 geometry.
* **The call returns true before the file is complete** (once: "Premature end of JPEG file"). The adapter therefore accepts a screenshot only when it ends with the JPEG
  EOI marker and its size is unchanged between two reads (up to 4 s), otherwise it records a *failed capture* (VM-17). Found on the XT5 and covered by a host test.
* The local DWS `POST /api/v1/snapshot/` also captures the playing video (0.6-0.8 s, 1280x800, full image via `GET /api/v1/files/sd/remote_snapshots/<name>?contents`; the player rotates
  that folder). It needs DWS credentials, so D5 uses `roVideoMode.Screenshot()` from the player application instead (no credential on the player, no network hop).
* **Not validated / unsupported:** HTML5/`roHtmlWidget` playback, HLS/UDP, multi-zone layouts, HDMI input, DRM-protected content (VM-19). A capture failure is reported as
  *Capture unsupported*, never as a mismatch.
* Direct RTSP/source frames are **supplemental** evidence only (they prove what the source sent, not what reached the HDMI output).

## Playback event contract (adapter input)
One JSON object per line in `events_path`. The BrightScript application has **no wall clock** on this firmware (`roDateTime` is epoch 0, `roSystemTime` fails; VM-16), so every event
carries `mono_ms` (ms since the application started) and the adapter assigns UTC = `anchor + mono_ms`, where `anchor` is the smallest `(time read − mono_ms)` seen since the last
`boot` event. The wall-clock error is **bounded by design** at about one poll period (200 ms) plus the player's flush delay (a few ms for a flushed line); it was **not independently measured** against a reference clock on the XT5 (a quantitative check needs a clock the player and host both observe). Events read long after they were written (adapter down) are stamped late.

| `ev` | Meaning / fields |
|---|---|
| `boot` | Application start (`mode`, `output`). Re-anchors the clock. |
| `session_scheduled_run` | A job was taken: `session, job_id, media_id (played), expected_media_id, max_play_ms, interrupt_at_ms, capture_count`. |
| `play_requested` | `PlayFile` called (`ok`). |
| `play_started` | roVideoEvent 3 (`latency_ms`, `vcode`). |
| `play_ended` / `play_stopped` | Media ended (code 8) / stopped by the application (`reason`: `slot_end`, `interrupted`). |
| `play_error` | `PlayFile` false or no start event within 15 s. |
| `capture_ok` / `capture_failed` | `idx, file, target_offset_ms, actual_offset_ms, took_ms, width, height` (`reason` on failure). |
| `session_end` | `reason, actual_play_ms` (monotonic): triggers the verification. |
| `video_event` | Any other roVideoEvent code (`vcode`). |

Job files (`SD:/pop/jobs/*.json`, written by `POST /api/pop/sessions`): `media_file, max_play_ms, loop, capture_offsets_ms, interrupt_at_ms, capture_dir, capture_width/height`.
Default offsets are 10 %, 50 % and 90 % of the slot (3, 15, 27 s of a 30 s slot).

## Database (`<storage_root>/pop.db`, `PRAGMA user_version = 1`)
Separate file like the D4 object index: Argus keeps sole ownership of `visual_memory.db`.

| Table | Content |
|---|---|
| `media_assets` | `media_id`, name, `kind` (file/rtsp), `uri` (as the player is told to play it), `sha256`, codec, `duration_s`, `slot_s` (expected play length), size, fps |
| `media_reference_frames` | `media_id`, `offset_ms`, 64-bit `phash`, image path (`pop/refs/<media>/<ms>.jpg`), `model_id`, FP32 embedding (1280, L2-normalised) |
| `playback_sessions` | `session_id`, `job_id`, expected / played media, state (scheduled → running → ended), `start/end_utc_ms`, `actual_play_ms`, `slot_ms`, `end_reason`, `captures_requested` |
| `playback_events` | every event: `type`, `mono_ms`, `utc_ms`, raw `detail_json` |
| `display_capture_evidence` | per capture: offsets, `status` ok/failed, failure reason, image path (hourly partition), pHash, SHA-256, size, `took_ms`, embedding, `image_available` |
| `playback_verifications` | one row per session: status, reason, counters, similarity statistics, `engine_version`, per-frame details (JSON) |
| `pop_models` | embedding space identity (model/version/preprocessing/dimension); only vectors of the same space are compared |

**Hourly partitioning** (VM-15): evidence is written to `pop/evidence/YYYY/MM/DD/HH/<source>/<session>_<n>.jpg`; the Visual Memory frame store now uses
`frames/YYYY/MM/DD/HH/<source>/` for new frames (old stores keep working because the database stores full relative paths). **Retention** is independent of the frame store:
`pop.evidence_max_bytes` and `pop.evidence_retention_days` remove screenshots (oldest first) while rows, embeddings and scores are kept; the dashboard then shows "no longer retained".

## Verification engine (`verify_playback`, pure function)
For each usable screenshot: best cosine similarity to the **expected** media's reference frames (and the offset of that reference), best similarity to every **other** registered media, and the pHash
distance. A frame *matches* at similarity ≥ `match_sim` unless another media beats it by `margin`; it is a *mismatch* if another media matches clearly better or it is below `reject_sim`;
otherwise *inconclusive*. Session status, in this priority order:

| Status | Rule |
|---|---|
| Scheduled | nothing reported, no screenshot |
| **Mismatch** | more than half of the usable screenshots mismatch and fewer than `min_match_fraction` match |
| **Incomplete** | the session ended without a complete play: reason not `slot_end`/`ended`, or played < `min_play_ratio` × slot (an interrupted play is incomplete even if the frames match) |
| **Capture unsupported** | playback reported, but no screenshot could be obtained (all captures failed) |
| **Playback corroborated** | complete playback events **and** ≥ `min_match_fraction` of the usable screenshots match |
| Visual match | screenshots match but the events do not (yet) show a complete play |
| Playback reported | playback reported; screenshots missing or inconclusive |

Partial screenshot loss does not change the verdict; the reason notes how many failed. Defaults: `match_sim 0.80`, `reject_sim 0.60`, `margin 0.05`, `min_play_ratio 0.90`, `min_match_fraction 0.60` (VM-20). **Evidence base:** 3 screenshots of Ad A (similarity 0.900-0.992, lowest on the fast neon-logo animation at 3 s), 3 screenshots of Ad B played while A was expected (0.573-0.592), 3 RTSP screenshots (0.924-0.969 against same-time frames, 0.767-0.783 against stale ones). The gap between 0.592 and 0.900 is wide for these two clips, but nine frames from two advertisements are **not** a statistically calibrated operating point; look-alike advertisements, very short or static scenes and other content have not been tested.

## Dashboard
The **Proof of Play** tab replaces the old preview page: status/adapter chips, a *schedule a play* form (test hooks only with `allow_test_jobs`), the **playback history** (filters, CSV export), the
**advertisement registry** and a **detail view** with the verdict, the playback timeline, and for each screenshot the display image next to the closest reference frame with similarity, pHash distance and the
best other media. Re-verify and JSON export are available per session. API: `docs/VISUAL_MEMORY_DASHBOARD_API.md`.

## Validation
**Host** (synthetic images, mock embedder; plumbing only, not hardware evidence): library tests 91/91 (10 new PoP tests: pHash, decision table, schema guard, reference library, the four scenarios,
hourly partition + retention, sessions/persistence); `tests/api/vm_pop_e2e.py` runs a real `vm_api_server` through scheduling, adapter, evidence, verdicts, exports, truncated-screenshot race, reference refresh and an
adapter restart (all checks pass); Go proxy policy tests; real-browser test `tests/browser/d5_ui_cdp.py` (19/19 against the XT5, 17/17 on the host).

**XT5** (real player, real playback events, real display screenshots, real NPU embeddings; `docs/test_results/d5_proof_of_play_xt5.json`, raw events `d5_playback_events_xt5.jsonl`). Ad A = `meet-brightsign.mp4`
(121.64 s 1080p25, sha256 `e2d0c1a8…3f93a`), Ad B = `snowy_river.mp4` (15.02 s 2160p30, sha256 `885b3680…c8c6d`), both unmodified; 30 s slot; screenshots at 3 / 15 / 27 s.

| Scenario | Session | Verdict | Evidence |
|---|---|---|---|
| A. expected A, play A | `xt5-a1` | **Playback corroborated** | played 30,015 ms of 30,000; 3/3 screenshots, similarity **0.900 / 0.992 / 0.958** (best reference at 2 / 15 / 27 s for captures at 3.0 / 15.0 / 27.0 s) |
| B. expected A, play B | `xt5-b1` | **Mismatch** | 3/3 screenshots show B: similarity to A **0.586 / 0.592 / 0.573**, closest registered media `ad-b` |
| C. expected A, interrupted at 10 s | `xt5-c1` | **Incomplete** | stopped after 10,054 ms of 30,000; the one screenshot still matched A (0.900) |
| D. screenshots fail | `xt5-d1` | **Capture unsupported** | playback complete (30,016 ms), all 3 `Screenshot()` calls returned false (target directory unwritable: a real failing call); no screenshot, no embedding |
| E. RTSP, expected live-cam | `xt5-e3` | **Playback corroborated** (supplemental) | 30,064 ms; display screenshots vs frames sampled from the same stream during the play: **0.955 / 0.924 / 0.969** |
| E′. RTSP vs stale references | `xt5-e2` | Playback reported (inconclusive) | references sampled earlier: 0.767 / 0.783 < 0.80, honest limitation (VM-18); the first attempt `xt5-e1` failed with the `Filename` key (see mechanisms) |

Timeline of `xt5-a1` (player clock): scheduled run +0 ms, `PlayFile` +149 ms, started +164 ms, screenshots +3,487 / +15,532 / +27,464 ms (3,006 / 15,045 / 27,010 ms after the start), stop +30,173 ms, session end +30,179 ms.

**Performance on the XT5** (`d5_performance_xt5.json`: Argus + Visual Memory + PoP enabled, presentation playing, 50 s idle baseline vs 105 s with three back-to-back sessions):

| Metric | Baseline | During PoP sessions |
|---|---|---|
| Argus FPS (mean / min / samples < 25) | 29.06 / 28 / 0 | 29.05 / 28 / 0 |
| `attention_demo` CPU (% of one core) / RSS | 52.0 / 97.7 MB | 51.9 / 97.7 MB |
| `vm_api_server` CPU / RSS | 1.3 % / 29.9 MB | 3.7 % / 30.9 MB |
| System CPU busy (8 cores) | 8.2 % | 9.3 % |
| MemAvailable (mean) | 2934 MB | 2901 MB |
| NPU load core 0 / 1 / 2 (sampled at 1 Hz) | 79 / 0 / 0 % | 79 / 0 / 0 % |
| SoC temperature | 35.8 °C | 36.0 °C |

NPU load was sampled at 1 Hz and the PoP embeddings run in ~120 ms bursts on core 1, so the 0 % mean for core 1 means 'not visible at this sampling rate', not 'unused'. A screenshot takes 290-380 ms inside the player application (BrightScript) and SQLite writes happen in the separate `vm_api_server` process; the presentation did not stall (play time 30,015-30,510 ms of a 30,000 ms slot) and
Argus kept 29 FPS. Embedding a reference frame takes ~120 ms (122 frames in 14.5 s).

## Run it
1. Register media and reference frames (host): `scripts/pop/register_media.py --api http://<player>:20300 --id ad-a --file meet-brightsign.mp4 --uri meet-brightsign.mp4`; `scripts/pop/make_reference_frames.sh <video> <dir> 1`;
   copy the frames to `<storage_root>/pop/refs_incoming/<id>/` and `POST /api/pop/media/<id>/references/build`.
2. On the player: `pop_autorun.brs` as `autorun.brs` and an `SD:/pop/enabled` file (without it the script plays `meet-brightsign.mp4` in a loop exactly as the original presentation); `visual_memory.enabled`, `embedding.enabled`, `vector_search_enabled` and `visual_memory.pop.enabled` in the Argus configuration.
3. Schedule from the dashboard (Proof of Play → Schedule a play) or `POST /api/pop/sessions`; the verdict appears in the history within a few seconds of the end of the slot.
4. Performance: `scripts/pop/perf_sampler.py --api http://<player>:20300 --exporter http://<player>:9101`.

## State of the development XT5 after validation
The D5 package `argus-ext-1793873298.zip` stays installed; the original configuration (`fd984a47…c7c94`) and autorun (`83076b0b…609ab`) are restored and verified by hash, Visual Memory and PoP are disabled, and the rollback packages
(`…816098`, `…800886`, `…779707`) are untouched. The temporary validation data was deleted through the DWS file API (no shell, no Telnet, presentation untouched): the D5 test store (649 files, including 439 audience-camera frames written while Visual Memory was enabled),
the temporary `/storage/sd/pop/` contents (job files, shots, event logs, probe logs) and the two obsolete D5 build zips. `/storage/sd/pop/{jobs,done,shots}` remain as empty directories. A repeat of the PoP scenarios needs the temporary configuration
and `pop_autorun.brs` again (Run it, above). Anything that writes frames while Visual Memory is enabled (including the Argus writer's first sample) can capture identifiable audience images: delete such stores when the test ends.

## Rollback (development XT5)
* **Autorun:** the original presentation script is `/storage/sd/autorun.brs.pre-d5` (sha256 `83076b0b4c6b71e7d2d05bb9b226fb3ab46575ef4d3a4973e923a87abd1609ab`). Restore it over `autorun.brs` (or delete `SD:/pop/enabled`), then reboot.
* **Configuration:** the original is `/storage/sd/configs/argus-config.json.pre-d5` (sha256 `fd984a47b87459bec8c7feb6b4394d4300b9f5326a836b8a2a3e5811ab6c7c94`). Copy it over `argus-config.json` and reboot.
* **Extension:** previous packages stay on the SD card (`argus-ext-1793816098.zip` = the D1-D3 build (D4 was only run from /tmp on spare ports and is first installed as part of the D5 package, disabled by default), `…800886`, `…779707`). In a root shell: `bsext_init stop`, `cd /usr/local && unzip -o /storage/sd/<zip> && bash ./ext_npu_argus_install-lvm.sh`, then a **full reboot**
  (the DWS call `PUT /api/v1/control/reboot` or `reboot`; leaving the root shell with `exit` restarts only the player software and does not re-mount the replaced extension volume).
* Data written by D5 lives in `<storage_root>/pop.db` and `<storage_root>/pop/`; deleting them removes all D5 data and nothing else.

## Privacy of the data D5 creates
Screenshots, reference frames and (with Visual Memory enabled) the audience-camera frames can show identifiable people: the reference clips and the RTSP source used in the validation contain people. `pop/evidence/` and `pop/refs/` are
subject to the retention settings above, but `pop/refs/` and `pop/refs_incoming/` are not pruned automatically. Keep screenshots, stores and `pop.db` out of Git and delete the store directory when a validation ends.

## Limitations
See `libs/visual-memory/BACKLOG.md` VM-16…VM-20: no wall clock in BrightScript (adapter-assigned UTC), `Screenshot()` completes asynchronously, live streams need same-time references, only file and RTSP playback were validated,
thresholds are calibrated on two advertisements, and DWS credentials must never be stored in the repository (the validation read them from an owner-only file on the host).
