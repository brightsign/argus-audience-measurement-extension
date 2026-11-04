# D4 — Object Search (manual region of interest)

Find visually similar **objects** in historical frames. The user draws a rectangle around an object in a stored frame; the
crop is embedded with the existing MobileNetV2 FP16 model (the same `pp1` preprocessing as full frames), stored in a
separate object index, and searched by cosine similarity. It is **manual ROI indexing**: there is no object detector, the
YOLOX/RetinaFace pipeline is untouched, and nothing here is general object recognition or identity matching. Scores are
cosine similarities, **not** identification probabilities. Off by default (`visual_memory.objects.enabled`).

## How it works
| Piece | Where |
|---|---|
| `ObjectIndex` (library, Argus-independent) | `libs/visual-memory/include/visualmemory/object_index.h`, `src/object_index.cpp` |
| HTTP API (`/api/vm/objects*`, `/api/vm/frames/{id}/crop`) | `src/tools/vm_api_server.cpp`; contract in `docs/VISUAL_MEMORY_DASHBOARD_API.md` |
| Proxy policy (POST only on the two JSON routes, 16 KiB cap, same-origin) | `dashboard-server/main.go` |
| UI: Select Object, crop preview, Save to Index, Object search mode, comparison with bounding-box overlay | `dashboard/objects.js`, hooks in `dashboard/vm.js`, `dashboard/vm.css` |
| Config | `visual_memory.objects.{enabled,max_regions,retention_days,min_roi_px}` (`docs/CONFIGURATION.md`) |

**Storage / migration.** Objects live in their own SQLite file `<storage_root>/objects.db` (`PRAGMA user_version = 1`):
`object_models`, `object_regions` (region_id, frame_id, source_id, capture_utc_ms, bbox_x/y/width/height,
image_width/height, object_label, model_id, embedding_status, crop_path (always NULL), created_at, frame_sha256) and
`object_vectors` (1280 × FP32, L2-normalised BLOB, `ON DELETE CASCADE`). A database written by a newer library is refused.
This is a **deliberate deviation** from "extend visual_memory.db": Argus keeps sole ownership of `visual_memory.db` (no
writer contention, no change to Stage 1/2 data or its schema version, the reader stays read-only), and the API process
owns `objects.db`. `frame_id` is a logical reference checked at index time. Vectors are a plain BLOB with an exact
brute-force cosine scan (bounded by `top_k`, filters, a deadline and client-disconnect cancellation); sqlite-vec is not used
for this table (a 10,000-object scan is 62 ms, see below). Object and full-frame vectors are different tables, and a
search only considers regions of the query's own model/preprocessing (`model_mismatch` otherwise).
No crop JPEG is stored: crops are regenerated from the unchanged original frame (`crop_path` exists for a future option).
Duplicate ROIs (same frame, rectangle, model) return the existing region. Retention is independent of frames: by
`max_regions` and `retention_days`, **counted from when a region was indexed** (a first version used the frame's capture
time and deleted a region the moment an old frame was indexed; found by the API end-to-end test, fixed and covered by a unit test).

**ROI geometry.** The browser maps pointer positions to the stored image's own pixels through `fitRect()` (the frame is drawn
`object-fit: contain`, so letterboxing/pillarboxing and scaling are handled), rounds outward, clamps to the image and
validates (`min_roi_px`, bounds); the server validates again (overflow-safe) and rejects out-of-bounds/too-small/negative
rectangles. Mouse and touch use pointer events; numeric X/Y/W/H fields give a keyboard path. The original JPEG is never modified.

## Validation
All numbers below were measured; the datasets are small and synthetic/controlled, so they show that the machinery works and
give relative behaviour, **not** general object-recognition accuracy.

| Check | Result |
|---|---|
| Library tests (new) | 8 (ROI rules, crop colour/aspect, original untouched, index/duplicate/restart persistence, retrieval across backgrounds, model mismatch, missing JPEG, retention/labels/schema guard, frame DB untouched); library total 81/81 |
| API end-to-end (`tests/api/vm_objects_e2e.py`, host, real server) | 45 checks: empty index, index/duplicate, 8 invalid-ROI rejections, bounds, search modes, filters, crop endpoints, method/path policy |
| Go proxy tests | object POST allow-list, origin, 16 KiB cap (`dashboard-server/main_test.go`) |
| ROI maths (`node tests/js/test_roi.js`) | 12 tests incl. 1.5× scale, pillarbox/letterbox offsets, clamping, round trips |
| Real browser (Chrome DevTools protocol, `tests/`-style script) | 23 checks on host (16:9 and 4:3-pillarboxed frames, drag → coordinates within 2 px, touch drag, preview, save, results order, overlay box within 2.5 px of the stored bbox, D3 mode intact) and 5/5 on the XT5 |
| Crop colour/geometry | server crop vs the same crop cut locally from the stored frame: mean abs. error 2.9/255 (max 5.0; JPEG re-encode), no channel swap (`CropColourAndOriginalUntouched` checks a red object stays red) |

**Retrieval (XT5, real RKNN model).** Dataset: `libs/visual-memory/scripts/make_object_scenes.py` — 240 people-free frames,
405 labelled objects: 4 classes (cup, bottle, box, can) × 5 instances, varying background, brightness/colour cast, scale
(0.55–1.15×), rotation (±22°), flips. Even frames are indexed (162 regions), odd frames are queries (162 of seen instances,
37 of 4 instances — can1–can4 — never indexed). Rectangles are the ground-truth box with ±6 % random edge error (a hand-drawn
box). Relevant = same instance (≈ 8 per query). Reproduce with `libs/visual-memory/scripts/eval_object_search.py`
(`docs/test_results/d4_object_search_eval_xt5.json`).

| Metric | Value |
|---|---|
| Instance precision@1 / @5 | 0.932 / 0.798 |
| Instance hit@5 (≥ 1 relevant in top 5) | 0.994 |
| Instance recall@5 (÷ min(5, relevant)) / recall@10 (÷ all relevant) | 0.798 / 0.625 |
| Instance MRR | 0.953 |
| Class (cup/bottle/box/can) precision@1 / @5 | 1.000 / 0.998 |
| Unseen instances: top-1 has the right class | 0.946 |
| Top-1 similarity, seen vs unseen instances (mean) / AUC | 0.913 vs 0.837 / 0.966 |

Scores separate seen from unseen instances well on this set (AUC 0.97) but the ranges overlap, so **no similarity threshold
is shipped**; `min_similarity` is opt-in.

**Latency (XT5, API server on NPU core 1, Argus running).** `docs/test_results/d4_object_search_scale_xt5.json`:

| Indexed regions | Vector scan (p50 / p95) | ROI query total, server (crop + decode + NPU embedding + scan) p50 / p95 | Query by stored region (scan only) p50 / p95 |
|---|---|---|---|
| 100 | 1.6 / 1.7 ms | 37 / 49 ms | 5.2 / 5.4 ms |
| 1,000 | 7.5 / 7.7 ms | 42 / 50 ms | 22.6 / 31.6 ms |
| 10,000 | 62 / 63 ms | 105 / 118 ms | 63 / 87 ms |

Indexing one region (decode + crop + NPU + insert with FULL sync on the SD card): 61 ms p50, 83 ms p95. An unrolled dot
product halved the 10,000-region scan (130 → 62 ms). The 3× gap between the region and ROI scan at 100/1,000 regions was
measured but its cause is not identified (open item).

**Impact on the audience pipeline.** Argus FPS (Prometheus `argus_fps_current`, 1 s samples, camera exposure priority unset
as on the original config): idle 29.03 FPS → 29.05 during the retrieval evaluation (97 s); 29.05 FPS over a 20-minute run
that indexed 10,000 regions and ran ~160 queries (min 28, **0 samples below 25**, drops 0). The API server is a separate
process: resident memory 28.8 MB (peak 29.6 MB) after the run; `objects.db` is 56.8 MB for 10,000 regions (≈ 5.7 KB each);
NPU core 0 (detectors) was unchanged and core 1 idle between requests; SoC 34 °C. The Argus capture process is not modified
by D4. No overhead figure for Argus beyond this FPS comparison is claimed.

**Full-frame regression (D3).** Same server, same dataset: coffee reference → frames 259/304 at 0.960/0.957 (as in the
D1–D3 report); `search/frame` unchanged; D3 tab is the default mode.

## D4 completion demonstration (XT5)
Store `/storage/sd/vm-d4-store` = 240 object frames + the 64 people-free demo scenes. Select the coffee cup in frame #259
(drag, preview, label "coffee cup", Save) → open frame #304 (the flipped/brightened variant) → Select Object around its
cup → Find Similar Objects: #1 is the indexed cup at 0.986 with the source frame and its bounding box highlighted; #2–#5 are
synthetic mugs at 0.51–0.57. Screenshots (people-free, not committed) are in the session scratchpad `d4host/shots_xt5/`.

## Run the demo (development only; nothing is installed)
```
# host: build, then stage binaries + dashboard + dataset
make build SOCS=rk3588 EXTRA_CMAKE_FLAGS="-DENABLE_VISUAL_MEMORY=ON -DENABLE_VISUAL_EMBEDDINGS=ON"
python3 libs/visual-memory/scripts/make_object_scenes.py /tmp/objdata
scp -r <stage>/vm-d4-input brightsign@<player>:/storage/sd/      # vm_api_server vm_ingest_dir dashboard-server dashboard/ scenes/ vm_object_demo.sh
# player (root shell): ingest, then start on :20398
cp /storage/sd/vm-d4-input/vm_ingest_dir /tmp/ && chmod +x /tmp/vm_ingest_dir
cd /storage/sd/vm-d4-input/scenes && EXT=/var/volatile/bsext/ext_npu_argus
LD_LIBRARY_PATH=$EXT/RK3588/lib:$EXT/lib /tmp/vm_ingest_dir --root /storage/sd/vm-d4-store --model $EXT/RK3588/model/visual_memory/mobilenet_embedder.rknn --npu-core 1 --span-minutes 180 --source-id d4-objects $(ls obj_*.jpg)
sh /storage/sd/vm-d4-input/vm_object_demo.sh start /storage/sd/vm-d4-store     # browse http://<player>:20398/#search?mode=objects
sh /storage/sd/vm-d4-input/vm_object_demo.sh stop
```
Production: set `visual_memory.objects.enabled` (with Stage 1+2 on) in the Argus config and ship the new package; the
dashboard then shows "Select Object" in the frame detail and an "Object search" mode on Visual Search.

## Limitations / follow-ups
* Manual ROI only; no detector. Retrieval quality on real, cluttered scenes and real product photos is **not** established;
  the numbers above come from a synthetic set plus one real photo pair.
* Exact brute-force scan: ~62 ms at 10,000 objects, linear in size. Beyond ~50,000 regions an ANN/sqlite-vec index is needed.
* Embeddings of loose crops are sensitive to how much background the box includes (6 % edge error was tested, not larger).
* No endpoint to delete a region from the dashboard yet (library `delete_region`/`set_label` exist).
* Crops are regenerated, so an expired parent JPEG removes the thumbnail/overlay (the embedding and box are kept and shown as expired).
* The scan-time gap between region and ROI queries is unexplained.
* Hourly frame-directory partitioning (FAT32 limit, `docs`/BACKLOG) is unaffected by D4 (no files are written per object) and is scheduled with D5, which writes screenshots continuously.
* MobileNetV2 pretrained-weight licence must be reviewed before commercial redistribution.
