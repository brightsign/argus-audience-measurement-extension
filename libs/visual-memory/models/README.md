# MobileNetV2 embedding model (visual-memory)

Model card (machine readable): `mobilenet_embedder.json`. Validation: `validation_results.md/.json`.

## Source and licence
- `torchvision.models.mobilenet_v2(weights=MobileNet_V2_Weights.IMAGENET1K_V1)` (torchvision code: BSD-3-Clause).
- Weights: https://download.pytorch.org/models/mobilenet_v2-b0353104.pth (trained on ImageNet-1k).
  Check the ImageNet terms of use for your deployment before shipping commercially.
- Embedding = `model.features` -> global average pool (mean over H,W) -> flatten. Output is the 1280-d vector
  before dropout/classifier (dimension verified from the exported ONNX graph: `[1,1280]`).
- model_name `mobilenet_v2_embedder`, model_version `tv-imagenet1k-v1`.

## Files and hashes (SHA-256)
| file | sha256 |
|---|---|
| mobilenet_v2-b0353104.pth (not committed) | b03531047ffacf1e2488318dcd2aba1126cde36e3bfe1aa5cb07700aeeee9889 |
| mobilenet_v2_embedder.onnx (8.9 MB) | ab4ad1b40609324a3bcda0442637416a89f7bb445be2c806c05550dd9dd844b2 |
| mobilenet_embedder_fp16.rknn (5.3 MB) | d5f3e467b0206a73fa57d6790b76fe415dbd87b821222c0f37282e83d7644e36 |
| mobilenet_embedder_int8.rknn (3.3 MB) | b19606d53c7bd10a096de2773087535d6dab23042ba1afea73d638329400ba49 |

The ONNX export is deterministic; `.rknn` builds are NOT byte-reproducible (hash changes on every build), so
treat the recorded `.rknn` hashes as identifying the shipped files only (verify with `scripts/verify_models.sh`).

## Tensors and layout
| | ONNX (reference) | RKNN (NPU) |
|---|---|---|
| input | `input` float32 `[1,3,224,224]` NCHW | `input` `[1,224,224,3]` NHWC, float32 from host |
| channel order | RGB | RGB |
| normalisation | none inside graph (done by pp1) | none inside; `mean_values=[[0,0,0]]`, `std_values=[[1,1,1]]` (identity) |
| output | `embedding` float32 `[1,1280]` | same; FP16 float32 out; INT8 see below |

Opset 12, legacy exporter, static shapes.

## Preprocessing pp1 (exact; reference: `scripts/reference_embedder.py`)
1. Resize the whole image (no crop, aspect NOT preserved) to 224x224 with separable antialiased bilinear (triangle)
   filter in float32, as Pillow's `precompute_coeffs` (BILINEAR, support 1.0) but without 8-bit rounding.
   Per axis: `scale=in/out; filterscale=max(scale,1); support=filterscale`; for output i: `center=(i+0.5)*scale`,
   `xmin=max(int(center-support+0.5),0)`, `xmax=min(int(center+support+0.5),in)`,
   `w_j=max(0,1-|(j-center+0.5)/filterscale|)` for j in [xmin,xmax), normalised to sum 1.
   Horizontal pass first, then vertical, on 0..255 floats.
2. `x=(v/255-mean[c])/std[c]`, RGB, mean=[0.485,0.456,0.406], std=[0.229,0.224,0.225].
3. NHWC float32 `[224,224,3]` to the NPU; NCHW transpose for ONNX.
4. After inference: flatten, L2-normalise (reject NaN/Inf/norm<1e-6). Stored embeddings are unit-length
   float32; similarity = dot product (= cosine).
Numpy resize vs Pillow BILINEAR (8-bit rounded): max diff 0.99 (0..255 units) on the test images.
Diff a C++ tensor against `reference_embedder.py dump-tensor <img> <out.f32>`.

## Quantisation
- `fp16`: `do_quantization=False` (FP16 compute, float32 I/O).
- `int8`: `asymmetric_quantized-8`, algorithm `normal`, method `channel` (toolkit defaults), calibrated on 41
  pp1-preprocessed public images (20 ImageNet-val samples, 20 COCO subset, 1 Argus artwork), one sample each.
  Calibration .npy are stored NCHW `[1,3,224,224]` (the toolkit requires the ONNX layout; same values as the
  NHWC pp1 tensor). 41 images is a small calibration set.
- Toolkit notes that the INT8 model's default input and output dtypes become int8. Per-tensor scale/zero-point
  cannot be read on the host without a device; read them on the device with `rknn_query(RKNN_QUERY_INPUT_ATTR /
  RKNN_QUERY_OUTPUT_ATTR)`, feed float32 NHWC (`pass_through=0`) and request float output (`want_float=1`).
  These attributes are NOT yet recorded in the model card.

## Validation summary (287 images = 41 bases x 6 transforms + bases; details in validation_results.md)
- ORT vs torch: cosine >= 0.99999 (max abs diff ~3e-7). Identical input -> bitwise-identical embedding. Norms = 1.
- Cosine: base vs own transforms mean 0.965 (min 0.806, gaussian blur sigma 2 is hardest); different images
  mean ~0.51, max 0.79. Retrieval top-1 and top-5 same-group hit rate: 1.0 (ORT).
- RKNN simulator vs ORT (cosine mean/min): FP16 1.0000/0.9999, INT8 0.979/0.919; top-5 overlap with ORT: FP16 1.00,
  INT8 0.98; retrieval hit rates remain 1.0.

## Measured on the XT5 (RK3588, librknnrt 2.3.0, 101 images, vs ONNX Runtime FP32)
| variant | cosine mean / min | max abs diff | top-5 overlap | top-1 agreement | NPU inference p50 |
|---|---|---|---|---|---|
| FP16 (shipped, `mobilenet_embedder.rknn`) | 0.999988 / 0.99993 | 0.0015 | 0.998 | 1.00 | 5.9 ms |
| INT8 (evaluated only, NOT enabled) | 0.982 / 0.962 | 0.046 | 0.861 | 0.95 | (wall 15.4 vs 16.8 ms FP16) |

## Licences (read before shipping)
| item | licence | note |
|---|---|---|
| torchvision code / ONNX export scripts | BSD-3-Clause | fine |
| MobileNetV2 `IMAGENET1K_V1` weights | no separate licence file is published; trained on ImageNet-1k | ImageNet is distributed for **non-commercial research/education**; whether weights trained on it may ship in a commercial product is a legal question that has NOT been answered. **Get legal sign-off before commercial release.** |
| sqlite-vec v0.1.6 | MIT OR Apache-2.0 (both texts in `third_party/sqlite-vec/`) | fine |
| Rockchip `librknnrt` / rknn-toolkit2 | Rockchip licence (already part of the Argus package) | unchanged |

## Distribution recommendation
* **Do not commit `.onnx` / `.rknn` files to git** (17 MB now; every re-conversion adds another unreproducible binary
  to history, and the licence question above is open). Commit only the scripts, the model card and the hashes.
* Argus already keeps compiled models out of the repo (`model/` is git-ignored; models live in the shared build
  cache). Do the same: publish the three files to the release artifact store (or Git LFS if one is provisioned),
  pinned by the sha256 above, and let `scripts/sync-models.sh` copy them from `libs/visual-memory/models/` (or
  `$VM_MODELS_DIR`) into the package. `scripts/verify_models.sh` checks the hashes before packaging.
* If legal review rejects the ImageNet weights, replace them by a model with a clean commercial licence (a different
  embedding model = a new `model_version`; stored vectors are never compared across versions). That is out of scope here.
* The INT8 file need not be shipped in production packages.

## sqlite-vec compatibility shim (summary)
The only change needed to compile sqlite-vec v0.1.6 against the SDK `sqlite3.h` (3.26.0) is
`-DSQLITE_INNOCUOUS=0` (flag added in SQLite 3.31; a security hint ignored by older SQLite). CMake detects it with
`check_c_source_compiles`. The vendored sources are unmodified (hashes in `third_party/sqlite-vec/VERSION`).

## Limitations (honest)
- ImageNet classification features are generic perceptual similarity, not tuned for this scene or domain;
  near-duplicate / same-scene matching works well, semantic "same place, different content" does not.
  Thresholds must be calibrated on real data, which was not available here.
- Resize ignores aspect ratio (squashes) by design; strong aspect changes lower similarity.
- INT8 deviates noticeably from FP32 (cosine 0.92-0.99); thresholds tuned on FP16/FP32 should be re-checked for INT8.
  INT8 evaluation is partly in-sample (the calibration images are the evaluation images).
- Validation used the rknn-toolkit2 x86 *simulator*, not the NPU; hardware results are not yet measured.
- The test set is small (41 public images) and transformations are synthetic.

## Regenerate (dev host; venv with torch 2.4.0, torchvision 0.19.0, onnx 1.16.1, onnxruntime, numpy 1.26.4, rknn-toolkit2 2.3.0)
```
python scripts/export_embedding_model.py          # downloads weights, writes ONNX + card
python scripts/convert_rknn.py --variant fp16
python scripts/convert_rknn.py --variant int8
python scripts/validate_embedding_model.py        # writes validation_results.{json,md}
python scripts/reference_embedder.py selfcheck
python scripts/make_demo_dataset.py <outdir>      # 100-frame demo dataset + reference.jpg
```
Note: rknn-toolkit2 2.3.0 fails to load ONNX with onnx >= 1.17 (`module 'onnx' has no attribute 'mapping'`); use onnx 1.16.1.
Calibration/work files go to a scratch dir (`WORK` in convert_rknn.py); edit it for your host.
