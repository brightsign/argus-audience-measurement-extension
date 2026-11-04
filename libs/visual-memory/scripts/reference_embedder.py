#!/usr/bin/env python3
"""Numpy reference implementation of preprocessing "pp1" + ONNX Runtime embedding.

pp1: whole image -> 224x224 (aspect not preserved), separable antialiased bilinear (triangle) resize in float32,
     computed like Pillow's precompute_coeffs but with NO 8-bit rounding; then (v/255-mean)/std, RGB, NHWC.

CLI:
  reference_embedder.py embed <img>
  reference_embedder.py dump-tensor <img> <out.f32>     # raw NHWC float32 little-endian [224,224,3]
  reference_embedder.py selfcheck [imgs...]            # numpy resize vs PIL BILINEAR
"""
import glob, os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
MODELS = os.path.normpath(os.path.join(HERE, "..", "models"))
ONNX_PATH = os.path.join(MODELS, "mobilenet_v2_embedder.onnx")
SIZE = 224
MEAN = np.array([0.485, 0.456, 0.406], np.float32)
STD = np.array([0.229, 0.224, 0.225], np.float32)


def _coeffs(in_size, out_size):
    """Return list of (xmin, weights[float32]) per output index (Pillow BILINEAR, support 1.0)."""
    scale = in_size / out_size
    filterscale = max(scale, 1.0)
    support = 1.0 * filterscale
    res = []
    for i in range(out_size):
        center = (i + 0.5) * scale
        xmin = max(int(center - support + 0.5), 0)
        xmax = min(int(center + support + 0.5), in_size)
        x = (np.arange(xmin, xmax, dtype=np.float64) - center + 0.5) / filterscale
        w = np.maximum(0.0, 1.0 - np.abs(x))
        s = w.sum()
        if s != 0:
            w = w / s
        res.append((xmin, w.astype(np.float32)))
    return res


def _matrix(in_size, out_size):
    m = np.zeros((out_size, in_size), np.float32)
    for i, (x0, w) in enumerate(_coeffs(in_size, out_size)):
        m[i, x0:x0 + len(w)] = w
    return m


def resize_pp1(img_u8):
    """img_u8: HxWx3 uint8 RGB -> 224x224x3 float32 in 0..255 (no rounding)."""
    h, w = img_u8.shape[:2]
    a = img_u8.astype(np.float32)
    # horizontal pass first, then vertical (same order as Pillow)
    tmp = np.einsum("ow,hwc->hoc", _matrix(w, SIZE), a, optimize=True)
    out = np.einsum("oh,hwc->owc", _matrix(h, SIZE), tmp, optimize=True)
    return out.astype(np.float32)


def preprocess_array(img_u8):
    if not isinstance(img_u8, np.ndarray) or img_u8.dtype != np.uint8:
        raise TypeError("image must be a uint8 numpy array")
    if img_u8.ndim != 3 or img_u8.shape[2] != 3:
        raise ValueError("image must be HxWx3 RGB")
    if img_u8.shape[0] < 1 or img_u8.shape[1] < 1:
        raise ValueError("empty image")
    r = resize_pp1(img_u8)
    return ((r / 255.0 - MEAN) / STD).astype(np.float32)  # NHWC [224,224,3]


def load_rgb(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.uint8)


def preprocess_file(path):
    return preprocess_array(load_rgb(path))


_sess = None


def _session():
    global _sess
    if _sess is None:
        import onnxruntime as ort
        _sess = ort.InferenceSession(ONNX_PATH, providers=["CPUExecutionProvider"])
    return _sess


def l2_normalise(v):
    v = np.asarray(v, np.float32).reshape(-1)
    if not np.all(np.isfinite(v)):
        raise ValueError("embedding contains NaN/Inf")
    n = float(np.linalg.norm(v))
    if n < 1e-6:
        raise ValueError("embedding norm too small")
    return (v / n).astype(np.float32)


def embed_tensor(nhwc):
    x = np.ascontiguousarray(np.transpose(nhwc, (2, 0, 1))[None], dtype=np.float32)
    return _session().run(None, {"input": x})[0]


def embed_array(img_u8):
    return l2_normalise(embed_tensor(preprocess_array(img_u8)))


def embed_file(path):
    return embed_array(load_rgb(path))


def selfcheck(paths):
    worst = 0.0
    for p in paths:
        im = Image.open(p).convert("RGB")
        ref = np.asarray(im.resize((SIZE, SIZE), Image.BILINEAR), np.float32)
        mine = resize_pp1(np.asarray(im, np.uint8))
        d = float(np.abs(ref - mine).max())
        worst = max(worst, d)
        print(f"{os.path.basename(p)} {im.size} max|numpy-PIL| = {d:.3f}")
    print(f"selfcheck worst max diff (0..255 units): {worst:.3f}")
    return worst


def default_images():
    base = os.environ.get("VM_DATASETS_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..", "argus-build-cache", "toolkit", "rknn_model_zoo", "datasets"))
    return sorted(glob.glob(base + "/imagenet/ILSVRC2012_img_val_samples/*.JPEG"))[:6] + \
        sorted(glob.glob(base + "/COCO/subset/*"))[:4]


def main(argv):
    if len(argv) >= 3 and argv[1] == "embed":
        e = embed_file(argv[2])
        print(f"dim={e.shape[0]} norm={np.linalg.norm(e):.6f}")
    elif len(argv) == 4 and argv[1] == "dump-tensor":
        t = preprocess_file(argv[2])
        t.astype("<f4").tofile(argv[3])
        print(f"wrote {argv[3]} shape={t.shape} (NHWC float32 LE)")
    elif len(argv) >= 2 and argv[1] == "selfcheck":
        selfcheck(argv[2:] or default_images())
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
