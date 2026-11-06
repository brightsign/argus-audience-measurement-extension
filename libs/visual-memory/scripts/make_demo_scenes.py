#!/usr/bin/env python3
"""Build the people-free Visual Memory demo dataset (scenes + objects) and a slideshow video.

  make_demo_scenes.py <outdir> [--slide-seconds 5]

Output (all generated locally, nothing is committed; the images are not redistributed by this repository):
  scenes/        one JPEG per slide (base scene + one variant of each: flip / brightness / crop), shuffled
  references/    one query JPEG per scene (an off-centre crop with a brightness change: never byte-identical to a slide)
  scenes.mp4     the slides as a 640x360 H.264 video, `--slide-seconds` per slide. Argus plays this through its normal
                 file-input pipeline, so the store is filled by the real Visual Memory writer + Stage-2 embedder.
  manifest.json  slide order, base-scene id, transform, and the base-scene id of every reference (ground truth)

Sources (no network): RKNN model-zoo ImageNet-val samples (a hand-picked, people-free subset; override the folder with
VM_DATASETS_DIR) and scikit-image's bundled public-domain sample images. Any image with an identifiable person is excluded
by construction; see KEEP below. Requires Pillow, scikit-image and ffmpeg.
"""
import argparse, glob, hashlib, json, os, random, subprocess, sys
from PIL import Image, ImageEnhance

HERE = os.path.dirname(os.path.abspath(__file__))
ZOO = os.environ.get("VM_DATASETS_DIR", os.path.join(HERE, "..", "..", "..", "..", "argus-build-cache", "toolkit",
                                                      "rknn_model_zoo", "datasets"))
SEED = 20261104
W, H = 640, 360           # video / stored-frame size (16:9, matches visual_memory.stored_width/height)

# Indices into sorted(ImageNet-val samples) + sorted(COCO subset), reviewed by eye: objects, animals, scenery, food,
# desk items. Excluded: everything with a person, a crowd, or a person's body/arm.
KEEP = [0, 1, 2, 4, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 22, 24, 26, 29, 31, 33, 35, 37, 38]
SKIMAGE = ["coffee", "chelsea", "rocket", "hubble_deep_field", "moon", "brick", "grass", "gravel"]


def cover(im, w=W, h=H):
    """Scale to cover w x h, centre-crop (no letterbox bars)."""
    s = max(w / im.width, h / im.height)
    im = im.resize((max(w, round(im.width * s)), max(h, round(im.height * s))), Image.LANCZOS)
    l, t = (im.width - w) // 2, (im.height - h) // 2
    return im.crop((l, t, l + w, t + h))


def crop_frac(im, f, dx=0.0, dy=0.0):
    w, h = im.size
    cw, ch = int(w * f), int(h * f)
    l = int((w - cw) / 2 + dx * (w - cw) / 2); t = int((h - ch) / 2 + dy * (h - ch) / 2)
    return im.crop((l, t, l + cw, t + ch))


VARIANTS = {
    "flip": lambda im: im.transpose(Image.FLIP_LEFT_RIGHT),
    "bright+15": lambda im: ImageEnhance.Brightness(im).enhance(1.15),
    "bright-15": lambda im: ImageEnhance.Brightness(im).enhance(0.85),
    "crop90": lambda im: cover(crop_frac(im, 0.9)),
}


def load_bases():
    import numpy as np, skimage.data as sd
    files = [f for f in sorted(glob.glob(ZOO + "/imagenet/ILSVRC2012_img_val_samples/*")) +
             sorted(glob.glob(ZOO + "/COCO/subset/*")) if f.lower().endswith((".jpeg", ".jpg", ".png"))]
    if len(files) < max(KEEP) + 1:
        sys.exit(f"model-zoo sample images not found under {ZOO} (set VM_DATASETS_DIR)")
    bases = [(os.path.basename(files[i]), Image.open(files[i]).convert("RGB")) for i in KEEP]
    for n in SKIMAGE:
        a = getattr(sd, n)()
        if a.ndim == 2:
            a = np.stack([a] * 3, -1)
        bases.append(("skimage:" + n, Image.fromarray(a[..., :3].astype("uint8"))))
    return bases


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("out"); ap.add_argument("--slide-seconds", type=float, default=5.0)
    a = ap.parse_args()
    rng = random.Random(SEED)
    for d in ("scenes", "references"):
        os.makedirs(os.path.join(a.out, d), exist_ok=True)
    bases = load_bases()
    slides = []
    for bid, (name, im) in enumerate(bases):
        base = cover(im)
        slides.append((bid, name, "original", base))
        t = rng.choice(sorted(VARIANTS))
        slides.append((bid, name, t, VARIANTS[t](base)))
    # shuffle so a scene and its variant are never adjacent (adjacent slides would trivially look "similar in time")
    for _ in range(100):
        rng.shuffle(slides)
        if all(slides[i][0] != slides[i + 1][0] for i in range(len(slides) - 1)):
            break
    man = {"seed": SEED, "slide_seconds": a.slide_seconds, "slides": [], "references": {}}
    for k, (bid, name, t, im) in enumerate(slides, 1):
        fn = f"scene_{k:03d}.jpg"
        im.save(os.path.join(a.out, "scenes", fn), "JPEG", quality=90)
        man["slides"].append({"file": fn, "base_id": bid, "base": name, "transform": t})
    sums = {hashlib.sha256(open(f, "rb").read()).hexdigest() for f in glob.glob(a.out + "/scenes/*.jpg")}
    for bid, (name, im) in enumerate(bases):
        r = ImageEnhance.Brightness(crop_frac(cover(im), 0.82, dx=0.4, dy=-0.4)).enhance(1.1)
        fn = f"ref_{bid:02d}.jpg"
        p = os.path.join(a.out, "references", fn)
        r.save(p, "JPEG", quality=88)
        assert hashlib.sha256(open(p, "rb").read()).hexdigest() not in sums
        man["references"][fn] = {"base_id": bid, "base": name}
    json.dump(man, open(os.path.join(a.out, "manifest.json"), "w"), indent=1)
    # slideshow: one still per slide, constant 30 fps, H.264 baseline (widely decodable)
    lst = os.path.join(a.out, "slides.txt")
    with open(lst, "w") as f:
        for s in man["slides"]:
            f.write(f"file 'scenes/{s['file']}'\nduration {a.slide_seconds}\n")
        f.write(f"file 'scenes/{man['slides'][-1]['file']}'\n")
    subprocess.check_call(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", lst, "-vf",
                           f"fps=30,scale={W}:{H},format=yuv420p", "-c:v", "libx264", "-profile:v", "baseline",
                           "-crf", "20", "-movflags", "+faststart", os.path.join(a.out, "scenes.mp4")])
    print(f"{len(slides)} slides ({len(bases)} scenes x 2), {len(man['references'])} references, "
          f"{len(slides) * a.slide_seconds:.0f} s video -> {a.out}")


if __name__ == "__main__":
    main()
