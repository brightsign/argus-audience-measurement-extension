# Visual Memory in the Argus dashboard (D1–D3)

Adds four navigation sections to the existing dashboard (Audience Measurement unchanged, Visual Memory, Visual Search,
Proof of Play = labelled preview placeholder) with a live Visual Memory status pill, a historical frame gallery and
image/scene similarity search. API contract: [VISUAL_MEMORY_DASHBOARD_API.md](VISUAL_MEMORY_DASHBOARD_API.md).

## Architecture
```
browser ──► dashboard-server :20300 ──(/api/vm/*, loopback)──► vm_api_server :20310 ──► libvisualmemory VisualMemoryReader
              static UI, /video proxy,                          own process, read-only SQLite,    (read-only open, vec0 search,
              method/Origin/token/size rules                    lazy RKNN model on NPU core 1     RKNN provider for the query image)
Argus (attention_demo) ──► libvisualmemory writer (unchanged capture path) ──► SQLite + JPEGs on SD
Argus status thread (every 5 s) ──► /tmp/argus_vm_status.json ──► vm_api_server /api/vm/status
```
* **Capture thread untouched.** The new Argus-side code is one background thread that writes a ~1 kB status JSON to tmpfs
  (atomic rename) from lock-free/cached counters. No SQL, no inference, no I/O on the capture thread.
* **libvisualmemory stays Argus-independent.** New: `VisualMemoryReader` (`reader.h`: summary, paged list, frame detail,
  bounded JPEG/thumbnail read, search by upload/frame/vector, `cancel_search()`), `SqliteStore::open_readonly`
  (no migration, no writes, no journal), shared `search_core`. Dashboard code lives in `src/tools/vm_api_server.cpp`.
* **Isolated reader process.** Visual Memory is read through a second read-only SQLite connection; Argus's writer, queues and
  NPU contexts are not shared. Query embedding runs on NPU **core 1** (`visual_memory.api.npu_core`, default 1); the
  detectors use core 0. The model is loaded on the first upload search and released after `idle_unload_s` (120 s).
* **Bounded, cancellable searches.** One search at a time (HTTP 429 otherwise), top_k ≤ 50, 8 MiB upload cap, 10 s deadline
  (`visual_memory.api.search_timeout_ms`) and client disconnect both call `sqlite3_interrupt`.
* **No paths exposed.** Frames are addressed by integer id; error strings never include filesystem paths. Uploaded images live
  in memory for one request only. `vm_api_server` binds 127.0.0.1; the proxy allows GET and POST `/api/vm/search/*` only,
  requires same-origin for POST, and supports an optional `dashboard.vm_token` cookie/header secret.
* **Disabled by default.** The committed config keeps `visual_memory.enabled=false`; `vm_api_server` is started by `bsext_init`
  only if packaged, reads the same active config as Argus (`/storage/sd/configs/argus-config.json` first) and stays idle
  (no DB open, no model load) unless Visual Memory is enabled there.

## Build / package
```
# XT5 (VM + Stage 2 + API server)
make build SOCS=rk3588 EXTRA_CMAKE_FLAGS="-DENABLE_VISUAL_MEMORY=ON -DENABLE_VISUAL_EMBEDDINGS=ON"   # or scripts/build.sh rk3588
./package --ext-only --soc RK3588          # packages vm_api_server, vm_embed_demo, dashboard (dev/ excluded), models
# host checks
cmake -S libs/visual-memory -B build-vm && cmake --build build-vm && ctest --test-dir build-vm    # 73 tests
(cd dashboard-server && go vet ./... && go test ./...)
python3 dashboard/dev/mock_vm_api.py   # UI-only mock (reports itself as MOCK); never packaged
```
Config (all optional, `visual_memory.api`): `npu_core` (default 1), `search_timeout_ms` (10000), `idle_unload_s` (120);
`dashboard.vm_token` (default none).

## XT5 scratch validation (no install, Argus and port 20300 untouched)
Setup: new `vm_api_server` + `dashboard-server` run from RAM on ports 20311 (API) / 20399 (dashboard) against a **copy** of the
existing benchmark store (`/storage/sd/vm-bench3-store`: 131 real camera frames, 131 FP16 embeddings, SHA-256 of the original
DB identical before/after). The scratch config enabled Visual Memory only for that process; the player's config files were not
modified. See STAGE2/BACKLOG for the caveat that the installed package predates the Argus status thread.

| check | result |
|---|---|
| audience dashboard through the new server | live video, 29–30 fps, NPU 75–80 %, KPIs via MQTT-WS all working |
| status from real data | 131 frames / 131 with image / 131 embeddings / 2.76 MB JPEG + 0.67 MB vectors + 1.44 MB DB, model `mobilenet_v2_embedder tv-imagenet1k-v1 pp1 dim 1280`, oldest/newest 09:11:34–09:22:26 UTC; no runtime snapshot → pill "degraded" with the reason listed |
| gallery / filters | 24 per page, thumbnails, `start`/`end` split 66 + 65 = 131, source filter 131 / unknown 0, asc/desc, offset paging, timestamps consistent with the stored file names |
| frame search (stored vector) | top-5 sim 0.941–0.894, server 4 ms, results are time-adjacent frames of the same scene |
| upload search (RKNN on core 1) | query = stored frame → itself 1.0000 then neighbours 0.941…; query = 85 % crop +15 % brightness → source frame #119 first (0.9316) |
| expired / missing JPEG (scratch copy edited) | 3 frames `image_available=0` → 410 `image_expired`; 2 files deleted → 404 `image_missing`; UI shows labelled placeholders, 0 broken `<img>`; they still appear in search results flagged; `require_image=1` drops them |
| invalid queries | non-JPEG → 400, truncated JPEG → 400 `jpeg decode: Premature end`, 25 Mpixel JPEG → 400, 9 MiB → 413, `top_k=0` → 400, unknown frame → 400, cross-origin POST → 403, DELETE → 405; UI shows readable messages |
| concurrency | 4 parallel clients → single-flight, overlapping requests got 429 |

Latency (client on the LAN, through the dashboard proxy; n=30 each, server-side time in brackets):

| call | p50 | p95 |
|---|---|---|
| GET /status | 2.6 ms | 3.3 ms |
| GET /frames?limit=24 | 6.2 ms | 6.9 ms |
| GET /frames/{id} | 2.0 ms | 4.0 ms |
| GET thumb w=240 | 2.9 ms | 4.7 ms |
| GET full JPEG | 1.9 ms | 2.2 ms |
| POST search/frame (stored vector) | 4.5 ms (4 ms) | 9.6 ms |
| POST search/image warm (decode + pp1 + NPU + scan, 131 vectors) | 40 ms (33 ms) | 55 ms |
| first upload after idle (loads RKNN model) | 255 ms | |

Sustained stress (back-to-back uploads, ~29 searches/s for 30 s) plus idle/other phases, sampled at 1 Hz on the device
(100 s): **Argus FPS 29.0–30.0 (mean 29.05; baseline 29)**; NPU core 0 (detectors) 70–84 % as before, **core 1 10–14 %** only
during searches; Argus RSS 69–72 MB (unchanged); `vm_api_server` RSS 5.1 MB idle, 27–29 MB with the model loaded (peak 29.4 MB);
`dashboard-server` 10–11 MB.

Screenshots (contain a person's face; kept out of git): `scratchpad/shots3/` (real-device) and `scratchpad/shots/` (mock-API UI states,
mobile 390 px, auth, API-down).

## Live integration test on the XT5 (installed package, 2026-11-04)
Package `argus-ext-1793816098.zip` (sha256 fdefb645…), rollback zips `argus-ext-1793800886.zip` / `argus-ext-1793779707.zip` kept on the SD card.
Verified live: Argus status snapshot thread (5 s), live gallery growth, filters, frame/upload search, expired and missing JPEGs,
token auth, `bsext_init` start/stop/restart of `vm_api_server`, crash/restart recovery, failure reporting.

**Controlled FPS benchmark** (A/B/A/B, 600 s each after a 60 s warm-up, same camera/scene/models, camera control fixed at
`exposure_auto_priority=0`, 1 Hz samples; A = Visual Memory off, B = Stage 1+2 with dashboard and API running):

| phase | FPS mean | median | p5 | min | < 25 fps | drops | Argus CPU (1 core = 100 %) | Argus RSS mean/peak |
|---|---|---|---|---|---|---|---|---|
| A1 | 29.06 | 29 | 29 | 28 | 0 % | 0 | 47.9 % | 68.6 / 70.6 MB |
| B1 | 29.05 | 29 | 29 | 28 | 0 % | 0 | 50.5 % | 86.9 / 89.3 MB |
| A2 | 29.06 | 29 | 29 | 28 | 0 % | 0 | 48.8 % | 68.8 / 71.0 MB |
| B2 | 29.06 | 29 | 29 | 28 | 0 % | 0 | 49.4 % | 86.9 / 89.1 MB |

FPS difference A→B ≤ 0.01 fps (≈ 0.03 %), i.e. far below the 5 % target for this workload (one person-scale scene, 5 s sampling,
FP16 MobileNetV2, 640×480 USB camera). NPU core 0 ≈ 80 % in every phase, core 1 idle between queries; SoC/NPU/big-core ≤ 34 °C
(no throttling); Stage-1/Stage-2 queue depth ≤ 1, 0 drops, 0 failures; no UVC/USB kernel messages. Extra memory ≈ 18 MB (limit 64 MiB).
Capture hook 0.41 ms p50 / 0.85 ms p95; embedding inference 6.3 ms p50; slow writer/DB operations (up to ≈ 1 s) are on background threads only.

**The earlier 18–19 fps dips were the camera, not Visual Memory.** With Argus *stopped*, `v4l2-ctl` streaming straight from the
camera delivered 19.9 fps while UVC `exposure_auto_priority` was 1 (the camera's state after every power-up; its default is 0)
and a steady 30.00 fps with 0. Argus reported 0 dropped frames and 0 ms queue latency during the dips, and the VM-off phase
dipped as well. New optional config `input.usb.exposure_auto_priority` (0/1, unset = existing behaviour) applies the control at
start for USB sources only (`docs/CONFIGURATION.md`); the log reports requested/previous/actual value and the measured camera
delivery rate. Hardware check: unset → camera untouched; 0 → `previous=1 actual=0`; 1 → `previous=0 actual=1`; invalid value →
ignored with a warning; delivery 30.0–30.3 fps in every case. In bright light the image statistics are the same with 0 and 1 (mean luma
139 vs 147 within scene motion, sharpness 3370 vs 3466, noise 18.4 vs 18.8). Dim-light image quality with 0 (expected: darker/noisier,
not slower) has **not** been measured yet (BACKLOG VM-12).

**Dashboard additions**: *Sampling & processing* panel (configured sampling interval, stored size/quality, capture FPS from Argus's
existing `argus_fps_current` metric, frames sampled / accepted / embedded / dropped / failed from the existing Stage-1/Stage-2
counters, embedding model) and a read-only **dataset viewer** mode (`vm_api_server --viewer`, see `VISUAL_MEMORY_DEMO.md`).
Live check with the demo config: 40 sampled / 40 accepted / 40 embedded / 0 dropped / 0 failed, capture 29 fps, 120 s FPS mean 29 (min 29).

**Dataset search check** (`VISUAL_MEMORY_DEMO.md`): 64 people-free frames (32 scenes × original+variant) ingested through the real writer + NPU
embedder; 32 reference images (off-centre crop + brightness) → top-1 correct 32/32, precision@2 100 %, MRR 1.000, server search median ≈ 31 ms.

## Limitations
* Search is image/scene similarity from a whole-frame embedding — not object detection, not identity matching.
* Capture FPS comes from the loopback Prometheus exporter (`:9101`); it is shown as unavailable if the exporter is down.
* In viewer mode the runtime counters are intentionally not available.
* No TLS; token auth is optional and off by default (BACKLOG VM-9).
* Exact scan: 131 vectors 4 ms; ~4 µs/vector beyond that (BACKLOG VM-4).

## Next steps
* **D4 object-level search**: manual crop → query embedding (reuse the same model; crop passed through pp1), store optional
  region metadata (`x,y,w,h,label,source_frame_id`) in a new table keyed by frame id and a separate vec table per crop model;
  no assumption that the person detector recognises products. Needs an evaluation set before any claim.
* **D5 proof-of-play foundation**: separate event/evidence store (media id/hash, device id, UTC start/end/error), a
  presentation-output capture path validated on hardware (never the USB audience camera), reference-frame embeddings, explicit
  verification levels (`event-only`, `frame-similar`, `unverified`) and a PoP dashboard page.
