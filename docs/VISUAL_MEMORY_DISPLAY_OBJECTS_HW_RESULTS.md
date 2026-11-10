# Display Object Search: hardware validation results (dev XT5, 2026-11-09)

Package installed: `argus-ext-1794247020.zip` (sha256 `d64ee576...e224`), one reboot, no Weston / Resilience / autorun change (autorun sha `394d7be1...` unchanged). Active config now = the original plus ONE block
`objects.auto.display {enabled:true, live_scan:false, frames_per_minute:12, queue_depth:8}` (file `hw_validation_20261109/config/argus-config.display-on.json`, original kept as `player_backup/argus-config.json`).
Plan and procedures: [VISUAL_MEMORY_DISPLAY_OBJECTS_DEPLOYMENT_PLAN.md](VISUAL_MEMORY_DISPLAY_OBJECTS_DEPLOYMENT_PLAN.md). Everything below is measured on the player; host FP32 numbers are NOT used here.

* **Backups (writers stopped, on-device copy, SHA-256 verified on the host, `integrity_check` ok):** `objects.db` (v2, 1,919 objects), `objects.db.pre-v2`, schema-1 `visual-memory/pop.db`, `pop-grpc-demo/pop.db`, config, autorun. 16 older `argus-ext-*.zip` moved to `/storage/sd/rollback/` (the installer prunes the SD root).
* **Migration:** schema 3, `integrity_check` ok, 0 foreign-key violations, `objects.db.pre-v3` byte-identical to the pre-install backup (`14c67331...`), all 1,919 rows (every v2 column), vectors, progress rows preserved, all `source_type=camera`.
* **Regression:** camera object list/search (scope camera, sims 0.96, 31-58 ms), manual objects from 5 Nov intact, full-frame Visual Search, Proof of Play (`playback_corroborated`), Weston preview 5 FPS, Time search all work.
* **Ad C:** video sha256 verified on the player (`2a88221c...`), registered as `ad-c` (30 + 30 references; Ad A/B untouched). First play: corroborated 3/3 checkpoints, mean similarity 0.990, 29.94 s played, 29 history frames (offsets 1.0-29.0 s, +-250 ms), presentation resumed.
* **Manual (display auto indexing OFF, no detector):** bottle and cup boxes on two stored frames: 4 objects stored in 640x360 coordinates with session/ad/offset/provenance, crops served, searchable; bottle-bottle similarity 0.936, cup-cup 0.975, bottle-cup about 0.57.
* **Automatic, real RKNN INT8 YOLOX-S (backfill of the 29 Ad C frames, 11.7 s):** bottle found in 13 of the 15 frames that show it, cup in 13 of 15, no bottle/cup in the 8 unambiguous scene-3 frames, every box centre inside the ground truth (median IoU bottle 0.88, cup 0.68 against the sprite-canvas truth), confidence bottle 0.78-0.87, cup 0.94. **The 2 misses and the 3 "failed" frames are one defect, not detector misses:** history frames at the 3/15/27 s checkpoint instants have no JPEG of their own (they reuse the checkpoint's) and my display source treated them as expired (`image_unavailable`); of the frames it could read, bottle 13/13 and cup 13/13. Fixed on the host (test `AHistoryInstantThatIsAlsoACheckpointIsServedFromTheCheckpointJpeg`, fails without the fix), NOT on the player (needs a second package install + reboot: `argus-ext-1794251841.zip` sha256 `8bed0a93841565f0abba61e293ff08d3cbc25783e0d5923035d5607450118513`, adds also `prctl` naming so the process shows in `/api/pop/system`).
* **Latency (INT8, NPU core 1, moving averages):** YOLOX 32-34 ms per frame, MobileNetV2 13-17 ms per crop, index per frame with objects 126 ms (decode, crop, embed, SQLite, crop JPEG), total per display frame 120-139 ms. Crops: 182 objects, 396 KB total (2.2 KB each, retained). SQLite insertion alone is not exposed separately (included in the index time); objects.db grew 270-356 KB per 5 min run.

## Performance matrix (5 min per mode, Ad C looping so Weston/D5 run in every mode; `hw_validation_20261109/perf/*.json|csv`)

| | baseline | camera only | display only | both |
|---|---|---|---|---|
| Argus FPS median (min) | 19 (18) | 19 (18) | 19 (18) | 19 (18) |
| Weston preview FPS | 5.0 | 5.0 | 5.0 | 4.8 |
| system CPU % of all cores, median (p95) | 12.6 (13.7) | 12.7 (13.9) | 12.7 (14.3) | 12.7 (13.9) |
| Argus CPU % of one core | 38.4 | 38.8 | 38.1 | 38.4 |
| MemAvailable MB (min) | 2851 (2825) | 2825 (2799) | 2792 (2777) | 2818 (2783) |
| NPU core 0 (Argus) % / core 1 median, p95, max | 64 / 0, 0, 2 | 64 / 0, 0, 4 | 64 / 0, 4, 7 | 64 / 0, 3, 4 |
| SD written KB/s median (total MB) | 129 (41.8) | 132 (45.3) | 152 (49.1) | 159 (52.0) |
| frames indexed per min (camera / display) | - | 6.0 / - | - / 10.8 | 6.0 / 10.5 |
| queue depth max, drops | - | 8, 0 | 8, 0 | 8 / 8, 0 |
| errors (db/detector/provider) | - | 0/0/0 | 0/0/0 | 0/0/0 |
| search under load, median / p95 ms (camera; display) | 50/66; 20/20 | 47/61; - | 45/58; n=1 (invalid) | 42/70; 21/24 |
| D5 sessions corroborated 3/3 (mean sim >= 0.989) | 10/10 | 10/10 | 10/10 | 10/10 |
| display history frames per session | 29-30 | 29-31 | 29-30 | 29 |

Clean display search sample afterwards (40 searches, idle): median 24 ms, all 200.

**Limits of this comparison:** the camera scene was dark throughout (mean luminance 9-10, against 135 before the reboot), so Argus ran at 19 FPS in every mode (low-light exposure, not the package; Argus is byte-identical to the previous install) and the camera worker indexed 0 objects: camera numbers show detector cost on a dark scene, not object-rich load. `vm_api_server` CPU/RSS/PSS were NOT captured: after the in-place config restart its process is called `exe` and the whitelist in `/api/pop/system` misses it (fixed in the pending package; system CPU and MemAvailable are the proxy: CPU unchanged within 0.3 points, about 33-60 MB less free memory with indexing on). NPU core 1 is a sampled text (bursts of 33 ms), SD writes are the whole card. The "expired JPEG" stage was not forced (no evidence deleted).

## Other findings
* The red `VM: errors` pill: Stage 1 reports `database_failures: 1` (cumulative, one lost camera-store write since boot); Argus (unchanged binary) does not provide the diagnostics the new pill logic needs, so the pill stays red.
* One transient HTTP 500 on the first full-frame Visual Search after the reboot (two immediate retries succeeded); cause not identified.
* Some manual indexing calls took 1.5-2.3 s right after idle (first embedding after model unload).
* Dashboard demo (screenshots in `hw_validation_20261109/results/screenshots/`): `d4_display_objects_live_cdp.py` 23/24 (the 24th was a wrong expectation of the camera card).

---
# Fix package installed (2026-11-09, later): `argus-ext-1794251841.zip`, sha256 `8bed0a93841565f0abba61e293ff08d3cbc25783e0d5923035d5607450118513`

Installed with one reboot after a second quiesced backup (`hw_validation_20261109/player_backup2`, `integrity_check` ok; `objects.db` v3 with camera 2,107 / display 182 objects, 0 foreign-key violations; `objects.db.pre-v3` and both PoP databases preserved; the previous package is in `/storage/sd/rollback/`). Only `vm_api_server` and `manifest.json` differ from `1794247020`. Config and autorun unchanged (sha `329c58ec...` / `394d7be1...`).

* **Linked frames:** all 29 frames of the original Ad C session resolve to JPEGs; the three checkpoint-linked frames are byte-identical to their D5 evidence (89/90/91) and to `/api/pop/history/N/image`; D5 evidence readable, session still corroborated. Re-indexing the three previously failed frames (retry_failed) added 4 objects (frames 157 and 169; 181 is scene 3) and a second run added 0. 0 duplicate (frame, box) keys among 258 display objects. Host tests: linked frame served from the checkpoint; history pruning keeps the checkpoint JPEG; a linked frame whose checkpoint image is gone is reported unavailable (no broken reference).
* **Fresh Ad C session, real INT8 YOLOX-S, bounded backfill of 29 frames (0 failed):** bottle **15/15** (confidence 0.805-0.87, median IoU 0.88), cup **15/15** (0.943-0.944, IoU 0.68 vs sprite canvas), 0 bottle/cup in the 8 scene-3 frames, 40 objects, every crop retained. No thresholds changed.
* **D5 regression:** correct -> corroborated (mean sim 0.948, 29 history frames); wrong ad -> mismatch (0.602); interrupted at 10 s -> incomplete (9 history frames); Ad C -> corroborated (0.99); presentation resumed each time.
* **Dashboard (live, `d4_display_objects_live_cdp.py`, screenshots `results/s3`):** 28/28, including "no JPEG expired tile in the picker", bottle and cup searches, compare dialog, PoP session link, camera search unchanged, no source mixing.
* **19 FPS explained:** the active config does not set `exposure_auto_priority`, so the camera keeps its default, which lets it drop the frame rate in dim light. Dark scene (luminance 9-10): 19 FPS in all four modes; bright scene (luminance 136): 29 FPS in all four modes. No setting was changed.

## Performance, bright scene (`perf2/`, 5 min per mode, Ad C looping)
| | baseline | camera only | display only | both |
|---|---|---|---|---|
| Argus FPS median (min) | 29 (28) | 29 (28) | 29 (28) | 29 (28) |
| Weston preview FPS median (mean) | 5.0 (4.7) | 4.6 (4.5) | 5.0 (4.7) | 4.4 (4.3) |
| system CPU % of all cores median (p95) | 14.6 (15.9) | 14.9 (16.4) | 15.2 (16.8) | 15.6 (17.2) |
| Argus CPU % of one core | 53.3 | 53.4 | 53.6 | 54.4 |
| `vm_api_server` CPU % of one core median (p95) | 15.6 (19.3) | 16.0 (21.2) | 17.6 (27.0) | 17.6 (26.2) |
| `vm_api_server` RSS MB (peak) | 40.2 (42.1) | 70.4 | 76.1 (76.4) | 74.8 (75.1) |
| NPU core 0 median / core 1 p95, max | 79 / 1, 2 | 79 / 4, 5 | 79 / 4, 6 | 79 / 4, 5 |
| SD written KB/s (total MB) | 127 (44.9) | 143 (49.5) | 140 (53.4) | 188 (54.4) |
| frames/min camera, display; queue max; drops | - | 6.0; 8; 0 | 12.1; 8; 0 | 5.8, 11.7; 8; 0 |
| worker errors (db/detector/provider) | - | 0/0/0 | 0/0/0 | 0/0/0 |
| latency ms (moving avg at end): YOLOX / MobileNetV2 per crop / index per frame / total per frame | - | 34.5 / 21.0 / 185 / 592 (camera) | 34.7 / 14.8 / 404 / 353 (display) | 34.5, 33.6 / 19.4, 17.7 / 448, 222 / 204, 263 (camera, display) |
| search under load median / p95 ms | camera 39/63, display 31/93 | 37/63, 10/88 | camera 33/140 | 37/64, 33/38 |
| D5 sessions corroborated 3/3 (history frames per session) | 10/10 (28-29) | 10/10 (29) | 10/10 (29-30) | 10/10 (28-29) |

PSS was not captured (not exposed by `/api/pop/system`); RSS and its peak (HWM) are. The index/total latencies are moving averages that vary a lot with NPU contention (Argus uses core 0 at 79 %, D5 embeds on the same NPU); the quiet earlier measurement gave 120-139 ms per display frame. SQLite insertion alone is not separately exposed. Indexing adds about 30-36 MB RSS (the RKNN YOLOX context and the embedder, loaded on first use and unloaded when idle) and 5 MB/min of SD writes at most; no mode changed Argus FPS or D5 results; the preview median dropped to 4.4-4.6 FPS with camera indexing on.
`database_failures` is 0 after the reboot (the counter is in Argus memory and was reset; the earlier value 1 is recorded in this document and in the previous results). The `VM: errors` pill is green again.
