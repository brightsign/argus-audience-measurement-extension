# D4 — Automatic object indexing (host-validated; NOT installed on the XT5)

> Display objects (Weston/VOP2): package, schema-3 migration rehearsal, demo media, hardware validation and performance plan are in [VISUAL_MEMORY_DISPLAY_OBJECTS_DEPLOYMENT_PLAN.md](VISUAL_MEMORY_DISPLAY_OBJECTS_DEPLOYMENT_PLAN.md).

Status: **implemented and tested on the host, cross-compiles for the XT5, never run on the NPU.** Automatic indexing is
**off by default**. Nothing in this document has been installed on a player; see "Hardware procedure" for what is needed.

## Why

D4 Object Search (see `VISUAL_MEMORY_OBJECT_SEARCH.md`) only searched objects that a person had drawn a box around. On the
dev XT5 that was three crops from 5 November, whose parent JPEGs had expired. This adds an optional background worker that
finds objects in stored camera frames with YOLOX-S and indexes them, so a bottle selected in one frame can retrieve similar
objects from other processed frames. It is **not** object recognition and it is **not** a scan of every stored frame.

## Findings that shaped the design

* `objects.db` (schema v1) had no class, confidence, detector or origin; `crop_path` was always NULL (crops were
  regenerated from the frame, so a preview died with the frame JPEG).
* Argus already runs `yolox_s.rknn` (COCO-80) on every camera frame; the post-processing returns all 80 classes at a 0.10 score
  floor and the **person-only filter is applied later** (`inference_worker.cpp:395`, `orchestrator.cpp:778`). Argus' detector
  and its confidence (0.5) are untouched by this work.
* Only frames whose JPEG is retained (48 h, config) can be scanned. On the dev XT5: 34,405 of 65,531 frames. Expired frames cannot
  be reconstructed from embeddings.
* RF-DETR is **not** in this repository (no runner, no model); YOLOX-S is the only detector available.
* Stored frames from the USB camera are **480×360** (the 640×480 camera frame scaled to fit 640×360), not 640×360.

### Host feasibility probe (float YOLOX-S ONNX, an upper bound for the INT8 RKNN model)

`scripts/visual_memory/object_probe_onnx.py` on the real stored frames #84748–#84750 (bottle held / drunk from):

| frame | what the float model reports for the bottle | confidence |
|---|---|---|
| #84748 | `cup`, box around the bottle | 0.27 (found only at threshold ≤ 0.25) |
| #84749 | `bottle` 0.438 **and** `cup` 0.445 on the same box (box correct) | 0.44 |
| #84750 | `cup`, box around the lower half of the bottle | 0.60 |

So the architecture finds the **object** in all three frames, but the **class is unreliable** (bottle/cup confusion on a clear
bottle) and the confidence is low, so a 0.5 threshold would miss two of the three. Defaults therefore use a 0.30 worker
threshold with `bottle`, `cup` and `wine glass` all allowlisted (duplicates of one box across classes are collapsed to the most
confident). A 0.25 threshold finds all three at the cost of more false positives (a stationary object at the desk edge is
detected in every frame as a cup). **The INT8 RKNN model on the NPU may be worse; that is measured by the hardware probe, not
assumed.** Frames contain a person and are kept out of Git.

## Architecture

```
vm_api_server (player process, NOT the Argus capture process)
  ObjectIndexer worker thread ── bounded queue (live: newest unprocessed frames, rate limited; backfill: explicit, capped)
     reader (own read-only connection to visual_memory.db) -> JPEG decode
     YoloxDetector  (own RKNN context, NPU core 1 by default; Argus' YOLOX untouched)
     allowlist / min box / per-frame cap / cross-class duplicate suppression
     MobileNetV2 provider (the existing RKNN embedding provider, shared, internally serialised)
     ObjectIndex::index_frame(): ONE short SQLite transaction per frame
         rows + vectors + frame-progress + live cursor   (crop JPEG files are written before the transaction)
  dashboard / API requests use their own ObjectIndex connection and are never queued behind detection
```

* Nothing runs on the Argus capture thread; no change to Argus' detector, thresholds or pipeline.
* One commit per processed frame (a first version committed three times per frame; SD commit latency is known to have a ~1 s tail,
  `libs/visual-memory/BACKLOG.md` VM-8, and a host test showed a 2 s reader stall). Readers still wait for that one commit; WAL was
  deliberately **not** enabled (SD filesystem not verified for WAL) — see "Known limits".
* Safe shutdown: the in-flight frame finishes, the worker joins, nothing is left half-written. Restart resumes from the persisted
  progress/cursor; a processed frame is never detected twice for the same detector version.

## Schema v2 of `objects.db` (additive; `PRAGMA user_version` 1 → 2)

* `object_regions` gains: `index_origin` (`manual` | `auto`, default `manual`), `class_id`, `class_name`, `confidence`, `detector`,
  `detector_version` (model fingerprint), `crop_bytes`, `crop_width`, `crop_height`, `crop_removed_at`; the existing `crop_path`
  is now used. The existing `source_id` column is unchanged (camera source).
* New `object_frame_progress(frame_id, detector, detector_version, …, status, objects_found, objects_indexed, threshold, error,
  processed_at)` (PK frame+detector+version) and `object_meta(key,value)` (live cursor).
* Migration: before the first change a verbatim copy `objects.db.pre-v2` is written next to the file; every v1 row is kept and
  reads back as `manual` with `crop_state: "none"`; a database from a newer library is refused. **A package rollback to a
  pre-v2 build refuses the migrated file** — restore `objects.db.pre-v2` (any objects added since are lost).
* Automatic detections never relabel or overwrite an existing (manual) region of the same frame/box.
* Retention of regions is by *indexed* time and `max_regions`; over the count limit **automatic regions are evicted before manual
  ones**.

## Crop images

Each indexed object (manual and automatic) gets its own small JPEG (`object_crops/…`, longest side ≤ 256 px, quality 80) so it
stays inspectable after the full-frame JPEG expires. Own policy: `crops.retention_days` (30) and `crops.max_mb` (256); the oldest
crops go first; the row, embedding, box, source-frame id and timestamp are kept and the crop becomes `expired`. States reported
to the UI: `retained`, `expired`, `none` (indexed before v2). `GET /api/vm/objects/{id}/image` serves the object's own crop,
else regenerates it from the frame while that is retained, else 410 with an accurate reason. Deleting or pruning regions removes
their crop files. Nothing is deleted by the migration.

## Configuration (`visual_memory.objects`, all new keys optional)

```json
"objects": {
  "enabled": true, "max_regions": 10000, "retention_days": 30, "min_roi_px": 16,
  "crops": { "enabled": true, "max_side": 256, "quality": 80, "retention_days": 30, "max_mb": 256 },
  "auto": {
    "enabled": false,                      // OFF by default
    "live_scan": true,                     // false = backfill only
    "classes": ["bottle","cup","wine glass","laptop","cell phone","book"],   // names or COCO ids; "person" (0) is always dropped
    "min_confidence": 0.30, "max_objects_per_frame": 8, "min_box_px": 24,
    "frames_per_minute": 12, "queue_depth": 8, "max_backfill_frames": 100, "backfill_pause_ms": 150,
    "detector_model": "model/yolox_s.rknn", "detector_npu_core": 1
  }
}
```
Class ids were verified against the packaged `coco_80_labels_list.txt` (bottle 39, wine_glass 40, cup 41, laptop 63, cell_phone 67,
book 73; a unit test fails if that file and the compiled table ever disagree). With `max_regions` 10,000 a live scan will evict
older automatic objects within a day or two; raise it (e.g. 50,000 ≈ 250 MB of vectors) if more history is wanted. Changing the
config file restarts `vm_api_server` in place (existing behaviour).

## API

| route | |
|---|---|
| `GET /api/vm/objects/auto/status` | worker state/errors/timings, progress, classes, per-class counts, backfill state, frames stored / with a retained JPEG, `scope` |
| `POST /api/vm/objects/auto/backfill` | `{"limit":≤max, "frame_ids":[…], "start","end", "retry_failed"}` → 202; one at a time (409); bounded; refused (503) when auto is off |
| `POST /api/vm/objects/auto/backfill/cancel` | |
| `GET /api/vm/objects?origin=manual\|auto&class=bottle` ; `POST /api/vm/objects/search` accepts `origin`, `class` | |
| region JSON | adds `index_origin`, `crop_state`, `crop_bytes`, `crop_width/height`; auto regions add `class_id/name`, `confidence`, `detector`, `detector_version`; hits add `crop_available` |
| search response | adds `scope` (“Searches the N indexed objects (M manual, K found automatically in P processed frames). Stored frames that were not processed are not searched.”) |

`require_image` keeps a hit whose own crop **or** source frame is still retained. The Go dashboard proxy allows POST on the two
backfill routes only (same-origin, size-capped like the others).

## Dashboard (Visual Search → Object search)

An "Automatic object indexing" panel (worker state, detector, thresholds, frames processed of stored and how many still have a JPEG,
frames with objects/failed, per-frame timings, queue, errors, allowlisted classes and a bounded "Index the 50 most recent retained
frames" button with progress/cancel; class chips with counts), totals split manual/automatic with crop counts, Indexed-by and
Class filters, result cards with origin/crop/frame badges, class + detector confidence, timestamp, source-frame id and box, the
scope sentence, and a comparison dialog with detector provenance and the source frame with its bounding box.

## The red `VM: errors` indicator (cause and diagnostics)

Cause on the dev XT5: `stage1.database_failures = 1`, a **cumulative counter since the Argus process started**; the dashboard
treated any non-zero value as a hard failure, so it stayed red until the next restart. `accepted − encoded = 1` confirms exactly
one frame lost (`visual_memory.cpp`: `db.insert()` returned an error and the JPEG was removed). It is **not** caused by D4. The
SQLite error was not recorded; a likely contributor is writer/reader contention on `visual_memory.db` (rollback-journal mode, a
5 s busy timeout, long vec searches or SD commit tails) — the same moment as the earlier `chunks iter error` — but that is a
**hypothesis**: the player log is in `/tmp` and was not read.

New diagnostics: `SqliteStore` records, per failure, the SQLite code and extended code, `SQLITE_*` name, message, operation
(`insert.prepare|step|not_open`), UTC time and the lost frame's source sequence (`Stats::last_database_error`, in the Argus status
snapshot as `stage1.last_database_error`). The dashboard now shows **red** only for an *active* failure (no successful commit after
it), and **amber "recovered"** — still listed with code, message, operation and time — when commits resumed. An older Argus build
without details stays red and says so. Nothing is suppressed.

## Tests (host)

* `libvisualmemory`: 147 gtest cases pass, including: v1→v2 migration (rows, vectors byte-identical, verbatim backup, idempotent,
  newer schema refused); COCO id table vs label file; allowlist/threshold/size/cap/duplicate filtering; manual labels never
  overwritten; restart without re-detection; expired frames recorded not reconstructed; crops outliving frames; crop age and quota
  eviction (oldest first), manual and automatic; region pruning removes crop files; detector/provider failure back-off without
  poisoning frames; bounded, single, cancellable backfill; live scan rate/queue bounds and persisted cursor; safe stop; stage-1 DB
  error diagnostics (real `SQLITE_BUSY` from a second connection).
* `tests/api/vm_objects_auto_e2e.py` (real `vm_api_server`, mock detector/embedder, synthetic shapes): migration through the server,
  allowlist, provenance, correct class/box/timestamp for every automatic object, similar bottles ranked first, class/origin filters,
  expiry of all frame JPEGs with crops still served, restart persistence, bounds (400/409/503), API responsiveness during a backfill.
* `tests/browser/d4_auto_ui_cdp.py` (headless Chrome): the panel, backfill from the UI, chips and filters, cards, scope sentence,
  compare dialog, expiry badges, and the VM health pill logic for active vs recovered DB failures.
* Go: proxy allow-list test. Existing D3/D4/D5/display suites re-run (see results below).

## Host results (2026-11-09)

| suite | result |
|---|---|
| `libvisualmemory` gtest | 147/147 (incl. 26 new D4-auto/crop/migration/diagnostics cases) |
| `tests/api/vm_objects_auto_e2e.py` | all checks pass |
| `tests/browser/d4_auto_ui_cdp.py` | 26/26 (3 consecutive runs) |
| existing `tests/api/vm_objects_e2e.py` (manual D4) | 0 failures (fresh store) |
| `display_history_cdp.py` / `display_search_cdp.py` / `pop_demo_cdp.py` | 145/145, 54/54, 43/43 (D5, 1 FPS history, time search, Weston search unchanged) |
| Go dashboard-server | pass (new proxy allow-list test) |
| cross-build `vm_api_server` + `vm_object_probe` (aarch64, detector enabled) | links; needs `librknnrt`, `librga`, sqlite, turbojpeg, all already shipped |

Host micro-benchmark (`ObjectBench`, tmpfs, mock embedder; **not representative of the SD card or the NPU**): `index_frame` (decode +
crop + embed + one commit) p50 4.0 ms / p95 6.2 ms; exact cosine search over 1280-d vectors: 1.5 ms @ 1k, 18 ms @ 10k, 92 ms @ 50k
objects. On-device YOLOX latency, MobileNetV2 latency under shared NPU use, CPU/RAM, Argus FPS (≈ 29 expected, not measured) and SD
consumption are **unmeasured** until the hardware probe and a controlled run.

## Hardware procedure (needs your approval; nothing below has been done)

1. **Probe only** (no install, no reboot of the extension, no config or database change). Copy the cross-built
   `vm_object_probe` (`build_xt5/vm_object_probe`) to the SD card with `scp`, then run it from a root shell (which stops the
   presentation, so the player must be rebooted afterwards):
   ```
   EXT=/var/volatile/bsext/ext_npu_argus/RK3588
   LD_LIBRARY_PATH=$EXT/lib:$EXT:/usr/lib /storage/sd/vm_object_probe --root /storage/sd/visual-memory --base-dir $EXT \
     --frames 84748,84749,84750 --thresholds 0.5,0.3,0.25 --core 1 --embed-core 2 --repeat 20 \
     --crop-dir /storage/sd/object_probe_crops --out /storage/sd/object_probe.json
   ```
   It reads frames through the read-only reader, reports for the real INT8 YOLOX on the NPU whether the bottle is found, class,
   confidence, box, per-call latency, crop JPEGs, the cosine matrix between the bottle crops and other objects, MobileNetV2 latency,
   CPU seconds and RSS. Frames #84748–#84750 expire about 48 h after capture (≈ 11 Nov 11:35); pick other frames after that. Delete
   the crop directory afterwards (it may show people).
2. **Only if the probe shows the bottle is found reliably:** package + install (`./package --ext-only --soc RK3588`, the install
   procedure in the player-facts notes, one reboot), still with `auto.enabled=false`; `objects.db` migrates on first open (backup
   written). Then, with a second approved config change, `auto.enabled=true`, `live_scan=false`, and a 20–50 frame backfill that
   includes bottle frames and unrelated scenes; search with a bottle selected in another frame.
3. **Measurements** while the worker runs: Argus FPS (`/api/vm/status` `capture.fps`, expected ≈ 29), detector/indexing ms
   (`auto/status` `worker`), process CPU and RSS (`/api/pop/system`), NPU load, queue depth and drops, SD use (`store` + object crop
   bytes), objects.db size. Compare with the same period before enabling.

## Known limits

* Automatic indexing finds objects only in **processed** frames that still have a JPEG; the search covers the indexed subset, and the
  UI and API say so. Historical frames are never scanned unless a bounded backfill is requested.
* Detector class labels and confidences can be wrong (bottle/cup confusion shown above); embeddings are what ranks results.
* Reader latency: dashboard reads of `objects.db` can wait for one commit on a slow SD card; WAL is a possible follow-up once the SD
  filesystem is verified.
* Not yet measured on hardware: YOLOX latency on the NPU, NPU core 1 sharing with embeddings, CPU/RAM, FPS impact, SD wear.
* Rolling back to a pre-v2 package needs `objects.db.pre-v2` restored.

---

# Part 2 — BrightSign display objects (Weston/VOP2 frames of Proof of Play) — host-validated, NOT installed

The same worker, detector context, class allowlist, MobileNetV2 provider, crop storage and `objects.db` now serve a second frame source:
the Weston/VOP2 display frames that Proof of Play keeps in `pop.db` (Phase 2 1 FPS history samples and the three verification checkpoints).
No separate AI pipeline and no second model instance. Everything below is **off by default** and untested on the NPU.

## What the inspection found

* **Phase 2 is deployed and recording on the dev XT5** (`pop.history.enabled`): 33 sessions, 29 retained history frames per full session,
  Weston/VOP2 JPEGs of 640×360 (the 16:9 output with black pillar bars for 4:3 content). Recording happens only inside registered sessions.
* **Frames of the two registered advertisements do not contain the supported objects.** I ran the float YOLOX-S ONNX on 22 retained Ad A
  frames (`scripts/visual_memory/object_probe_onnx.py`): no bottle, cup, wine glass, laptop, cell phone or book at ≥ 0.15. It detects the
  *television* in the video (`tv` 0.25–0.72, correct boxes) plus a few low-confidence false positives (boat/bird/donut/cake ≤ 0.2). So Ad A
  cannot demonstrate "bottle" search. A demonstration needs approved content with clearly visible bottles/cups (a product ad or an image
  sequence); nothing was fabricated. `tv` is not in the allowlist and was not added.
* **Geometry:** YOLOX letterboxes the whole 640×360 frame (scale 1.0, vertical padding) and its boxes come back in the stored frame's pixels,
  bars included; a unit test places objects directly beside a pillar bar and checks box, crop and stored coordinates. Small products: the probe
  shows the detector works at this resolution for large objects (a TV), but the reliability for small products is **not measured**; if 640×360 proves too coarse that is
  reported as a limitation, the OS capture pipeline is not changed.
* **Identity:** camera `frames.id`, PoP `display_history_frames.frame_id` and PoP `display_capture_evidence.evidence_id` are three independent
  counters that all start at 1. A stored frame is therefore always the triple `(source_type, kind, id)` and nothing joins on the id alone.

## Design

```
camera : VisualMemoryReader  -> CameraObjectSource  \
                                                     >  ObjectIndexer (one thread, one YOLOX context, one embedding provider)  ->  ObjectIndex (objects.db v3)
display: PopStore            -> DisplayObjectSource /        per-source: bounded queue, frames/min, cursor, progress
```

* `ObjectFrameSource` (`libs/visual-memory/include/visualmemory/object_frame_source.h`): `get / read_jpeg / list_after / list_range / counts`.
  `DisplayObjectSource` serves only frames captured through the OS rendered-frame service (`capture_method=resilience_grpc`); a BrightScript
  screenshot checkpoint is not Weston output and is excluded. It reports session, expected and player-reported advertisement, playback offset,
  timestamp uncertainty and provenance, and checks that the JPEG file really exists.
* Scheduling is fair: live jobs of the sources in turn, each at its own `frames_per_minute` and its own bounded queue; a bounded backfill (≤ 100
  frames, one at a time) runs only when no live job is due, so a backfill cannot starve live work and one source cannot starve the other.
* Display live scan indexes only frames Proof of Play **persisted** (history rows and checkpoints), newest unprocessed first, from the moment it is enabled. The 5 FPS live-preview
  frames are never indexed. D5 verification, its thresholds and the three-checkpoint logic are untouched; the worker only reads `pop.db`.
* Backfill: newest first, only frames whose JPEG is retained and which this detector has not processed; an expired parent is recorded as
  `image_unavailable` and never reconstructed; progress is keyed by source + kind + id + detector version, so a restart never repeats work.

## Schema v3 of `objects.db` (additive in effect; rebuild of two tables)

`object_regions` gains `source_type` (`camera`|`display`), `source_kind` (`''`|`history`|`checkpoint`), `pop_session_id`, `expected_media_id`,
`reported_media_id`, `playback_offset_ms`, `time_uncertainty_ms`, `provenance`; `UNIQUE(source_type, source_kind, frame_id, box, model)` replaces the old key
and `object_frame_progress` gains the source in its primary key. SQLite cannot alter a UNIQUE constraint, so v3 **rebuilds** both tables:

1. a verbatim copy `objects.db.pre-v3` (the v2 file; `objects.db.pre-v2` is the v1 file from the earlier step) is written first;
2. foreign keys are switched off (otherwise `DROP TABLE` would cascade-delete the vectors), the tables are rebuilt in ONE transaction preserving every row,
   `region_id`, the AUTOINCREMENT counter, vectors, crop paths and progress; existing rows become `camera`;
3. before COMMIT it verifies the region and vector counts are unchanged and that no *new* foreign-key violation appeared; otherwise it rolls back and the
   file is left exactly as it was (the service then reports the object index unavailable instead of losing data).

Tested on copies (`ObjectSchemaV3.*`): ids, labels, classes, confidences, crop states, vectors (byte-identical), progress and meta preserved; the counter keeps ids of deleted
regions unused; cascade works afterwards; a pre-existing orphan vector is carried over rather than failing the migration; a newer schema is refused.
**Rollback:** stop the extension, restore `objects.db.pre-v3` over `objects.db` (display objects made since are lost), reinstall the previous package. An older package
refuses a v3 file.

## Configuration (all new keys optional, `visual_memory.objects.auto.*`)

```json
"auto": {
  "enabled": true,                       // master switch of the worker (as before)
  "camera":  { "enabled": true },        // camera source (default true, behaviour unchanged)
  "display": { "enabled": false,         // OFF by default: Weston/VOP2 frames of Proof of Play (needs visual_memory.pop.enabled)
               "live_scan": false,       // false = bounded backfill only
               "frames_per_minute": 12, "queue_depth": 8 }
}
```
Classes, thresholds, `max_objects_per_frame`, `detector_npu_core`, crops and retention are shared with the camera.

## API

`GET /api/vm/objects` and `POST /api/vm/objects/search` take `source_type` = `camera` (default) | `display` | `all`; **`all` mixes the sources only when asked for explicitly**.
A search from a region defaults to the region's own source; a box selected on a stored frame is sent as `frame_source_type` + `kind` + `frame_id`.
`POST /api/vm/objects` (manual) takes `source_type` + `kind` for display frames. `GET /api/vm/objects/auto/status?source=display`, `POST /api/vm/objects/auto/backfill {"source":"display","limit":50}`
(503 when display indexing is off). `GET /api/vm/display-frames[?session_id=]` lists stored Weston frames, `GET /api/vm/display-frames/<kind>/<id>/image|crop` serves them (same-origin,
authenticated by the existing proxy policy). Display regions carry `pop_session_id`, `session_link` (`#pop?session=…`), `expected_media_id`, `reported_media_id`, `playback_offset_ms`,
`time_uncertainty_ms`, `provenance` and `frame_image_url`.

## Dashboard

Visual Search → Source: **Audience Camera | BrightSign Display (Weston/VOP2)**; the mode switch (Full frame / Object search) now applies to both. With BrightSign Display + Object search:
the display panel (state, frames processed of stored, with objects, classes, per-source rate/queue, errors, bounded backfill), totals of display objects, class chips and filters, cards with crop,
class/confidence, expected vs player-reported advertisement, offset into the play (± uncertainty) and session, **"Select object in a stored display frame…"** (session list → stored frames →
box on the frame → find similar / save), results with the scope sentence ("…indexed display objects only, not every frame ever displayed"), a comparison dialog with the source frame and its bounding box and an
**Open Proof-of-Play session** link. Switching source resets the query; camera and display results are never listed together.

## Host results (2026-11-09)

| suite | result |
|---|---|
| `libvisualmemory` gtest | 156/156 (new: frame sources, schema v3 migration on a v2 copy, id-collision isolation, provenance, expired parents, fair scheduling, per-source limits) |
| `tests/api/vm_objects_display_e2e.py` (real server + real PopStore data) | all pass: tests A (camera regression) to F (expired evidence) |
| `tests/browser/d4_display_objects_ui_cdp.py` | 29/29 |
| camera: `vm_objects_auto_e2e.py`, `d4_auto_ui_cdp.py`, `vm_objects_e2e.py` | all pass (26/26 UI) |
| D5 / Phase 2 / Phase 1 / time search | `pop_demo_cdp` 43/43, `display_history_cdp` 145/145, `display_search_cdp` 54/54 |
| cross-build (aarch64, detector on) | `vm_api_server`, `vm_object_probe`, `vm_pop_fixture` link |

## Deployment and rollback plan (needs your approval; nothing below has been done)

1. **Package** (`./package --ext-only --soc RK3588`, after syncing `install/RK3588/dashboard` and rebuilding `dashboard-server`): contains the v3 `vm_api_server`, the new dashboard. Config is **not** changed:
   camera automatic indexing stays as it is (enabled today), display indexing is off.
2. **Install** with the usual procedure and one reboot. On the first object-index open `objects.db` migrates v2 → v3 (≈ 1,000 rows: milliseconds); verify `objects.db.pre-v3` exists and the dashboard's camera object
   counts are unchanged (`/api/vm/objects/auto/status`). Rollback = previous zip + restore `objects.db.pre-v3`.
3. **Hardware probe on display frames** (read-only, no config change): fetch retained display frames from the dashboard API (`/api/vm/display-frames/history/<id>/image`), copy them and `vm_object_probe`
   to the SD card and run `vm_object_probe --jpeg /storage/sd/probe/h19.jpg,... --thresholds 0.5,0.3,0.25 --core 1 --embed-core 2 --repeat 20` from a root shell (reboot afterwards): detection latency,
   boxes, MobileNetV2 latency, CPU/RSS on real Weston frames.
4. **Content:** register an approved advertisement containing clearly visible bottles/cups (or an image sequence) in Proof of Play and run a session so Phase 2 stores its frames.
5. **Enable** `objects.auto.display.enabled=true`, `live_scan=false` (config edit; `vm_api_server` restarts in place), backfill 20–50 display frames from the dashboard, search from another frame of the ad.
   Only then consider `display.live_scan=true` (`frames_per_minute` 6).
6. **Measure** before/after: Argus camera FPS (≈ 29), Weston live-preview FPS (`/api/display/status` `fps`, ≈ 5), detector/indexing ms per display frame (`auto/status` `worker`), objects per ad, queue and drops
   per source, process CPU/RSS (`/api/pop/system`), NPU load, `objects.db` and crop bytes (SD), search latency; confirm D5 verification still passes (Verify Correct Ad).

## Known limits (display)

* Only indexed objects are searched; display frames exist only for registered sessions; the registered ads (Ad A "Meet BrightSign") contain no allowlisted object, so a demonstration needs new approved content.
* Display frames are the OS-composed 640×360 output (approximate, letterboxed): small products may be too small for the detector; unmeasured on the NPU.
* A history frame that doubles as a checkpoint is indexed once (as history); the checkpoint without history row is indexed as `checkpoint`.
* `image_exists` for display frames in list endpoints reflects the retention flag; single-frame lookups (search hits, detail) also check the file.
