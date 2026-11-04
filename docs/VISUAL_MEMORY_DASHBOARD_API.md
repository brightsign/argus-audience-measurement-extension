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

## Proxy-level responses (dashboard-server, before the request reaches `vm_api_server`)
`401 unauthorized` (token configured, missing/wrong), `403 forbidden` (POST with missing/foreign Origin/Referer),
`405 method_not_allowed`, `413 too_large` (body > 8 MiB), `502 vm_api_unavailable` (API process down).
All use `{"error","message"}`. Counters in `runtime.stage1/stage2` are since the Argus process started.
Search `results[]` also carry `width`/`height`. `model` is the stored embedding space (`null` only if several models exist and no provider is loaded).
