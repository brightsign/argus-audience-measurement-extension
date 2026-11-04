#!/usr/bin/env python3
"""ONNX -> RKNN (rk3588) conversion with rknn-toolkit2 (dev host, x86).

  convert_rknn.py --variant fp16   -> models/mobilenet_embedder_fp16.rknn
  convert_rknn.py --variant int8   -> models/mobilenet_embedder_int8.rknn

Normalisation is NOT inside the model (pp1 does (v/255-mean)/std on the host), so we configure
mean_values=[[0,0,0]], std_values=[[1,1,1]] explicitly = identity; the NPU gets pre-normalised float32 NHWC.

INT8 calibration set: pp1-preprocessed .npy files (NHWC float32 [1,224,224,3]) of the public images
  rknn_model_zoo/datasets/imagenet/ILSVRC2012_img_val_samples/*.JPEG (21)
  rknn_model_zoo/datasets/COCO/subset/* (20)
  images/the-hundred-eyes-of-Argus.jpg (1)            => 42 samples (+ listed in a dataset txt)
Quantisation: asymmetric_quantized-8, default (normal) algorithm, per-tensor... (toolkit default for rk3588).
"""
import argparse, glob, hashlib, json, os, random, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import reference_embedder as ref  # noqa: E402

MODELS = ref.MODELS
CARD = os.path.join(MODELS, "mobilenet_embedder.json")
ZOO = os.environ.get("VM_DATASETS_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..", "argus-build-cache", "toolkit", "rknn_model_zoo", "datasets"))  # RKNN model-zoo datasets (ImageNet-val + COCO samples); override with VM_DATASETS_DIR
ARGUS = os.path.normpath(os.path.join(HERE, "..", "..", "..", "images", "the-hundred-eyes-of-Argus.jpg"))
WORK = os.environ.get("VM_WORK_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build-models"))


def calib_images():
    return (sorted(glob.glob(ZOO + "/imagenet/ILSVRC2012_img_val_samples/*.JPEG")) +
            sorted(glob.glob(ZOO + "/COCO/subset/*")) + [ARGUS])


def build_calibration(outdir):
    os.makedirs(outdir, exist_ok=True)
    lines = []
    for i, p in enumerate(calib_images()):
        # pp1 output is NHWC; the toolkit (ONNX graph is NCHW) wants calibration .npy as NCHW [1,3,224,224] -> same values, transposed
        t = np.transpose(ref.preprocess_file(p), (2, 0, 1))[None]
        f = os.path.join(outdir, f"calib_{i:03d}.npy")
        np.save(f, t.astype(np.float32))
        lines.append(f)
    txt = os.path.join(outdir, "dataset.txt")
    open(txt, "w").write("\n".join(lines) + "\n")
    return txt, len(lines)


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", choices=["fp16", "int8"], required=True)
    ap.add_argument("--onnx", default=ref.ONNX_PATH)
    ap.add_argument("--workdir", default=WORK)
    a = ap.parse_args()
    random.seed(0); np.random.seed(0)
    from rknn.api import RKNN
    import importlib.metadata as md
    ver = md.version("rknn-toolkit2")
    print("rknn-toolkit2", ver, "numpy", np.__version__)
    out = os.path.join(MODELS, f"mobilenet_embedder_{a.variant}.rknn")
    quant = a.variant == "int8"

    rknn = RKNN(verbose=True, verbose_file=os.path.join(a.workdir, f"rknn_build_{a.variant}.log"))
    os.makedirs(a.workdir, exist_ok=True)
    kw = dict(mean_values=[[0, 0, 0]], std_values=[[1, 1, 1]],  # identity: normalisation is done by pp1 on host
              target_platform="rk3588", optimization_level=3)
    if quant:
        kw.update(quantized_dtype="asymmetric_quantized-8", quantized_algorithm="normal", quantized_method="channel")
    assert rknn.config(**kw) == 0
    assert rknn.load_onnx(model=a.onnx, inputs=["input"], input_size_list=[[1, 3, 224, 224]]) == 0
    ds, n = None, 0
    if quant:
        ds, n = build_calibration(os.path.join(a.workdir, "calib"))
        print("calibration samples:", n, ds)
    assert rknn.build(do_quantization=quant, dataset=ds) == 0, "build failed"
    assert rknn.export_rknn(out) == 0
    print("wrote", out, os.path.getsize(out), "bytes")
    rknn.release()
    h = sha256(out)
    print("sha256", h)

    # Tensor attributes (scale/zero-point) are not exposed by the toolkit without a device; the toolkit
    # only reports dtype changes at build time. Authoritative values come from rknn_query(INPUT/OUTPUT_ATTR)
    # on the device. Recorded here: what the toolkit stated.
    attrs = {}
    card = json.load(open(CARD)) if os.path.exists(CARD) else {}
    card.setdefault("rknn_sha256", {})[a.variant] = h
    v = card.setdefault("variants", {})
    v[a.variant] = {
        "file": os.path.basename(out), "target_platform": "rk3588", "toolkit": "rknn-toolkit2 " + ver,
        "runtime_expected": "librknnrt 2.3.0",
        "config": {k: val for k, val in kw.items()},
        "do_quantization": quant,
        "quantisation": ({"dtype": "asymmetric_quantized-8", "algorithm": "normal", "method": "channel",
                          "calibration": {"count": n, "preprocessing": "pp1, same values as the NHWC tensor, stored NCHW float32 [1,3,224,224] .npy (rknn-toolkit2 requires the ONNX layout)",
                                          "sources": ["imagenet ILSVRC2012_img_val_samples (21)", "COCO subset (20)",
                                                      "images/the-hundred-eyes-of-Argus.jpg (1)"]}}
                         if quant else {"dtype": "float16 (no quantisation)"}),
        "io": {"input": {"name": "input", "shape_nhwc": [1, 224, 224, 3], "layout": "NHWC", "channel_order": "RGB",
                         "host_dtype": "float32", "note": "pre-normalised by pp1; identity mean/std in RKNN"},
               "output": {"name": "embedding", "shape": [1, 1280], "host_dtype": "float32"}},
        "tensor_attr_note": ("INT8: toolkit warns default input AND output dtype in the .rknn are int8 (asymmetric affine, "
                             "per-tensor scale/zp); query rknn_query(RKNN_QUERY_INPUT_ATTR/OUTPUT_ATTR) for scale/zp. "
                             "Feed float32 NHWC with rknn_input.pass_through=0 and use want_float=1 on output to dequantise. "
                             "FP16: float32 in/out, fp16 compute." if quant else
                             "FP16: float32 NHWC in / float32 out; queried attrs on device give fmt/type."),
        **attrs,
    }
    json.dump(card, open(CARD, "w"), indent=2)
    open(CARD, "a").write("\n")


if __name__ == "__main__":
    main()
