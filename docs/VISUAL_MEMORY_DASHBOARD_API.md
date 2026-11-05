# Visual Memory dashboard API (D1–D3)

Read-only (plus two bounded search POSTs) JSON API served under `/api/vm/` by `dashboard-server` (port 20300),
which reverse-proxies to `vm_api_server` (loopback `127.0.0.1:20310`). `vm_api_server` is a thin adapter over
`libvisualmemory`'s `VisualMemoryReader`; it never runs on the capture thread, never writes to the Visual Memory
store, and never accepts or returns filesystem paths (frames are addressed by integer `id`).

All timestamps are **UTC epoch milliseconds** (`*_utc_ms`). The browser converts to local time for display.
All responses: `Content-Type: application/json` (except images), `Cache-Control: no-store` for JSON,
`private, max-age=300` for thumbnails/images. Errors: `{"error":"<code>","message":"..."}` with an HTTP status.

## Access control
* `vm_api_server` listens on loopback only. The only route to it is the dashboard proxy.
* The proxy allows only `GET` and `POST /api/vm/search/*`, caps request bodies at 8 MiB, and for `POST`
  requires `Origin`/`Referer` host == `Host` (blocks cross-site form posts).
* Optional shared secret: if `dashboard.vm_token` is set in the active config, every `/api/vm/*` request needs
  the cookie `vm_token=<token>` (or header `X-VM-Token`). Missing/wrong => `401 {"error":"unauthorized"}`.
  `POST /api/vm/auth` with body `{"token":"..."}` sets the cookie (HttpOnly, SameSite=Strict) and returns `{"ok":true}`.
  `GET /api/vm/auth` returns `{"required":bool,"authenticated":bool}` (never 401).
* Uploaded query images are processed in memory and never written to disk or logged.

## GET /api/vm/status
Combines the live runtime snapshot Argus writes (`/tmp/argus_vm_status.json`, refreshed every 5 s) with
read-only store counts. Nothing is hardcoded; missing data is `null`.

```json
{
  "server_time_utc_ms": 1793780000000,
  "configured": true,             // visual_memory.enabled in the ACTIVE argus config
  "config_path": "/storage/sd/configs/argus-config.json",
  "vector_search_configured": true, "embedding_configured": true,
  "runtime": { "running": true, "snapshot_age_ms": 2100, "stale": false,
               "stage1": { "samples_seen":0,"accepted":0,"encoded":0,"dropped_queue_full":0,"dropped_invalid":0,
                           "jpeg_write_failures":0,"database_failures":0,"retention_deletions":0,
                           "queue_depth":0,"max_queue_depth":0,"bytes_on_disk":0 },
               "stage2": { "status":"active","backend":"sqlite-vec v0.1.6","embedded":0,"vectors_stored":0,
                           "vector_bytes":0,"queue_depth":0,"max_queue_depth":0,"dropped_queue_full":0,
                           "inference_failures":0,"invalid_vectors":0,"decode_failures":0,
                           "permanent_failures":0,"db_failures":0 } },
  "model": { "name":"mobilenet_v2_embedder","version":"tv-imagenet1k-v1","preprocessing":"pp1",
             "dimension":1280,"sha256":"...","dtype":"float32","metric":"cosine" },
  "store": { "available": true, "schema_version": 2, "vec_backend":"sqlite-vec v0.1.6",
             "frames":0,"frames_with_image":0,"embeddings_ok":0,"embeddings_failed":0,
             "jpeg_bytes":0,"vector_bytes":0,"db_bytes":0,"quota_bytes":4000000000,
             "retention_hours":48,"embedding_retention_days":7,
             "oldest_utc_ms":0,"newest_utc_ms":0,"sources":["/dev/video1"],"disk_free_bytes":0 },
  "search": { "available": true, "reason": "", "model_loaded": false, "busy": false },
  "errors": [ "human readable problems, e.g. 'runtime snapshot stale (95 s)'" ]
}
```
`viewer` is `true` when `vm_api_server` runs with `--viewer` (read-only view of a stored dataset with no live Argus writer; `runtime` is then `null` and no "no runtime snapshot" problem is reported).
`sampling` (from the active config: `interval_ms`, `stored_width`, `stored_height`, `jpeg_quality`) and `capture`
(`fps` = Argus's existing `argus_fps_current` metric read from the loopback exporter on :9101 and cached 2 s, plus
`dropped_frames`, `width`, `height`; `null` when the exporter is unreachable) complement `runtime`. The dashboard's
"Sampling & processing" panel shows these together with the existing `runtime.stage1/stage2` counters: sampled =
`samples_seen`, accepted, encoded, embedded = `stage2.embedded`, dropped = queue-full + invalid-frame drops,
failed = write/DB/embedding failures. `vm_api_server --metrics host:port` overrides the exporter address.
`configured:false` => everything else except `server_time_utc_ms`, `config_path`, `errors` is `null`.
`search.available:false` carries the reason (Visual Memory disabled, embeddings not configured, no stored vectors,
provider failed to load, ...).

## GET /api/vm/frames
Query: `start`, `end` (UTC ms, `end` exclusive, optional), `source` (optional), `limit` (1..100, default 24),
`offset` (default 0), `order` (`desc` default | `asc`).
```json
{ "total": 119, "limit": 24, "offset": 0,
  "frames": [ { "id": 321, "capture_utc_ms": 1793783494543, "source_id": "/dev/video1",
                "width": 480, "height": 360, "jpeg_bytes": 23131, "image_available": true,
                "image_exists": true, "has_embedding": true } ] }
```
`image_available` = DB flag (false once the JPEG expired but metadata/embedding is retained); `image_exists` =
file present now. Frames with either false must be rendered as "image expired/missing", not as broken `<img>`.

## GET /api/vm/frames/{id}
```json
{ "id": 321, "capture_utc_ms": 0, "created_utc_ms": 0, "source_id": "", "source_type": "usb", "device_id": "",
  "width": 480, "height": 360, "jpeg_bytes": 23131, "sha256": "…", "sequence": 157,
  "image_available": true, "image_exists": true,
  "embeddings": [ { "model_name":"mobilenet_v2_embedder","model_version":"tv-imagenet1k-v1","preprocessing":"pp1",
                    "dimension":1280,"status":"ok","created_utc_ms":0,"error":"" } ],
  "retention": { "jpeg_expires_utc_ms": 0, "embedding_expires_utc_ms": 0 } }
```
`status` is `ok` | `failed`. 404 `{"error":"not_found"}` for unknown ids.

## GET /api/vm/frames/{id}/image and /thumb
`/image` returns the stored JPEG. `/thumb?w=240` (w 64..640) returns a downscaled JPEG. If the JPEG is
expired/missing: `410 {"error":"image_expired"}` (DB says expired) or `404 {"error":"image_missing"}` (row says
available but the file is gone). Unknown id: `404 {"error":"not_found"}`.

## POST /api/vm/search/image
Body: raw JPEG bytes (`Content-Type: image/jpeg`, ≤ 8 MiB). Query: `top_k` (1..50, default 5), `start`, `end`,
`source`, `min_similarity` (optional, opt-in; no default threshold), `require_image` (`1` => drop hits whose
JPEG is gone).
## POST /api/vm/search/frame
No body. Query: `frame_id` (required) + the same options. Uses the frame's STORED vector when it has one (no
inference); otherwise embeds its JPEG. The query frame itself is excluded from the results.

Response (both):
```json
{ "label": "Image/scene similarity (whole-frame MobileNetV2 embedding). Not object detection, not identity matching.",
  "query": { "type": "upload|frame", "frame_id": null, "embedded_with": "stored vector|rknn" },
  "model": { "name":"…","version":"…","preprocessing":"pp1","dimension":1280 },
  "status": { "code":"ok","message":"","elapsed_ms":41.2,"candidates_scanned":10000 },
  "results": [ { "rank":1, "frame_id":62, "similarity":0.9607, "distance":0.0393,
                 "capture_utc_ms":0, "source_id":"", "width":480,"height":360,
                 "image_available":true, "image_exists":true } ] }
```
`status.code`: `ok`, `not_enabled`, `dimension_mismatch`, `invalid_vector`, `model_mismatch`, `provider_failed`,
`invalid_argument`, `database_error`, `busy`, `timeout`, `cancelled`. HTTP 200 for `ok`; 503 for
`not_enabled`/`provider_failed`; 400 for invalid input; 429 for `busy` (one search at a time); 504 for `timeout`.
Searches are exact brute-force scans (not ANN) bounded by `top_k<=50` and a server-side deadline (default 10 s)
that interrupts the SQLite query; closing the connection cancels a running search.

## D4 Object search endpoints (optional; `visual_memory.objects.enabled`, off by default)
Manually selected rectangles ("regions") of stored frames are embedded with the same MobileNetV2 model and kept in a
**separate** index (`<storage_root>/objects.db`); object vectors are never compared with full-frame vectors. Coordinates
are pixels of the **stored JPEG** (`frames/{id}` → `width`/`height`), origin top-left. When the feature is off every
object route answers `503 {"error":"not_enabled"}`; `GET /api/vm/status` carries an `objects` block
(`enabled`, `available`, `regions`, `vector_bytes`, `db_bytes`, `max_regions`, `models`).

| Route | Body / query | Result |
|---|---|---|
| `POST /api/vm/objects` | JSON `{frame_id,x,y,w,h,label?}` (≤ 16 KiB) | `201 {"status":"indexed","region":{…}}`; re-indexing the same frame/ROI/model → `200 {"status":"duplicate"}` with the same `region_id` (a new label updates the old one). `400 invalid_roi` (outside the image, below `min_roi_px`, negative…), `404 not_found` (frame), `410 image_unavailable` (JPEG expired), `429 busy`, `503 provider_failed` |
| `GET /api/vm/objects` | `limit` (1..100), `offset`, `label`, `source`, `frame_id`, `start`, `end` | `{"total","objects":[region…]}` newest first |
| `GET /api/vm/objects/{id}` | – | region + `image_available`/`image_exists` of the parent frame |
| `GET /api/vm/objects/{id}/image?max=240` | `max` 32..640 | JPEG crop **regenerated** from the stored frame (no crop file is stored). `410` if the frame JPEG expired, `404 image_missing` |
| `POST /api/vm/objects/search` | JSON: `{region_id}` **or** `{frame_id,x,y,w,h}`, plus `top_k` (1..50), `label`, `source`, `start`, `end`, `min_similarity?`, `require_image?` | `{"label","query","index_regions","status":{code,message,elapsed_ms,candidates_scanned,empty_index?},"results":[{rank,region_id,frame_id,bbox:{x,y,w,h},image_width,image_height,label,capture_utc_ms,source_id,similarity,distance,image_available,image_exists}]}` sorted by cosine similarity. `status.code` as for frame search; `model_mismatch` (400) when the index was built with a different model/preprocessing |
| `GET /api/vm/frames/{id}/crop?x&y&w&h&max=240` | – | preview crop of a stored frame (any rectangle inside the image); the frame is never modified |

A region search (`region_id`) uses the stored vector (no inference) and excludes the query region. ROI searches use the
NPU and share the one-at-a-time guard with indexing (`429 busy`). The deadline/disconnect watchdog of frame search
applies. Scores are cosine similarities of image embeddings, **not identification probabilities**. The proxy accepts
POST only on `/api/vm/search/*`, `/api/vm/objects` and `/api/vm/objects/search` (same-origin required; object bodies are
capped at 16 KiB). No route accepts a filesystem path.

## D5 Proof of Play endpoints (optional; `visual_memory.pop.enabled`, off by default)
Served by the same `vm_api_server` and proxied by the dashboard server under `/api/pop/` with the same rules (POST only on the routes
marked POST, same-origin required, 16 KiB body cap, token if configured). When disabled, `GET /api/pop/status` answers `{"enabled":false}`
and every other route answers `503 not_enabled`. Full description: `docs/VISUAL_MEMORY_PROOF_OF_PLAY.md`.

| Route | Purpose |
|---|---|
| `GET /api/pop/status` | Store counters, adapter state (events ingested, errors, last message), thresholds, evidence source. |
| `GET /api/pop/system` | Read-only snapshot for performance work: system CPU jiffies, MemAvailable, per-process CPU jiffies/RSS for the Argus processes, NPU load, temperatures. |
| `GET /api/pop/media`, `POST /api/pop/media` | List / register (upsert) an advertisement: `media_id, display_name, kind(file\|rtsp), uri, sha256, codec, duration_s, slot_s, width, height, fps`. |
| `GET /api/pop/media/{id}`, `GET .../references` | Asset and its reference frames (`ref_id, offset_ms, image_url`). |
| `POST /api/pop/media/{id}/references/build[?replace=1]` | Embeds every `<store>/pop/refs_incoming/<id>/<offset_ms>.jpg` into the reference library (`replace=1` first clears the old set: live sources). |
| `GET /api/pop/media/{id}/references/{ref_id}/image` | Reference frame JPEG. |
| `GET /api/pop/sessions?media_id&status&start_utc_ms&end_utc_ms&limit&offset` | Playback history with the verdict (`status`, `status_label`, `reason`, counters). |
| `POST /api/pop/sessions` | Schedule a play: `{expected_media_id, session_id?, slot_s?, capture_offsets_s?}` (+ test hooks `play_media_id, interrupt_at_s, break_capture` when `allow_test_jobs`). Writes the job file for the player application; 201. |
| `GET /api/pop/sessions/{id}` (`/export`) | Session, events, evidence, reference frames of the expected media and the stored verification with per-frame scores. |
| `POST /api/pop/sessions/{id}/verify` | Re-run the verification with the current thresholds. |
| `GET /api/pop/sessions/{id}/evidence/{evidence_id}/image` | Display screenshot JPEG (410 once retention removed it). |
| `GET /api/pop/sessions/export.csv[?media_id&status]` | One CSV row per session. |

Status values: `scheduled`, `playback_reported`, `visual_match`, `playback_corroborated`, `mismatch`, `incomplete`, `capture_unsupported`.

## Proxy-level responses (dashboard-server, before the request reaches `vm_api_server`)
`401 unauthorized` (token configured, missing/wrong), `403 forbidden` (POST with missing/foreign Origin/Referer),
`405 method_not_allowed`, `413 too_large` (body > 8 MiB), `502 vm_api_unavailable` (API process down).
All use `{"error","message"}`. Counters in `runtime.stage1/stage2` are since the Argus process started.
Search `results[]` also carry `width`/`height`. `model` is the stored embedding space (`null` only if several models exist and no provider is loaded).
