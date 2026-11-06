#!/usr/bin/env python3
"""Build a reproducible demo dataset from public images (no network).

  make_demo_dataset.py <outdir>

40 base images (ImageNet-val samples + COCO subset) + 60 variants (20 bases get 2 variants, 20 get 1)
= 100 JPEGs (quality 85, width <= 640, original aspect), named frame_001.jpg.., shuffled with a fixed seed so
related frames are not adjacent, with ascending synthetic timestamps (5 s apart from 2026-01-01T10:00:00Z).
Also writes reference.jpg: 85% crop + brightness of one base image (not byte-identical to any dataset file).
"""
import datetime as dt, glob, hashlib, json, os, random, sys
from PIL import Image, ImageEnhance

ZOO = os.environ.get("VM_DATASETS_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..", "argus-build-cache", "toolkit", "rknn_model_zoo", "datasets"))  # RKNN model-zoo datasets (ImageNet-val + COCO samples); override with VM_DATASETS_DIR
SEED = 20260101
Q = 85


def fit(im, maxw=640):
    if im.width > maxw:
        im = im.resize((maxw, round(im.height * maxw / im.width)), Image.LANCZOS)
    return im


def crop_frac(im, f, dx=0.0, dy=0.0):
    w, h = im.size
    cw, ch = int(w * f), int(h * f)
    l = int((w - cw) / 2 + dx * (w - cw) / 2); t = int((h - ch) / 2 + dy * (h - ch) / 2)
    return im.crop((l, t, l + cw, t + ch))


TRANSFORMS = {
    "flip": lambda im: im.transpose(Image.FLIP_LEFT_RIGHT),
    "bright+15": lambda im: ImageEnhance.Brightness(im).enhance(1.15),
    "bright-15": lambda im: ImageEnhance.Brightness(im).enhance(0.85),
    "crop90": lambda im: crop_frac(im, 0.9),
}


def main(out):
    os.makedirs(out, exist_ok=True)
    rng = random.Random(SEED)
    bases = (sorted(glob.glob(ZOO + "/imagenet/ILSVRC2012_img_val_samples/*.JPEG")) +
             sorted(glob.glob(ZOO + "/COCO/subset/*")))[:40]
    assert len(bases) == 40, len(bases)
    items = []  # (base_path, transform_name or "none", PIL image)
    tnames = sorted(TRANSFORMS)
    for i, p in enumerate(bases):
        im = fit(Image.open(p).convert("RGB"))
        items.append((p, "none", im))
        n = 2 if i < 20 else 1
        for t in rng.sample(tnames, n):
            items.append((p, t, TRANSFORMS[t](im)))
    assert len(items) == 100, len(items)
    rng.shuffle(items)
    t0 = dt.datetime(2026, 1, 1, 10, 0, 0, tzinfo=dt.timezone.utc)
    manifest = {"seed": SEED, "jpeg_quality": Q, "frames": {}}
    for k, (p, t, im) in enumerate(items, 1):
        name = f"frame_{k:03d}.jpg"
        im.save(os.path.join(out, name), "JPEG", quality=Q)
        ts = t0 + dt.timedelta(seconds=5 * (k - 1))
        manifest["frames"][name] = {"base_image": os.path.basename(p), "transform": t,
                                    "synthetic_timestamp_ms": int(ts.timestamp() * 1000),
                                    "synthetic_timestamp_iso": ts.strftime("%Y-%m-%dT%H:%M:%SZ")}
    # reference: 85% off-centre crop + brightness of a base that is in the dataset
    refbase = bases[7]
    im = fit(Image.open(refbase).convert("RGB"))
    r = ImageEnhance.Brightness(crop_frac(im, 0.85, dx=0.5, dy=-0.5)).enhance(1.1)
    r.save(os.path.join(out, "reference.jpg"), "JPEG", quality=Q)
    manifest["reference"] = {"file": "reference.jpg", "base_image": os.path.basename(refbase),
                             "transform": "crop85(off-centre)+bright+10"}
    sums = {hashlib.sha256(open(f, "rb").read()).hexdigest() for f in glob.glob(out + "/frame_*.jpg")}
    assert hashlib.sha256(open(os.path.join(out, "reference.jpg"), "rb").read()).hexdigest() not in sums
    json.dump(manifest, open(os.path.join(out, "manifest.json"), "w"), indent=2)
    print("wrote", len(items), "frames + reference.jpg + manifest.json to", out)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
