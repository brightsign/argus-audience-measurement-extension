# libvisualmemory

Reusable C++20 static library (no Argus dependency) that

* **Stage 1** samples frames, JPEG-encodes them (TurboJPEG) and stores them on disk with SQLite metadata,
  bounded asynchronous queue, age retention and a storage quota;
* **Stage 2 (optional)** extracts an image embedding for each stored frame on a separate worker, stores the
  vector in the same SQLite database and answers *"find the most similar historical frames"* queries.

Stage 2 is **inert unless enabled**: `Config::embedding.enabled` defaults to `false`, and nothing runs unless an
`EmbeddingProvider` is injected. Stage 1 behaviour and public API are unchanged.

```
Argus capture thread ──try_submit()──► Stage 1 queue (bounded, 4) ──► JPEG + SQLite row
                                                                   │  (id, relative path)
                                                                   ▼
                                       Stage 2 queue (bounded, ~100 B/entry, no pixels)
                                                                   ▼
                       embedding worker: read JPEG → decode → provider.embed() → validate → L2-normalise
                                                                   ▼
                                  one transaction: frame_embeddings row + vec0 row (sqlite-vec)
search_similar()/search_by_image()  ── runs on the CALLER's thread ──►  exact top-K over the vec0 table
```

The capture thread never touches Stage 2: it only calls `try_submit()`. A slow or failed NPU can only fill the
bounded Stage 2 queue (drops are counted; `backfill_embeddings()` recovers them later).

## Measured on the XT5 (summary; details in `STAGE2_RESULTS.md`)

| | Stage 1 | Stage 1 + 2 | delta |
|---|---|---|---|
| FPS (3×10 min, alternating) | 29.05 | 29.04 | none |
| RSS mean / peak | 69.5 / 73.3 MiB | 86.6 / 90.4 MiB | **+17.1 MiB** |
| CPU mean (100 % = one core) | 48.6 % | 50.9 % | +2.3 pp |
| NPU core 0 | 79.5 % | 79.2 % | none |

FP16 embeddings vs ONNX FP32 on device: cosine 0.99999 mean (0.99993 min), top-5 overlap 0.998. NPU inference 5.9 ms
(preprocessing 10.9 ms idle). Storage ≈ 5.1 kB per FP32 vector (+ ≈ 27 kB per JPEG). Exact search: 0.74 ms @100, 5.1 ms @1k,
41 ms @10k vectors (≈ 4 µs/vector; **not ANN**). Known issues: `BACKLOG.md`. Model provenance, hashes, licences, distribution:
`models/README.md`. The model binaries are **not in git**; fetch them and run `scripts/verify_models.sh`.

**Which config file is used:** Argus reads `/storage/sd/configs/argus-config.json` first and `config.json` only if it is absent.

## Build

| CMake option | Default | Meaning |
|---|---|---|
| `ENABLE_VISUAL_MEMORY` (Argus top level) | OFF | build/link the library and the Argus adapter |
| `ENABLE_VISUAL_EMBEDDINGS` (Argus top level) | OFF | Stage 2 in Argus: RKNN backend + `vm_embed_demo` CLI + model packaging |
| `ENABLE_SQLITE_VEC` | ON | compile the vendored sqlite-vec (`third_party/sqlite-vec`); OFF = BLOB + C++ cosine fallback |
| `ENABLE_RKNN_EMBEDDINGS` | OFF | build `visualmemory_rknn` (needs `rknn_api.h`, `librknnrt`); forced ON by `ENABLE_VISUAL_EMBEDDINGS` |
| `VM_SQLITE_AMALGAMATION_DIR` | – | **dev/test only**: build SQLite from an amalgamation (e.g. 3.26.0) instead of the system library |

The Stage 2 *core* (embedding worker, vector store, search API, mock provider) is always compiled — it needs only
SQLite and TurboJPEG — so the library builds on plain x86 Linux without RKNN:

```
cmake -S libs/visual-memory -B build-vm && cmake --build build-vm && ctest --test-dir build-vm
# exact-target test against SQLite 3.26.0 (no libsqlite3-dev needed):
cmake -S libs/visual-memory -B build-vm -DVM_SQLITE_AMALGAMATION_DIR=/path/to/sqlite-amalgamation-3260000
```

Nothing is downloaded at build time.

## Runtime configuration (Argus `configs/config.json`)

Both stages are **disabled in the committed config**. Stage 2 runs only when *all* of these hold: the binary was
built with `-DENABLE_VISUAL_EMBEDDINGS=ON`, `visual_memory.enabled`, `visual_memory.vector_search_enabled` and
`visual_memory.embedding.enabled` are `true`, and the model loads.

```json
"visual_memory": {
  "enabled": false, "storage_root": "/storage/sd/visual-memory", "sample_interval_ms": 5000,
  "stored_width": 640, "stored_height": 360, "jpeg_quality": 75, "max_pending_frames": 4,
  "max_storage_bytes": 4000000000, "retention_hours": 48,
  "vector_search_enabled": false,
  "embedding": { "enabled": false, "backend": "rknn",
                 "model_path": "model/visual_memory/mobilenet_embedder.rknn",
                 "max_pending": 4, "retention_days": 7, "backfill_on_startup": false,
                 "npu_core": -1, "prefer_sqlite_vec": true }
}
```

`retention_hours` governs **JPEGs**, `embedding.retention_days` governs **vectors**; they are independent (below).
`max_storage_bytes` bounds JPEG bytes **plus** vector bytes (`dimension*4` per vector).

## Public API additions (all additive; Stage 1 signatures unchanged)

```cpp
vm.set_embedding_provider(std::shared_ptr<EmbeddingProvider>);   // before start()
bool embeddings_active() const;
std::vector<SearchResult> search_similar(const std::vector<float>&, const SearchOptions&,
                                         SearchStatus* = nullptr, const EmbeddingSpec* query_space = nullptr);
std::vector<SearchResult> search_by_image(const ImageView&, const SearchOptions&, SearchStatus* = nullptr);
std::vector<SearchResult> search_by_jpeg(const std::vector<uint8_t>&, const SearchOptions&, SearchStatus* = nullptr);
size_t backfill_embeddings(size_t max_frames, int64_t deadline_ms = 0);
EmbeddingStats embedding_stats() const;
```

`EmbeddingProvider` (`embedding_provider.h`) gained `embed(ImageView, out)` (pure virtual), `ready()`,
`last_error()`, `provider_stats()`, `shutdown()`; `spec()` and `extract(OwnedFrame)` keep their Stage 1 meaning.
`FrameInfo::image_available` is new. `MockEmbeddingProvider` is a deterministic provider for tests/demos.
The RKNN provider lives in `backends/rknn/` and is **not** linked into the core.

### Search semantics

* Cosine similarity on L2-normalised vectors (`similarity = 1 - distance`, range −1…1), results sorted by
  similarity descending (ties by frame id). `top_k` is clamped to `[1,100]`.
* Filters: capture-time `[start,end)`, `source_id`. They are pushed into the vector scan (vec0 metadata
  columns), not applied after the fact, so `top_k` results are returned whenever that many candidates exist.
* `min_similarity` is **opt-in** (`has_min_similarity`); there is no built-in threshold because none has been
  validated for any application.
* Errors are reported through `SearchStatus` (`NotEnabled`, `DimensionMismatch`, `InvalidVector` (NaN/Inf/zero
  norm), `ModelMismatch` (model name/version/sha/preprocessing/dimension differ), `ProviderFailed`, …).
* Each hit reports `image_available` (DB flag: JPEG already expired) and `image_exists` (file present now);
  `require_image=true` drops hits without a readable image.

### Scaling characteristics (read before relying on it)

sqlite-vec v0.1.6 `vec0` performs an **exact brute-force scan** of every row that passes the metadata filter.
It is **not** an approximate-nearest-neighbour index (no HNSW/IVF). Cost is O(N·d) per query; memory is one
128-row chunk at a time (chunk_size=128 → 655 kB for 1280-d). Measured numbers are in
`STAGE2_RESULTS.md`. A query holds the single SQLite connection mutex, so it briefly delays Stage 1/Stage 2
inserts; use time/source filters (they cut the scan) and keep `embedding.retention_days` realistic. A
time-bounded or ANN index is the next step if N grows into the hundreds of thousands.

## Schema (PRAGMA user_version = 2, additive migration; v1 databases upgrade in place)

* `frames` – unchanged columns + `image_available INTEGER NOT NULL DEFAULT 1`.
* `embedding_models(model_id, model_name, model_version, model_sha256, preprocessing, dimension, dtype, metric,
  backend, created_utc_ms)` – one row per distinct embedding space (UNIQUE over all identity fields).
* `frame_embeddings(frame_id, model_id, status, created_utc_ms, error)` – PK `(frame_id, model_id)`;
  `status` 1 = vector stored, 2 = permanent failure (marker, no vector).
* `embedding_sources(ix, source_id)` – interns source ids for the integer metadata filter.
* `vec_emb_<model_id>` – `vec0(embedding float[D] distance_metric=cosine, capture_ms integer, source_ix integer,
  chunk_size=128)`, **rowid = frames.id** (stable observation id). Fallback: `blob_emb_<model_id>`.

Because every model has its own vector table, vectors from different models/preprocessing versions are never
compared. Vector + marker are written in **one transaction**; startup `reconcile()` repairs any divergence.
All SQL uses prepared statements. FP32 vectors only (INT8 *storage* is deferred until retrieval quality is measured).

## Retention (independent lifetimes)

* JPEG older than `retention_hours` (or evicted by quota): the file is deleted. If the frame has an embedding, the
  `frames` row is **kept** with `image_available=0`, so its vector stays searchable; otherwise the row is deleted
  exactly as in Stage 1.
* Embedding older than `embedding.retention_days`: vector + marker are deleted; the `frames` row is deleted too if
  its JPEG is already gone.
* Quota (`max_storage_bytes`): oldest JPEGs first, then oldest embeddings.

## Failure behaviour

Provider not ready / model failed to load → Stage 2 inactive, Stage 1 unaffected (`embedding_stats().status`).
Inference failure → bounded retries, no DB marker (recoverable; backfill retries). Missing/corrupt JPEG or
invalid vector (NaN, Inf, near-zero norm, wrong dimension) → permanent failure marker, never retried.
Stage 2 queue full → drop counted. Process killed mid-run → databases reopen consistently (SQLite journal),
orphaned `.tmp` files are reaped, `backfill_embeddings()` fills any gap.

## sqlite-vec compatibility with the BrightSign SDK SQLite (3.26.0)

* Vendored: sqlite-vec **v0.1.6** amalgamation, unmodified (`third_party/sqlite-vec/VERSION` has hashes).
  Compiled as a static C object and registered explicitly with `sqlite3_vec_init(db)` — no runtime extension loading.
* **Build:** with the SDK `sqlite3.h` (3.26.0) it fails with exactly one error:
  `sqlite-vec.c:9621: 'SQLITE_INNOCUOUS' undeclared` (that flag was added in SQLite 3.31.0). CMake detects this and
  adds `-DSQLITE_INNOCUOUS=0`; the flag is only a security hint for `sqlite3_create_function_v2` and is ignored by
  old SQLite. All other APIs used (`sqlite3_str_*` 3.24, `sqlite3_value_pointer` 3.20, `sqlite3_value_nochange` 3.22, …)
  exist in 3.26.0. `sqlite3_vtab_in` (3.38) is compiled out by sqlite-vec itself.
* **Runtime (tested on SQLite 3.26.0, host and XT5):** `vec0` create/insert/delete/KNN work. Differences from newer
  SQLite: the `ORDER BY distance LIMIT n` form needs SQLite ≥ 3.41, so queries use the older, supported
  `WHERE embedding MATCH ? AND k = ?` form; `IN (...)` subquery pre-filtering needs 3.38, so time/source filtering
  uses vec0 **metadata columns** (`capture_ms`, `source_ix`) which work on 3.26.
* Known upstream issue: v0.1.6 `vec0_free()` does not free the metadata-chunk-name strings (2 small allocations per
  vec0 table per connection; found by ASan). One-off, bounded, not per-query. The vendored file is not patched.

## Tests

`ctest` (GoogleTest): Stage 1 (24) + Stage 2 (40): pp1 preprocessing, vector store on both backends vs a
brute-force oracle, filters, idempotence, model separation, v1→v2 migration, reconcile, retention independence,
queue bounds, provider failures, backfill, restart persistence, missing/corrupt JPEG. Also run clean under
ASan/UBSan (only the upstream sqlite-vec leak above) and TSan (no races).

## Tools

`vm_embed_demo` (built with `VISUALMEMORY_BUILD_EXAMPLES`, shipped in the XT5 package) —
`embed`, `ingest`, `search`, `backfill`, `stats`, `bench-db`; `--provider mock|rknn`. See `STAGE2_RESULTS.md`.
Model/pipeline scripts: `models/README.md`, `scripts/`.
