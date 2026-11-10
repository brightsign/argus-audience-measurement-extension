# Display Object Search: package, migration, demo media and hardware validation plan

Status (2026-11-09): package built and verified on the host, **not installed**. Nothing on the XT5 was changed: no install, no reboot, no autorun or configuration
change, no migration of the active `objects.db`, no push, no commit. This document is the plan that needs an explicit approval before any player step.
Design, schema and API are in [Part 2 of VISUAL_MEMORY_OBJECT_AUTO_INDEXING.md](VISUAL_MEMORY_OBJECT_AUTO_INDEXING.md).

## 1. Package

| | |
|---|---|
| File | `argus-ext-1794247020.zip` (72,981,234 bytes) in the repository root |
| SHA-256 (zip) | `d64ee5765f184ddb7a14c013d07267a6b25828aefc7a22af5cf1141c68b1e224` |
| SHA-256 (`ext_npu_argus.squashfs` inside) | `acc3a0bbce035f88b4533b0a4bb2d9fa3e708a5221a5dc0a4b0a9f9a942a9f1a` |
| Helper | `install-on-player.sh` (SHA-256 `5123c9d6f33bbfd2f281eac70813c4619dcae114086c37423aeaece0b9d10ed3`), generated for this zip |
| Build recipe | cross-built `build_xt5/{vm_api_server,dashboard-server}`, `./package --ext-only --soc RK3588` with `ARGUS_DISPLAY_BRIDGE` = the bridge of the installed package |
| Commit | HEAD `22c22e3851eb9e51a372878d8c19fe875c7f1072` on `feature/d4-d5-object-search-pop` (equal to `origin/...`, **nothing newer is committed**) |
| Uncommitted work in the package | tracked diff against HEAD: 30 files, +2516 / -178, SHA-256 of `git diff HEAD --binary` `9701b55b935d75670d1c969da9639b8d0112e33a4761961ed3450c1221908688`; 27 new source/test/doc/script files (list `git status`), SHA-256 over their contents `cd8c2da4f0bfb6be92187f6c9d2b65d216e72d6682ff776d8422b308dcd065ff` |

Verified by unpacking the squashfs (`unsquashfs`) and comparing: `RK3588/vm_api_server` and `RK3588/dashboard-server` are byte-identical to the cross-built `build_xt5` binaries
(aarch64; `vm_api_server` contains `avg_embed_ms`, `dashboard-server` contains the `/api/vm/objects/auto/backfill` route), all 11 dashboard files (`objects.js` with `setSource`,
`vm.js`, `vm.css`, ...) are byte-identical to `dashboard/`. **Unchanged, byte-identical to the installed package `argus-ext-1794231179.zip`:** `attention_demo` (Argus), `argus_display_bridge`,
`argus-exporter`, `image-stream-server`, `vm_embed_demo`, and also `dashboard-server` (the installed package already contains the proxy with the backfill routes), `vm.css`, all models, libraries and
configs (`diff -rq` of the two unpacked squashfs trees). **Exactly four files differ from the installed package: `RK3588/vm_api_server`, `RK3588/dashboard/objects.js`, `RK3588/dashboard/vm.js` and `manifest.json`.**

Rollback packages (all preserved on the host in `.../scratchpad/rollback/`): `argus-ext-1794231179.zip` (**currently installed**: camera auto indexing, schema v2, sha256
`71323e9349ea657c09d4571393b3d7b5a92002265de592611f882eafdfb1a9fb`), `1794063462` (time search), `1794063169`, `1793976273`, `1793969416` (Phase 2), `1793966121`, `1793965688`,
`1793953264`, `1793779707`, and `argus-ext-1794245703.zip.superseded-no-embed-timing` (the first build of this task, identical except for the embedding-time counter).
**The installer deletes every other `/storage/sd/argus-ext-*.zip`** (see its last loop), so move the current rollback zip aside on the player first (step 2 below).

## 2. Protecting the existing object data

The live `objects.db` is schema 2 (1,665 objects: 1,662 automatic + 3 manual, 1,665 vectors, 1,382 progress rows, journal mode DELETE, no `-wal`/`-shm`/`-journal` files present).

**Migration performed by the new `vm_api_server` at first start** (`ObjectIndex::open`): checkpoints any WAL, copies the file verbatim to `objects.db.pre-v3`, then in ONE transaction rebuilds
`object_regions` and `object_frame_progress` with the source-aware keys (region ids, AUTOINCREMENT counter, vectors and progress are kept), verifies the row counts and that there are no
*new* foreign-key violations, and only then commits. Any failure rolls back and leaves the file as it was. Nothing else (`visual_memory.db`, `pop.db`) is touched.

**Rehearsed on a COPY of the live file** (read-only `scp` of `/storage/sd/visual-memory/objects.db`, SHA-256 verified against the copy; `scripts/visual_memory/validate_objects_migration.py`, host build of the new
`vm_api_server`; result file `.../scratchpad/mig_v3/run1.txt`): **28 of 28 checks pass**.

* schema 2 -> 3, `integrity_check` ok, no new foreign-key violations, migration + start 0.1 s;
* `objects.db.pre-v3` is **byte-identical** to the v2 input;
* row counts equal for `object_regions` (1,665), `object_vectors`, `object_frame_progress`, `object_models`, `object_meta`;
* every one of the 26 v2 columns of every region row is identical (ids, frame ids, boxes, class, confidence, label, origin, crop paths and sizes, detector, timestamps); the vector blobs hash identically;
  progress rows identical; AUTOINCREMENT counter not lowered; origins preserved (auto 1,662 / manual 3);
* the 8 new columns exist; every migrated row is `source_type='camera'`, `source_kind=''` and keeps its camera frame id; progress rows are camera;
* a second start changes nothing (backup untouched, still v3, integrity ok);
* **rollback = restoring the backup**: copying `objects.db.pre-v3` over `objects.db` gives a file byte-identical to the v2 input (schema 2, integrity ok, counts unchanged), and the migration can be re-run on it.

Source-specific identifiers: camera frame ids, Proof-of-Play history ids and checkpoint ids are three counters that all start at 1. The identity is always (source, kind, id); the unique keys and the
progress primary key now include the source. `vm_objects_display_e2e.py` (56 checks) exercises the collision case with real overlapping ids; library test `ObjectSchemaV3` migrates a v2 fixture.

**Rollback against the previous installed package.** The previously installed `vm_api_server` (sha256 `4ccd8ed7…db4`, extracted from `argus-ext-1794231179.zip`) is aarch64 and cannot run on the host
(no qemu), so its behaviour was checked from its own strings and source: it opens only schema <= 2 and refuses a newer file with `objects.db schema N is newer than this library (2)` (object search then
reports unavailable; nothing is written or damaged). Therefore the rollback is **package + file**: reinstall `argus-ext-1794231179.zip`, restore `objects.db.pre-v3` over `objects.db` (objects indexed
after the migration, including all display objects, are lost; the crop files stay on disk and are pruned by the old retention). This is a hardware step and is part of the validation procedure (section 6, step 14).

Backup procedure to run BEFORE the install (root shell is not needed; `scp` only, SSH password read from the file, never typed or logged):
`scp brightsign@PLAYER:/storage/sd/visual-memory/objects.db* ./objects_backup_YYYYMMDD/` then `sha256sum`. `objects.db` is the only file (journal mode is DELETE, so there is no WAL/SHM; a `-journal`
file exists only during a commit, copy it too if it is present). The migration's own `objects.db.pre-v3` is a second copy on the card.

## 3. Demonstration media (outside Git)

Folder `/home/sree/bs/sqlite_argus/demo_media/product_test/` (not a Git repository). Generated by `scripts/visual_memory/make_product_test_video.py` (Pillow + numpy + ffmpeg, procedural drawing: **no photographs, no
third-party art, no people**). `meet-brightsign.mp4` and `snowy_river.mp4` were not touched (mtimes 2025-08-17 / 2023-02-27).

| | |
|---|---|
| Video | `product_test_30s.mp4`, 1920x1080, H.264 High, yuv420p, 25 fps, 750 frames, 30.000 s, no audio, 1,147,736 bytes |
| SHA-256 | `2a88221c3d37805a08de1419ed88d138526185534e8888b64e131025104ecec1` (regenerating gives the same hash: reproducible) |
| Timeline | 0-10 s scene 1: water bottle (left) + coffee cup (right); 10-20 s scene 2: same two objects at other positions and sizes (cup left, bottle right, different wall); 20-30 s scene 3: unrelated objects (vase, orange, wooden blocks) |
| Reference frames | `frames1fps/` 30 JPEGs, `<offset_ms>.jpg`, made from the original video with the verified display transform (`scale=1920:1080,crop=1280:800:0:0,scale=576:360,pad=640:360:32:0`); `reference_frames.sha256` |
| Provisioning bundle | `bundle/` from `scripts/pop/provision_demo.py prepare` for media `ad-c` "Product Test (bottle and cup)": 30 screenshot-profile + 30 display-profile references, `register.json`, manifest |
| Ground truth | `product_test_30s.manifest.json` (object centres, sizes, drift per scene) |

Geometry: the player puts a video in a 1920x1080 rectangle and its 1280x800 output shows the **top-left** 1280x800 of it; the OS frame is that picture scaled into 576x360 between black bars. All
objects are therefore inside x < 1280, y < 800 with margin, and the caption is drawn outside that window (visible on a PC, never on the player). The bottle is about 100 px and the cup about 150 px wide in the 640x360 frame.

**Detections of the existing YOLOX-S (host, float ONNX, the model the packaged INT8 `yolox_s.rknn` is converted from), on the 30 display-geometry frames, worker allowlist, ground truth from the manifest**
(`yolox_float_display_frames.json`, `host_eval_float_yolox_0.3.json`):

| | frames with the object (unambiguous) | found | confidence min / median | box centre inside truth | median IoU |
|---|---|---|---|---|---|
| bottle | 16 | **16** | 0.742 / 0.830 | 16 / 16 | 0.885 |
| cup | 16 | **16** | 0.939 / 0.946 | 16 / 16 | 0.677 (truth box = sprite incl. saucer and handle) |
| scene 3 (8 frames) | - | no bottle, no cup (only `vase` 0.94 and `apple` 0.43-0.58, neither in the allowlist) | | | |

6 frames within 1.5 s of a scene change are excluded as ambiguous (the player offset has a +-250 ms uncertainty). Scene 1/2 frames also contain `vase` detections (0.2-0.7, the steam/bottle) which are not in the allowlist and are never indexed.

**What this does and does not show.** It shows that the content is suitable for YOLOX-S **in float**. It is NOT the INT8 NPU result, which is unknown until the probe or the worker runs on the player, and
the objects are drawings, not photographs. Finding while preparing it: a large upright, symmetric, translucent bottle is read as `vase` (0.87-0.92, `bottle` 0.05-0.24); the bottle is found reliably
(0.57-0.61) when it is 90-110 px tall in the 640x360 frame and loses to `vase` above about 130 px, so the video uses small bottles on purpose (tilted/lying bottles were detected as `kite`). This is content
design, not threshold tuning; no similarity or detector threshold was changed. The same size sensitivity will apply to real products on a display frame: small products need to be shown reasonably large and upright.

## 4. Source-aware indexing tests prepared for the hardware

`tests/api/vm_objects_display_hw.py` (stdlib only, talks to the dashboard API, never changes configuration; writes only through the documented manual-index and backfill calls) with stages
`baseline`, `compare`, `manual`, `auto`, `search`, `isolation`, `expired`, `evaluate`. Coverage of your list:

| Requirement | Stage | Notes |
|---|---|---|
| Camera Object Search regression | `baseline` before, `compare` after the migration | every camera object id, frame id, box, class, label, origin, confidence, detector and timestamp unchanged; top-10 of 5 searches identical (new objects excluded); default scope camera |
| Manual selection from stored Weston frames | `manual` | bottle and cup boxes (ground truth) in scene 1 and scene 2 frames; stored in 640x360 coordinates with session/ad/offset/uncertainty/provenance; crop served; searchable; scene-1 bottle finds scene-2 bottle. **Independent of YOLOX**: run with `objects.auto.display.enabled=false` (the stage prints the setting). Host proof: `vm_objects_display_e2e.py` indexes a display crop with display auto off and with no detector run. |
| Automatic indexing of the product video | `auto` | bounded backfill of <= 50 frames, then every object against the ground truth: found rate per class, box centre inside the truth, no bottle/cup in scene 3, allowlist, crops retained, detector provenance |
| Crop persistence and bounding boxes | `auto`, `manual` | boxes inside the 640x360 frame (bars handled), crop JPEG served |
| Display-only search | `search` | display scope, "indexed objects only" text, similar class first, timestamps/ad/offset/session link |
| Correct ad, timestamp, PoP session link | `search` | the session opens in Proof of Play with the same expected/reported ad; time search at the object's time returns the same frame; frame image served |
| Source isolation | `isolation` | default camera, display, explicit `all`; camera query with display object, same ids in both sources (reported as not exercised if the data has no overlap; the e2e covers it) |
| Missing / expired JPEG | `expired` | reports **NOT EXERCISED** unless the player has an expired frame; forcing it means deleting one test JPEG (needs approval) |

Pass criteria are reporting criteria (>= 80 % of the unambiguous frames per class, centre inside truth, 0 false positives in scene 3), not tuning targets. A miss is a result.

Host validation of the scripts: `vm_objects_display_hw_selftest.py` runs all stages against a host server (mock detector, grey letterboxed frames with painted blocks, fake session `sp`/`ad-c`, camera frames with overlapping ids): **all stages pass**.
The ground-truth evaluator passes on the real float-YOLOX results and fails (4 checks) on a deliberately sabotaged result (missing bottles, wrong cup boxes, a bottle in scene 3). The mock cannot separate two objects in one frame, so the
two-objects-per-frame rates are only proven by the evaluator test and on the player.

## 5. Performance comparison

`scripts/visual_memory/measure_object_indexing.py` (read-only sampler, 2 s interval) per run, label per configuration. Metrics: Argus FPS, Weston preview FPS, system CPU (all cores), Argus and `vm_api_server` CPU (one core = 100 %),
RSS/HWM, free memory, NPU load per core, SD sectors written, objects.db and crop growth, per-source frames/min, objects, queue depth, drops, and the worker latencies: **YOLOX** (`avg_detect_ms`), **MobileNetV2 per crop**
(`avg_embed_ms`, new in this package), **index per frame** (`avg_index_ms`: decode, crop, embed, SQLite), **total per frame** (`avg_frame_total_ms`), each also per source.

| Run | Configuration (`objects.auto`) | Load |
|---|---|---|
| 0 baseline | `enabled:false` | quiet, camera pipeline only (and the same with the product video playing, for the Weston FPS reference) |
| 1 camera only | `enabled:true, camera.enabled:true, live_scan:true`, `display.enabled:false` | live camera |
| 2 display only | `camera.enabled:false`, `display.enabled:true, live_scan:true` | product video playing (history 1 FPS) |
| 3 both | both on | product video playing |

Each run 5 minutes after a 1 minute warm-up; the live rate limits stay at the configured per-source values (camera as it is, display `frames_per_minute` 12, `queue_depth` 8); the bounded backfill is measured separately
(one 50-frame backfill during run 3, to show the live sources are not starved). Config edits restart `vm_api_server` in place (about 2 s), no reboot.
Limits: SD writes are the whole card's counter, not SQLite alone; per-process I/O counters are unavailable (-1) on this firmware; NPU load is a sampled text; Weston FPS exists only while a viewer or a Proof-of-Play session holds the preview.
Dry run of the sampler against the live player (read-only, 14 s, installed package): Argus 29 FPS, system CPU 10 %, `vm_api_server` RSS 74 MB, NPU core 0 77 % (Argus), core 1 0-3 %.
Not changed: models, thresholds, Proof-of-Play verification logic. The existing camera-indexing pipeline is not optimised.

## 6. Exact player changes and the hardware validation procedure (needs your approval)

**Reboots/autorun.** One reboot, the one the installer performs. No autorun change: the existing job-mode `autorun.brs` (gate `SD:/pop/enabled`, `allow_test_jobs: true`) plays any file on the SD root, so the product video is a third
registered advertisement (`ad-c`) scheduled from the existing dashboard controls. Configuration edits restart the API server in place and need no reboot. The new keys are additive and default OFF; the current configuration keeps working unchanged.

Player changes, in order:

1. **Backup (read-only, no change).** `scp` `objects.db*` and the active `argus-config.json` (`/storage/sd/configs/argus-config.json`) to the host; record SHA-256; `vm_objects_display_hw.py baseline --out baseline.json`; keep the current dashboard counts.
2. **Protect the rollback zip.** On the player (root shell), move it out of the installer's prune pattern: `mkdir -p /storage/sd/rollback && mv /storage/sd/argus-ext-1794231179.zip /storage/sd/rollback/` (a host copy exists as well).
3. **Install.** `scp argus-ext-1794247020.zip install-on-player.sh` to `/storage/sd/`; at the root shell `sh /storage/sd/install-on-player.sh` (stops the extension, installs the LVM volume, prunes other `argus-ext-*.zip`, **reboots**). On any failure it stays at the shell without rebooting.
4. **After the reboot (a few minutes: the player must not be left at a root shell).** Check `/api/vm/status`: Argus ~29 FPS, `objects.available`, `/api/pop/status` unchanged. The first start migrates `objects.db` to schema 3 and creates `objects.db.pre-v3`: `vm_objects_display_hw.py compare --baseline baseline.json` (camera regression), compare the camera object count (1,665 + whatever live scan added).
5. **Verify the display source is OFF and the demo is unchanged:** D5 "Verify Correct Ad" (Play A) still corroborates; time search and Display Preview still work.
6. **Provision `ad-c` (no autorun change).** `scp` `product_test_30s.mp4` to `/storage/sd/`; `scp -r bundle/refs_incoming/*` to `/storage/sd/pop-grpc-demo/pop/refs_incoming/`; `scripts/pop/provision_demo.py apply --api http://PLAYER:20300 --bundle bundle` (registers the media row and builds the references on the device; there is no API to delete a media row, so `ad-c` stays in the isolated demo store until removed by SQL).
7. **Play it.** Dashboard Proof of Play -> schedule `ad-c` (expected `ad-c`), play; the slot is 30 s, the video 30.000 s (if the end races the slot, schedule 33 s). Note the session id.
8. **Manual stage with display auto indexing off:** `vm_objects_display_hw.py manual --session SID --manifest ...`.
9. **Enable display indexing** (single block in `/storage/sd/configs/argus-config.json`, edited by you; the file is backed up first; `objects.auto.display` is a new sub-object next to your existing keys):
   `"display": { "enabled": true, "live_scan": false, "frames_per_minute": 12, "queue_depth": 8 }`. The server restarts itself.
10. **Auto stage:** `auto --session SID`, then `search --session SID`, `isolation`, `expired`.
11. **Dashboard checks (UI):** Visual Search -> source "BrightSign Display" -> Object search: totals, classes, crop gallery, original frame with box, similarity, timestamp/uncertainty, ad name/offset, "Open Proof-of-Play session", the "indexed objects only" sentence; switching to "Audience Camera" shows only camera objects.
12. **Performance runs 0-3** (section 5).
13. **Optional, with approval: INT8 probe** `build_xt5/vm_object_probe` plus a few `frames1fps/*.jpg` copied to the SD card and run from a root shell (stops the presentation; reboot afterwards) to read the NPU YOLOX result before any indexing. Running the worker (steps 9-10) already gives the same answer without a root shell, so the probe is only needed for per-frame diagnostics.
14. **Rollback test (separate, planned):** set `objects.auto.display.enabled` false (instant). Full rollback: stop, reinstall `/storage/sd/rollback/argus-ext-1794231179.zip` with the same helper (adapt the file name; one reboot), then `cp objects.db.pre-v3 objects.db` while the extension is stopped; verify the 3 manual + 1,662 automatic camera objects and search again.

Safety: display indexing is OFF by default and bounded (per-source queue and rate, one 50-frame backfill at a time, backfill only when no live job is due); `pop.db`, Proof-of-Play verification and the OS capture are not touched.

## 7. Known limits and risks

* INT8 NPU detection of the product video is unmeasured; float YOLOX-S is the upper bound. Bottle vs vase sensitivity (section 3) may differ after quantisation.
* The reference frames and the product video are drawings; real products on a real display may behave differently.
* One SQLite commit can stall a reader for up to ~1 s on the SD card (journal mode DELETE; WAL not enabled because the card filesystem has not been verified for it).
* Registering `ad-c` is a permanent row in the isolated Proof-of-Play demo store until removed by SQL; its history frames expire by the normal Phase 2 retention.
* The rollback needs a reinstall plus restoring `objects.db.pre-v3`; display objects created after the migration are lost by that rollback.
* The migration rehearsal used a copy taken while the live scan was running (verified by SHA-256 and `integrity_check`, no journal present).
* A stray test directory `argus-audience-measurement-extension/--root/` (synthetic frames, created at 17:44 by an earlier mis-invoked test tool) is untracked debris and is not part of the package; delete it when convenient.
