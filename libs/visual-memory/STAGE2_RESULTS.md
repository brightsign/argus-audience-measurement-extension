# Stage 2 results (measured on a BrightSign XT5 / RK3588, BrightSignOS 9.1.139, SDK SQLite 3.26.0)

Model: `mobilenet_v2_embedder` / `tv-imagenet1k-v1`, 1280-d, preprocessing `pp1` (see `models/README.md`).
All numbers below were measured; nothing is extrapolated. Stage 2 is disabled by default.

## 1. Embedding correctness (device NPU vs ONNX Runtime FP32, 101 images)

| variant | cosine vs ORT mean / min | max abs diff | top-5 overlap | top-1 agreement |
|---|---|---|---|---|
| RKNN FP16 | 0.999988 / 0.99993 | 0.0015 | 0.998 | 1.00 |
| RKNN INT8 | 0.982 / 0.962 | 0.046 | 0.861 | 0.95 |

FP16 is the shipped model. INT8 (41-image calibration set) loses retrieval quality and is **not** enabled.
INT8 tensor quantisation (from the device): input scale 0.0186585 zp -14, output scale 0.0187422 zp -128.
The RKNN tensors: input NHWC [1,224,224,3] FP16 path (float fed), output 1280.

## 2. Single-image latency (Argus stopped, idle NPU, 300 runs)

| | p50 | p95 | p99 |
|---|---|---|---|
| preprocessing (scalar C++) | 10.9 ms | 10.9 ms | – |
| RKNN inference FP16 | 5.9 ms | 5.9 ms | 5.9 ms |
| embed wall (preprocess+infer) FP16 | 16.8 ms | 16.8 ms | 16.8 ms |
| embed wall INT8 | 15.4 ms | 15.4 ms | 15.5 ms |

## 3. Stage 1 vs Stage 1 + Stage 2, live camera, same detectors/config (3 alternating 10-min pairs, 60 s warm-up)

Source: `vm_ab3.sh`. Each phase uses a fresh store; cadence 5 s (119–120 frames per phase).

| metric | Stage 1 only | Stage 1 + 2 | delta |
|---|---|---|---|
| FPS (median of 5 s samples) | 29 | 29 | none (mean 29.05 vs 29.04, -0.03 %) |
| RSS mean | 69.5 MiB | 86.6 MiB | **+17.1 MiB** |
| RSS peak (VmHWM) | 73.3 MiB | 90.4 MiB | +17.1 MiB |
| CPU mean (100 % = 1 core) | 48.6 % | 50.9 % | +2.3 pp (≈ +0.3 % of the 8-core SoC) |
| CPU peak, 1 s window | 79.5 % | 81.5 % | +2 pp (within noise) |
| NPU core 0 load mean / peak | 79.5 / 85 % | 79.2 / 85 % | none; cores 1,2 idle in both |
| frames stored per phase | 119–120 | 119–120 (all 119 embedded) | – |

Per-run values: CPU S1 48.7/48.7/48.4, S2 51.6/50.4/50.6; RSS S1 69.4/69.5/69.5, S2 86.7/86.4/86.7.
Targets: extra RSS < 256 MiB ✔ (17 MiB); FPS regression < 5 % ✔ (≈0); 5 s cadence kept ✔; capture thread never blocked ✔
(queue drops 0, `drop_full=0`, hook sample p99 0.9 ms with Stage 2 on and off).

Stage 2 stage latencies in the live runs (n=119 per run, three runs agree):

| stage | p50 | p95 | p99 |
|---|---|---|---|
| preprocessing | 10.8 ms | 52 ms | 55 ms |
| RKNN inference | 6.3 ms | 6.5 ms | 6.6–7.2 ms |
| JPEG decode | 1.6 ms | 3.9 ms | – |
| vector insert (SD card) | 25–28 ms | 72–109 ms | 940–970 ms (max 1.0–1.2 s) |
| total per frame | 50–54 ms | 97–127 ms | 960–990 ms |

Queue: Stage 2 max depth 1, 0 drops, 0 failures. Preprocess p95 is higher live than idle because the detectors load the CPU.
Insert p99 is the SD card (FAT32) commit latency; the same ~0.4–1.5 s commit stalls appear in Stage 1 DB writes
(Stage 1 `db_ms` p99 0.16–1.5 s with Stage 2 off) and run on the worker thread only.

Storage per phase (10 min): JPEGs 3.2 MB (≈27 kB/frame); SQLite 84 kB (Stage 1) vs 1.41 MB (Stage 1+2: +5.1 kB vector + vec0 chunk overhead
per frame, 119 vectors = 609 kB raw FP32).
Frame content check: four frames sampled across the last Stage 1+2 phase (480x360, JPEG q75, mean luma 119–127, full 0–255 range) were
viewed and show the intended indoor camera scene, well lit, correctly decoded and correctly coloured (4:3 camera scaled to fit 640x360
keeps its aspect ratio, hence 480x360). Not dark, not corrupted. The frames contain a person's face, so they are not committed.

## 4. Vector search scaling, on-device (SD card, mock provider isolates the DB, dim 1280, exact scan)

Search = top-5, exact brute force over all rows passing the filter (sqlite-vec vec0, **not ANN**). Device, Argus stopped:

| N | full scan p50 / p95 | 10 % time window p50 | source filter p50 | DB size |
|---|---|---|---|---|
| 100 | 0.74 / 0.81 ms | 1.0 ms | 0.9 ms | 0.74 MiB (7.7 kB/vector) |
| 1,000 | 5.1 / 5.5 ms | 7.7 ms | 6.4 ms | 5.46 MiB (5.7 kB/vector) |
| 10,000 | 41.5 / 43.7 ms (p99 67 ms) | 68 ms | 54 ms | 53.1 MiB (5.6 kB/vector) |

Cost is linear (≈4 µs per vector). At 5 s cadence and 7-day embedding retention N≈120k per source: ≈0.5 s per full scan —
use time/source filters, but note the time window did **not** reduce the scan cost on vec0 here (metadata filtering is
evaluated per row). Over ~100k vectors an ANN/time-partitioned index is the next step. x86 host reference: 23 ms at 10k.
Ingest of 10,000 frames (JPEG + vector) on the SD card took 1037 s (Stage 1 FAT writes dominate); vector insert alone
p50 34 ms, p95 62 ms, p99 1.0 s.

## 5. End-to-end demo on the XT5 (embedding + persistence + retrieval together)

Dataset: 100 JPEGs = 40 public base images + 60 variants (flip, brightness, crop, JPEG quality), seed 20260101, built by
`scripts/make_demo_dataset.py <outdir>` (reads ImageNet-val/COCO sample images from the local RKNN model zoo; no network);
frames carry synthetic timestamps 5 s apart. The reference query `reference.jpg` is an off-centre 85 % crop, brightened, of
`ILSVRC2012_val_00018512`; it is not one of the 100 frames.

```
vm_embed_demo ingest --root <store> --dir <frames> --manifest manifest.json --provider rknn --model mobilenet_embedder.rknn
vm_embed_demo search --root <store> --query reference.jpg --top 5 --provider rknn --model mobilenet_embedder.rknn
Query: reference.jpg   (sqlite-vec v0.1.6; scanned 100 vectors; end-to-end 21.5 ms incl. query embedding)
Rank  Frame  Similarity  Timestamp (UTC)      Image
1     62     0.9607      2026-01-01 10:05:05  frames/2026/01/01/demo-camera/1767261905000_62_….jpg
2     19     0.9548      2026-01-01 10:01:30  ...1767261690000_19_….jpg
3     99     0.9548      2026-01-01 10:08:10  ...1767262090000_99_….jpg
4     6      0.6382      2026-01-01 10:00:25  ...
5     67     0.6328      2026-01-01 10:05:30  ...
```

Ground truth from the manifest: frames 62 (original), 19 (flipped) and 99 (brightness -15) are the three frames derived from the same base image;
ranks 4–5 are other images at ≈0.63. Similarity here separates same-source images from others on this dataset only.
It says nothing about advertisement playback, and not about the identity of people.

Also verified on-device: backfill bounded (`--max 30` then the remaining 70, then 0 = idempotent), reopen after restart,
`kill -9` during ingest followed by reopen + backfill (8 missing frames filled, no `.tmp` left), time-window + `--min-sim` filtering.

## 6. Host tests

`ctest`: 64 tests (24 Stage 1 + 40 Stage 2) pass against SQLite 3.26.0 amalgamation; ASan/UBSan clean except the upstream sqlite-vec
leak (metadata chunk names, one-off); TSan clean.

## 7. Known limitations

* Exact scan only; O(N·d). No ANN.
* INT8 not enabled (top-5 overlap 0.86, small calibration set).
* Preprocessing is scalar C++ (≈11 ms, larger than the 6 ms inference); fine at 5 s cadence, NEON is a possible follow-up.
* SD-card commit stalls (~1 s p99) affect worker latency, never the capture thread.
* `.rknn` files are not byte-reproducible (hashes recorded for the shipped build).
* FAT32 directory limit / hour partitioning from Stage 1 remains deferred.
* Mobilenet ImageNet weights: check the ImageNet licence terms before commercial redistribution.
