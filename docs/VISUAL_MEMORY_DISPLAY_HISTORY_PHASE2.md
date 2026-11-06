# Display history (Phase 2): about one Weston/VOP2 frame per second during registered playback

Status: implemented, host-validated, installed on the development XT5 (package `argus-ext-1793969416.zip`, history recording **enabled** by the one configuration block shown in section 8). Not pushed.

## 1. Architecture

```
OS producer (5 FPS, 640x360 JPEG) -> Resilience gRPC -> argus_display_bridge -> DisplayFrameSource ring (16 frames, stale/ambiguous/duplicate rejected)
                                                              |                         |
                                        D5 checkpoint sampler (3 offsets, unchanged)    |  history sampler (new, grpc thread, ring lookups only)
                                                              |                         v
                                          display_capture_evidence              staged in memory (commit_delay_ms)  -> bounded queue (8)
                                          (verification engine reads this)                                          -> writer thread: JPEG file + row
                                                                                                                    -> same thread, NPU free: vectors
                                                                                      display_history_frames  <- searched by Visual Search "BrightSign Display"
```

Reused unchanged: OS producer and Resilience service, `argus_display_bridge`, `DisplayFrameSource` (freshness, ambiguity and duplicate rejection, producer interest `hold()` per session, so no dashboard viewer is needed), libvisualmemory JPEG store (atomic write, hourly partition), the MobileNetV2 FP16 RKNN provider (single-flight with the other NPU users), the D5 verification engine, Phase 1 search. Nothing new decodes or captures frames, and the OS, Weston and the Resilience extension are untouched.

* **Scope.** Recording exists only between `play_started` and `session_end` of a registered session (capture backend `resilience_grpc`). The idle presentation is never recorded and there is no continuous 5 FPS persistence. Default: OFF (`visual_memory.pop.history.enabled`).
* **Timeline.** Targets are `phase_ms + k * interval_ms` (default 0, 1000) after the play start, scheduled against the monotonic clock (`start_mono` = now_mono - (now_utc - play_start_utc)). Each target takes the nearest ring frame within +/-300 ms that is usable: its earliest possible display moment is after the start guard, its uncertainty is within the limit, and it is not the same frame (`seq`) as the previous sample. Targets whose whole window precedes the earliest usable moment are skipped, not counted as misses.
* **Session end.** A frame is staged in memory and committed `commit_delay_ms` (1.5 s) after its capture. `session_end` runs a final pass: open targets before the end are sampled, frames captured less than `grpc_end_guard_ms` before the end (or after it) are dropped, and a finalize job removes anything stored later than that and records the played media. Frames of the idle loop after an interruption are therefore never attributed to the session. Residual risk: the guard assumes the end event arrives within about 1.1 s of the true end.
* **No blocking.** The sampler does ring lookups on the existing display thread. Storage and embedding run on one writer thread; the hand-over queue is bounded (8) and overflow is counted. Argus capture/inference threads are never touched. Embedding uses the NPU only when the single-flight runner is free (`embed_busy` counts the retries) and checkpoints keep their retry behaviour.
* **Verification is untouched.** The verifier reads `display_capture_evidence` only. A unit test adds 30 history frames of unrelated content to a session and asserts the verdict, reason, counts and mean similarity are identical and the checkpoints are still exactly three.

## 2. SQLite decision

Dedicated additive table `display_history_frames` inside the existing schema-2 `pop.db`; `PRAGMA user_version` stays 2; no existing table or column changes.

| | |
|---|---|
| Why not reuse `display_capture_evidence` | the verifier reads every row of a session, so extra rows would change verdicts (the existing `capture_every_s` mode already does that); `UNIQUE(session_id, idx)` and the retention/quota of checkpoints are shared |
| Created | only by `history_open()` when `history.enabled` is true and the file is schema 2. A schema-1 file is never touched (unit test: byte-identical) |
| Rollback | an older binary ignores the table (`user_version` unchanged). `DROP TABLE display_history_frames` plus deleting `<storage_root>/pop/history/` removes everything; the backup of `pop.db` taken before enabling is the full rollback |
| Columns | frame_id, session_id, sec_idx (UNIQUE with session), evidence_id (reused checkpoint, nullable), source, expected_media_id, played_media_id, utc_ms, mono_ms, target_offset_ms, actual_offset_ms, uncertainty_ms, capture_method, provenance (Weston/VOP2 writeback), path, sha256, width, height, bytes, model_id, embedding BLOB (1280 x float32, L2-normalised), embedded, embed_attempts, image_available, status, created_at |
| Role | history sample = a row with `evidence_id` NULL; a history instant that is also a verification checkpoint stores `evidence_id` and **no second JPEG or vector** (the checkpoint's are reused); checkpoints themselves stay in `display_capture_evidence` |
| No double counting | search skips linked history rows and any history row whose (session, capture time) equals a stored Weston checkpoint |
| Filters | source/capture method (history is Weston/VOP2 only), session, capture-time window, advertisement (expected or played), role, require-image |
| Indexes | `ix_pop_hist_time(utc_ms)`, `ix_pop_hist_pending(embedded, embed_attempts)`; the search remains an exact scan |

A copied-database migration test is unnecessary because there is no migration; the installed demo database gets the table only when history is enabled (done on the XT5 with the backup below).

## 3. Implementation map

* `libs/visual-memory`: `PopStore::history_open/add_history_frame/embed_pending_history/list_history/history_jpeg/history_stats/history_finalize_session/prune_history`, search over both tables (`role`, `session_id` filters, coverage split). The JPEG is validated (SOI..EOI, SOF dimensions, 8 MiB) and stored unchanged; sha256 recorded.
* `src/tools/pop_service.*`: sampler (`history_begin/tick/end/abort`), writer/embedder thread, counters, `history_status()`, `history_frames()`, quota/retention in `prune()`, session-detail summary, search parameters `role` and `session_id`, `/api/pop/system` now also reports per-process `io_write_bytes` and block-device sectors for SD write-rate measurements.
* `src/tools/vm_api_server.cpp`: `GET /api/pop/history/status`, `/history/frames?session_id=&limit=&offset=`, `/history/<id>/image`; config `visual_memory.pop.history`.
* Dashboard: Display History panel and role/session filters on Visual Search (BrightSign Display); History-sample / Verification-checkpoint chips on result cards; the session detail separates "Verification checkpoints (used for the verdict)" from "Display history" thumbnails, with a "Partial history" warning; the `f1940aa` wording ("a match is an observation, not independent proof") is included.

## 4. Configuration (`visual_memory.pop.history`)

| key | default | meaning |
|---|---|---|
| enabled | false | recording on/off |
| interval_ms / phase_ms / window_ms | 1000 / 0 / 300 | cadence; phase 0 makes the 3, 15, 27 s checkpoints coincide with history targets (3 of ~29 frames reused) |
| commit_delay_ms | 1500 | staging time before a frame is committed |
| queue_capacity | 8 | hand-over to the writer |
| max_frames_per_session | 600 | per-session cap |
| max_bytes | 256 MiB | quota for history JPEGs only (separate from `evidence_max_bytes` and from the camera store in `/storage/sd/visual-memory`) |
| jpeg_retention_days / metadata_retention_days | 7 / 30 | JPEG removed first; row and vector later |
| max_rows | 50000 | bounds the exact-scan size |
| min_free_bytes | 256 MiB | recording pauses below this free space |
| max_pending_embeddings | 300 | un-embedded rows allowed before new samples are refused |
| embed_batch | 2 | vectors per NPU turn |

Pruning runs every 60 s (existing adapter cadence): JPEGs older than retention or beyond the quota go oldest first (the vector stays searchable and the card says the JPEG is no longer retained), rows older than the metadata retention or beyond `max_rows` are deleted. Files live in `pop/history/YYYY/MM/DD/HH/<source>/` under the pop storage root (the isolated `/storage/sd/pop-grpc-demo`).

## 5. Tests

Host (mock embedder, real gRPC chain; synthetic pictures only):

* libvisualmemory 120/120 including 11 new `PopHistory` tests (additive table, legacy schema-1 file byte-identical, original bytes stored, truncated JPEG/duplicate/limits/backlog, checkpoint reuse without double count, verification unchanged, search by moment/session/time/ad/role, finalize after end, quota/retention/row bound).
* `tests/browser/display_history_cdp.py` 78/78: A correct ad (29 to 30 frames, existing three-point verdict, 12 s reference found), B wrong ad (Mismatch kept, frames are the ad that really played, 15 s loop found at about 5 s and 20 s), C interrupted (no frames after the end, Incomplete), D producer stall (gap, no stale frame, partial flagged, checkpoint failed honestly), E boundaries (a reference from 40 s of a 30 s slot is not presented as a match), F storage pressure (recording pauses, checkpoints unaffected, no JPEG written), dashboard checks and screenshots.
* Regression: Phase 1 `display_search_cdp.py` 54/54, `pop_demo_cdp.py` 43/43, Go proxy tests, JS 12/12.

## 6. Hardware results on the XT5 (2026-11-06, real RKNN)

* Correct 30 s session with history: Playback corroborated, 3 of 3 checkpoints (mean 0.942, same as the earlier runs); **29 history frames**, 29 embedded, 3 reused checkpoints, offsets 0.9 s to 28.9 s, gaps 0.8 to 1.2 s, uncertainty 251 to 260 ms; one target missed (counted).
* Interrupted at 10 s: Incomplete; 9 frames, the last at 8.9 s of a 10.0 s play; nothing added afterwards.
* Argus FPS 29.08 mean (min 28, none below 25) during the session; `attention_demo` 51.8 % of a core (baseline 52.7); `vm_api_server` 4.9 %; weston 15.8 %; producer 6.1 %; bridge 0.3 %; NPU core 0 79.8 % (baseline 79.2).
* Writer: steady 20 to 60 ms per frame (JPEG fsync + SQLite insert); the very first frame took 5.05 s (directory creation and first fsync on the SD card), once. Embedding 75 ms per vector while Argus keeps the NPU busy.
* Searches (see section 7 for the exact references): a reference of Ad A at 27 s finds history frames at 27.0 to 27.1 s with 0.98 to 0.99 in several sessions; Ad B 0.99; an unrelated picture finds nothing (closest 0.63).

## 7. Search behaviour to expect

* Hits are real stored frames. A reference near a stored moment scores 0.97 to 0.99.
* Ad A at 12 s now returns frames at 11.1, 11.9 and 12.9 s with 0.80 to 0.82: the ad changes quickly, the closest stored moments are about a second away, and the play offset and the file time differ by up to about 0.5 s (decoder start latency). Treat 0.80 to 0.90 as "similar scene", not as the same frame.
* Ad A at 40 s (never played in a 30 s slot) returns two hits at 0.802 (scene similarity with frames from about 22 s). They are above the unchanged 0.80 match threshold; I did not tune the threshold. The score is shown on every card; a near-identical frame scores above 0.95. A display-only "weak similarity" band in the UI is proposed but not implemented (it would need another install).

## 8. Storage and performance budget

* Per frame: JPEG 26 KB (your earlier median) to 43 KB (measured on the 53 Weston frames of the demo ads: min 16, median 47, max 60 KB) plus a vector of 5.12 KB plus about 140 B SQLite overhead (measured 5.26 KB per row from a 100,000-row host database).
* Per 30 s session (29 frames): 0.9 MB (26 KB frames) to 1.4 MB (43 KB frames). The 256 MiB JPEG quota holds about 5,900 to 9,800 frames (200 to 340 sessions, 1.6 to 2.7 h of continuous play); the 50,000-row bound holds about 13.9 h of play with vectors only (263 MB).
* SD writes at 1 FPS while recording: about 43 KB JPEG + 5 KB vector + journal pages, i.e. an estimate of 60 to 100 KB/s (about 1.8 to 3 MB per 30 s session); measure with `/api/pop/system` (`io_write_bytes`, `disks`).
* Exact-scan latency (host, warm cache, `PopHistoryBench`): 30 rows 0.23 ms, 300 0.74 ms, 3,000 15 ms, 30,000 155 ms, 100,000 511 ms (about 5 us/row, mostly SQLite BLOB reads). On the XT5 with 101 rows the scan was 1.4 to 2.5 ms (25 ms on a cold first search). Expect a few times the host figures at 50,000 rows (the row bound); no index is justified before that. A resident float matrix or sqlite-vec would be the next step if retention is raised.

## 9. XT5 procedure (done once; repeat for a clean test)

1. Backups before the change: `/storage/sd/configs/argus-config.json.pre-phase2` (sha 01ecbc35...); demo database hash eeb10f29... (schema-2 `pop-grpc-demo/pop.db`, size 1,978,368 bytes) and schema-1 `visual-memory/pop.db` 8edd5c98... recorded in the session notes; autorun 394d7be1... unchanged.
2. Package: `argus-ext-1793969416.zip` sha256 `6d5a861e0a32f0c270305a617a88ed4a6f86bc06b7e90ba90b6186058c518584` (squashfs `fb9a1c38f10213fd72aa3fba2ce638852d11a8a0a6e662fb3afd5fda6f8a944f`). Only `vm_api_server`, `vm_embed_demo`, `dashboard-server` and the dashboard files differ from `argus-ext-1793966121.zip`; Argus, bridge, exporter, libraries and models are byte-identical.
3. Config: the active config gets exactly one added block, `visual_memory.pop.history = {"enabled": true}`. The new file is `/storage/sd/configs/argus-config.json` (sha `3b52aaef...`).
4. Install: stop the extension, kill mosquitto, unzip, installer, copy the config, one reboot.
5. Test: Proof of Play page, "Verify Correct Ad" (about 55 s), then Visual Search, source BrightSign Display. Use `GET /api/pop/history/status`.

## 10. Rollback

* Disable recording without a reboot: restore `argus-config.json.pre-phase2` and restart only `vm_api_server` (history stays searchable but nothing new is recorded).
* Full rollback: reinstall `argus-ext-1793966121.zip` (Phase 1) or `argus-ext-1793953264.zip` (before Phase 1) with the same procedure and one reboot; the extra table is ignored. To remove the data: stop `vm_api_server`, `DROP TABLE display_history_frames`, delete `/storage/sd/pop-grpc-demo/pop/history/`.

## 11. Remaining risks

* **Time.** A frame's capture time is an upper bound of its display moment; the uncertainty (about 250 ms) is the configured producer-latency budget and is still not measured. The play start comes from the player event stamped by the adapter (about +/-0.25 s), so offsets in the play carry that uncertainty; an offset is not a position in the video file (observed up to about 0.5 s apart).
* **End-of-session guard** assumes the end event is processed within about 1.1 s of the true end; the end guard (0.4 s) and the finalize job trim the rest.
* **First write latency** of about 5 s on the SD card (one-off) delays that frame's storage, not capture.
* **Similarity** is image-embedding similarity. Scene-level similarity can exceed 0.80 for different moments of the same advertisement; a match is an observation of captured visual content, not independent proof that the complete advertisement played.
* **Scale.** The exact scan is linear; the row bound (50,000) and retention keep it bounded. A hot SD card or a very long retention would need a resident vector cache.
* **Restart.** Staged (not yet committed) frames are lost on a restart; committed rows survive; un-embedded rows are embedded on the next idle NPU turns.
