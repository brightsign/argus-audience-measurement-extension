# Visual Memory — stakeholder demo (D1–D3), repeatable procedure

A ~10-minute walk-through of the Argus dashboard's **Visual Memory** and **Visual Search** sections using a **people-free
dataset** (objects, animals, scenery, food, desk items). Nothing in the repository contains camera images of people; the
dataset is generated locally and the screenshots are kept out of Git.

What it shows: a rolling store of JPEG frames with image embeddings (MobileNetV2 FP16 on the NPU), a gallery with
time/source filters and paging, and *image/scene similarity* search (reference JPEG → top-K stored frames, side-by-side
comparison, similarity score). It is **not** object detection and **not** identity matching, and the UI says so.

## 0. Prerequisites
* An XT5 running the Argus extension package that contains `vm_api_server`, the dashboard and the FP16 model
  (`argus-ext-1793816098.zip` or later), SSH/telnet access, and the default Argus config (Visual Memory **off**).
* Host: Python 3 with Pillow, NumPy, scikit-image and `ffmpeg`; the RKNN model-zoo sample images
  (`argus-build-cache/toolkit/rknn_model_zoo/datasets`, or `VM_DATASETS_DIR=…`).
* The cross-compiled `vm_ingest_dir` tool: `make build SOCS=rk3588 EXTRA_CMAKE_FLAGS="-DENABLE_VISUAL_MEMORY=ON -DENABLE_VISUAL_EMBEDDINGS=ON"`
  then `make vm_ingest_dir` in the build directory. It is a developer tool and is **not** packaged.

## 1. Prepare the dataset (host, once)
```
python3 libs/visual-memory/scripts/make_demo_scenes.py /tmp/vm-demo
#  -> 64 slides (32 scenes x [original + one variant: flip / brightness / crop]), shuffled so a scene and its variant are
#     never adjacent; 32 reference JPEGs (off-centre crop + brightness, never byte-identical to a stored frame);
#     manifest.json (ground truth); scenes.mp4 (not needed by the steps below)
```
The selection was reviewed by eye: every image with a person, a crowd or a body part is excluded (see `KEEP` in the script).

## 2. Load the dataset into an isolated store (player, once)
The store is written by the **real** Visual Memory writer and the **real** NPU embedder, with capture times spread over the
last 3 hours so the date filters have something to filter. Argus itself is not stopped and its config is not changed.
```
scp build_xt5/vm_ingest_dir  brightsign@<player>:/storage/sd/vm-demo-input/
scp -r /tmp/vm-demo/scenes   brightsign@<player>:/storage/sd/vm-demo-input/
scp scripts/vm_demo_viewer.sh brightsign@<player>:/storage/sd/vm-demo-input/
# in a root shell on the player (the SD card is noexec, so run the tool from /tmp):
cp /storage/sd/vm-demo-input/vm_ingest_dir /tmp/ && chmod +x /tmp/vm_ingest_dir
EXT=/var/volatile/bsext/ext_npu_argus
cd /storage/sd/vm-demo-input/scenes
LD_LIBRARY_PATH=$EXT/RK3588/lib:$EXT/lib /tmp/vm_ingest_dir --root /storage/sd/vm-demo-dataset \
   --model $EXT/RK3588/model/visual_memory/mobilenet_embedder.rknn --npu-core 1 --span-minutes 180 --source-id demo-scenes $(ls scene_*.jpg)
#  ingested 64/64 frames ... encoded=64 dropped=0 ... embedded=64 failed=0
```
The tool refuses to write into an existing store unless `--append` is given.

## 3. Start the dataset viewer (player)
```
sh /storage/sd/vm-demo-input/vm_demo_viewer.sh start      # dashboard :20399, API :20311, store /storage/sd/vm-demo-dataset
```
This runs a **second** dashboard + `vm_api_server --viewer` pair on spare ports that read only the dataset store. Port 20300,
the active Argus config and the live pipeline are untouched; the Audience tab of the viewer still shows the live camera. The
viewer has no access token and exposes only the dataset: stop it after the demo (`… stop`). Note that the *Audience
Measurement* tab shows the live camera, which may include people — hide that tab or crop it when presenting externally.

## 4. Walk-through (browse `http://<player>:20399/`)
| # | Show | Expected |
|---|---|---|
| 1 | Pill top-right | **VM: dataset viewer** (green). Explain: read-only view of a stored dataset, no live capture. |
| 2 | *Visual Memory* tiles | 64 frames, 64 with image, 64 embeddings (0 failed), storage ≈ 5.4 MB of a 4 GB quota, model `mobilenet_v2_embedder tv-imagenet1k-v1 · pp1 · dim 1280`, retention 48 h / 7 d, oldest/newest frame 3 h apart. |
| 3 | *Sampling & processing* | Interval **5 s**, stored 640×360 / JPEG q90, **capture FPS ≈ 29** (live from Argus), model. Live counters show “—” in viewer mode (see §7 for the live variant). |
| 4 | Gallery | 24 per page, 3 pages, thumbnails, newest first. *Last hour* → ≈ 20 frames; *Local/UTC* toggle; hover a time for UTC + local; source filter (`demo-scenes`); order asc/desc. Click a card → detail (size, SHA-256, embedding row, retention). |
| 5 | *Visual Search* → drop `references/ref_24.jpg` (coffee cup, off-centre crop, brighter) → **Search** | 5 results in ≈ 20–40 ms. #1 **0.96** (the coffee scene), #2 **0.957** (the same scene flipped/brightened), then a clear gap to ≈ 0.59 for unrelated scenes. |
| 6 | Click result #1 | Side-by-side: reference (local preview, never stored) vs match — frame id, capture time (local + UTC), source, size, similarity, distance. “Find similar to this match” chains a search; “Open frame detail” opens the record. |
| 7 | Search again with `ref_26.jpg` (rocket), then **Pick a stored frame…** → any chimp/zebra frame | Rocket → rocket scene #1/#2; stored-frame search uses the *stored* vector (no inference, ≈ 5 ms) and excludes the query frame. |
| 8 | Optional: *Minimum similarity* 0.80, *Require JPEG still available*, time window | Results narrow accordingly; empty result shows a clear message rather than an error. |
| 9 | Optional: upload a text file / truncated JPEG | Readable error message; the server never exposes filesystem paths. |

Ground truth (§6) shows this is not cherry-picked: all 32 references return the right scene at rank 1.

## 5. Live variant (optional; shows real runtime counters)
Uses the player's camera, so **frames contain people** — keep screenshots private.
```
cp configs/demo-visual-memory.json  →  /storage/sd/configs/argus-config.json   (back up the original first!)
/var/volatile/bsext/ext_npu_argus/bsext_init restart
```
`demo-visual-memory.json`: Visual Memory Stage 1+2 on (store `/storage/sd/vm-demo`, 5 s sampling, FP16 MobileNetV2),
`input.usb.exposure_auto_priority: 0` (constant frame rate in dim light). Open `http://<player>:20300/` → *Visual Memory*:
the pill is **VM: active**, and *Sampling & processing* shows frames sampled / accepted / embedded / dropped / failed
growing every 5 s, capture FPS ≈ 29–30 and the model. Restore the original config afterwards and confirm its checksum.

## 6. Verify the dataset search offline (optional)
`manifest.json` maps every slide to its source scene, so the claim can be checked against ground truth:
frame ids follow slide order; POST each `references/ref_NN.jpg` to `/api/vm/search/image?top_k=5` (send an `Origin` header equal
to the viewer's host) and compare scene ids. Measured on the XT5 (64 frames, 32 references): **top-1 correct 32/32,
correct scene in top-5 32/32, precision@2 (scene + its variant) 100 %, MRR 1.000**; correct top-1 similarity min 0.870 /
median 0.943; server-side search median ≈ 31 ms (decode + preprocess + NPU + scan), max 43 ms.

## 7. Teardown
```
sh /storage/sd/vm-demo-input/vm_demo_viewer.sh stop
# live variant only: restore the original config, restart Argus, confirm sha256sum matches the backup
```
Delete `/storage/sd/vm-demo-dataset` and `/storage/sd/vm-demo-input` when the demo is no longer needed (they are test data;
nothing in the extension depends on them).

## Troubleshooting
* **Camera shows ≈ 19 fps in dim light** — UVC `exposure_auto_priority=1` (the camera's state after every power-up) lets the
  camera lower its frame rate. Set `input.usb.exposure_auto_priority` to `0` (see `docs/CONFIGURATION.md`).
* **Pill “VM: locked”** — `dashboard.vm_token` is set in the active config: enter the token in the banner.
* **`vm_ingest_dir: error while loading shared libraries`** — set `LD_LIBRARY_PATH` as shown in §2.
* **No live video on the viewer** — the viewer copies the live dashboard's `-upstream`; start Argus first.
