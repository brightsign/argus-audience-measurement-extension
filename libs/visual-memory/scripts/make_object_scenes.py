#!/usr/bin/env python3
"""Controlled, people-free object dataset for D4 (Object Search) validation.

  make_object_scenes.py <outdir> [--frames 240] [--seed N]

Renders procedurally drawn everyday objects (cups, bottles, boxes, cans; several distinct instances of each) onto varied
backgrounds with different lighting, colour casts, scales, rotations and positions. Ground truth (class, instance id, exact
bounding box in the 640x360 frame) is written to manifest.json, so ROI accuracy and retrieval metrics are measurable.

This is a SYNTHETIC, CONTROLLED set: it checks the machinery (ROI -> crop -> embedding -> search) and gives relative numbers.
It is not evidence of general object recognition on real scenes. Requires Pillow and NumPy only.
"""
import argparse, json, math, os, random
import numpy as np
from PIL import Image, ImageDraw, ImageEnhance, ImageFilter

W, H = 640, 360
SS = 3  # supersampling for the object sprites


def shade(c, f):
    return tuple(max(0, min(255, int(v * f))) for v in c)


def sprite_cup(p, S):
    w, h = int(150 * S), int(150 * S)
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    bw, bh = int(w * 0.62), int(h * 0.74); x0, y0 = int(w * 0.10), int(h * 0.20)
    c = p["color"]
    d.ellipse([x0, y0 + bh - bw * 0.16, x0 + bw, y0 + bh + bw * 0.16], fill=shade(c, 0.7))
    d.rectangle([x0, y0 + bw * 0.08, x0 + bw, y0 + bh], fill=c)
    for i in range(0, bw, 3):                      # cylindrical shading
        f = 0.75 + 0.35 * math.sin(math.pi * i / bw)
        d.line([x0 + i, y0 + bw * 0.08, x0 + i, y0 + bh], fill=shade(c, f), width=3)
    d.ellipse([x0, y0 - bw * 0.08, x0 + bw, y0 + bw * 0.24], fill=shade(c, 1.15))
    d.ellipse([x0 + bw * 0.06, y0 - bw * 0.04, x0 + bw * 0.94, y0 + bw * 0.20], fill=p["inner"])
    hx = x0 + bw - 2
    d.arc([hx - bw * 0.1, y0 + bh * 0.18, hx + bw * 0.42, y0 + bh * 0.72], -90, 90, fill=shade(c, 0.8), width=int(w * 0.07))
    if p["pattern"] == "stripe":
        for k in range(2): d.rectangle([x0, y0 + bh * (0.35 + 0.18 * k), x0 + bw, y0 + bh * (0.41 + 0.18 * k)], fill=p["accent"])
    elif p["pattern"] == "dots":
        for k in range(8): d.ellipse([x0 + bw * (0.15 + 0.1 * (k % 4) * 2), y0 + bh * (0.35 + 0.25 * (k // 4)), x0 + bw * (0.25 + 0.1 * (k % 4) * 2), y0 + bh * (0.45 + 0.25 * (k // 4))], fill=p["accent"])
    return im


def sprite_bottle(p, S):
    w, h = int(80 * S), int(190 * S)
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    c = p["color"]; bx0, bx1 = int(w * 0.12), int(w * 0.88)
    d.rounded_rectangle([bx0, int(h * 0.30), bx1, int(h * 0.98)], radius=int(w * 0.18), fill=c)
    d.polygon([(bx0 + w * 0.08, h * 0.30), (bx1 - w * 0.08, h * 0.30), (w * 0.62, h * 0.12), (w * 0.38, h * 0.12)], fill=shade(c, 0.9))
    d.rectangle([w * 0.38, h * 0.04, w * 0.62, h * 0.13], fill=shade(c, 0.9))
    d.rectangle([w * 0.36, 0, w * 0.64, h * 0.07], fill=p["accent"])
    for i in range(bx0, bx1, 2):
        f = 0.7 + 0.5 * math.sin(math.pi * (i - bx0) / (bx1 - bx0)); d.line([i, h * 0.32, i, h * 0.96], fill=shade(c, f), width=2)
    d.rectangle([bx0, h * 0.50, bx1, h * 0.78], fill=p["label"])
    d.rectangle([bx0, h * 0.58, bx1, h * 0.62], fill=p["accent"])
    return im


def sprite_box(p, S):
    w, h = int(170 * S), int(150 * S)
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    c = p["color"]; fx0, fy0, fx1, fy1 = int(w * 0.04), int(h * 0.28), int(w * 0.72), int(h * 0.96)
    dx, dy = int(w * 0.24), int(h * 0.24)
    d.polygon([(fx0, fy0), (fx0 + dx, fy0 - dy), (fx1 + dx, fy0 - dy), (fx1, fy0)], fill=shade(c, 1.2))
    d.polygon([(fx1, fy0), (fx1 + dx, fy0 - dy), (fx1 + dx, fy1 - dy), (fx1, fy1)], fill=shade(c, 0.65))
    d.rectangle([fx0, fy0, fx1, fy1], fill=c)
    d.rectangle([fx0 + (fx1 - fx0) * 0.12, fy0 + (fy1 - fy0) * 0.18, fx0 + (fx1 - fx0) * 0.88, fy0 + (fy1 - fy0) * 0.50], fill=p["label"])
    d.rectangle([fx0 + (fx1 - fx0) * 0.12, fy0 + (fy1 - fy0) * 0.62, fx0 + (fx1 - fx0) * 0.60, fy0 + (fy1 - fy0) * 0.72], fill=p["accent"])
    if p["pattern"] == "tape": d.rectangle([fx0 + (fx1 - fx0) * 0.46, fy0, fx0 + (fx1 - fx0) * 0.54, fy1], fill=p["accent"])
    return im


def sprite_can(p, S):
    w, h = int(100 * S), int(150 * S)
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    c = p["color"]; x0, x1 = int(w * 0.08), int(w * 0.92); e = int(w * 0.16)
    d.ellipse([x0, h - e * 2 - 2, x1, h - 2], fill=shade(c, 0.6))
    d.rectangle([x0, e, x1, h - e - 2], fill=c)
    for i in range(x0, x1, 2):
        f = 0.65 + 0.55 * math.sin(math.pi * (i - x0) / (x1 - x0)); d.line([i, e, i, h - e - 2], fill=shade(c, f), width=2)
    d.rectangle([x0, h * 0.35, x1, h * 0.72], fill=p["label"])
    d.rectangle([x0, h * 0.50, x1, h * 0.56], fill=p["accent"])
    d.ellipse([x0, 0, x1, e * 2], fill=(205, 208, 214)); d.ellipse([x0 + w * 0.12, e * 0.5, x1 - w * 0.12, e * 1.5], fill=(150, 154, 162))
    return im


SPRITES = {"cup": sprite_cup, "bottle": sprite_bottle, "box": sprite_box, "can": sprite_can}
PALETTE = [(200, 40, 40), (40, 90, 190), (40, 150, 80), (230, 170, 30), (110, 60, 150), (30, 160, 170), (240, 240, 235), (60, 60, 65), (200, 110, 40), (150, 50, 90)]


def make_instances(rng, per_class=5):
    inst = []
    for cls in SPRITES:
        cols = rng.sample(PALETTE, per_class)
        for k, col in enumerate(cols):
            acc = rng.choice([c for c in PALETTE if c != col])
            inst.append({"inst": f"{cls}{k}", "cls": cls, "p": {
                "color": col, "accent": acc, "inner": shade(col, 0.35) if cls == "cup" else (30, 30, 30),
                "label": rng.choice([(245, 245, 240), (250, 235, 190), (30, 30, 35), acc]),
                "pattern": rng.choice(["plain", "stripe", "dots"]) if cls == "cup" else rng.choice(["plain", "tape"])}})
    return inst


def background(rng):
    kind = rng.choice(["gradient", "wood", "stripes", "noise", "tile", "dark"])
    a = np.zeros((H, W, 3), np.float32)
    base = np.array([rng.randint(60, 220) for _ in range(3)], np.float32)
    if kind == "gradient":
        t = np.linspace(0, 1, H)[:, None, None]; other = np.array([rng.randint(30, 230) for _ in range(3)], np.float32)
        a[:] = base * (1 - t) + other * t
    elif kind == "wood":
        a[:] = [150, 105, 60]; yy = np.arange(H)[:, None]
        a += (np.sin(yy / 5.0 + np.sin(np.arange(W)[None, :] / 90.0) * 3)[..., None] * 14)
    elif kind == "stripes":
        a[:] = base; a[:, ::2 * rng.randint(12, 40)] *= 0.8
        for x in range(0, W, 2 * rng.randint(16, 50)): a[:, x:x + rng.randint(8, 20)] *= 1.12
    elif kind == "tile":
        a[:] = base; s = rng.choice([40, 60, 80])
        for y in range(0, H, s):
            for x in range(0, W, s): a[y:y + s, x:x + s] *= 0.9 + 0.2 * ((x // s + y // s) % 2)
    elif kind == "dark":
        a[:] = [28, 30, 36]
    else:
        a[:] = base
    a += np.random.default_rng(rng.randint(0, 10 ** 9)).normal(0, rng.choice([2, 6, 12]), a.shape)
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(rng.choice([0, 0, 1.5])))


def place(rng, inst, canvas, taken):
    """Render one instance with random scale/rotation/lighting at a free position; returns the exact tight bbox."""
    scale = rng.uniform(0.55, 1.15)
    spr = SPRITES[inst["cls"]](inst["p"], SS)
    spr = spr.resize((max(8, int(spr.width * scale / SS)), max(8, int(spr.height * scale / SS))), Image.LANCZOS) if False else spr
    spr = spr.resize((max(16, int(spr.width * scale / SS * 1.0)), max(16, int(spr.height * scale / SS * 1.0))), Image.LANCZOS)
    ang = rng.uniform(-22, 22)
    spr = spr.rotate(ang, expand=True, resample=Image.BICUBIC)
    if rng.random() < 0.5: spr = spr.transpose(Image.FLIP_LEFT_RIGHT)
    rgb = spr.convert("RGB"); alpha = spr.split()[3]
    rgb = ImageEnhance.Brightness(rgb).enhance(rng.uniform(0.6, 1.3))
    cast = np.array([rng.uniform(0.9, 1.1) for _ in range(3)], np.float32)
    rgb = Image.fromarray(np.clip(np.asarray(rgb, np.float32) * cast, 0, 255).astype(np.uint8))
    bb = alpha.point(lambda v: 255 if v > 24 else 0).getbbox()
    if not bb: return None
    ow, oh = bb[2] - bb[0], bb[3] - bb[1]
    for _ in range(40):
        x = rng.randint(4, max(5, W - spr.width - 4)); y = rng.randint(4, max(5, H - spr.height - 4))
        box = (x + bb[0], y + bb[1], ow, oh)
        if all(box[0] + box[2] + 6 < t[0] or t[0] + t[2] + 6 < box[0] or box[1] + box[3] + 6 < t[1] or t[1] + t[3] + 6 < box[1] for t in taken):
            canvas.paste(rgb, (x, y), alpha)
            return list(box)
    return None


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("out"); ap.add_argument("--frames", type=int, default=240); ap.add_argument("--seed", type=int, default=20261105)
    a = ap.parse_args(); rng = random.Random(a.seed)
    os.makedirs(os.path.join(a.out, "frames"), exist_ok=True)
    insts = make_instances(rng)
    man = {"seed": a.seed, "width": W, "height": H, "instances": {i["inst"]: i["cls"] for i in insts}, "frames": []}
    for n in range(a.frames):
        canvas = background(rng); objs = []; taken = []
        # each frame holds 1-3 objects, instances chosen round-robin-ish so every instance appears many times
        for k in range(rng.choice([1, 1, 2, 3])):
            it = insts[(n * 3 + k * 7 + rng.randint(0, 1)) % len(insts)]
            if any(o["inst"] == it["inst"] for o in objs): continue
            box = place(rng, it, canvas, taken)
            if box: taken.append(box); objs.append({"inst": it["inst"], "cls": it["cls"], "bbox": box})
        if rng.random() < 0.5: canvas = ImageEnhance.Brightness(canvas).enhance(rng.uniform(0.75, 1.15))
        fn = f"obj_{n:04d}.jpg"; canvas.save(os.path.join(a.out, "frames", fn), "JPEG", quality=90)
        man["frames"].append({"file": fn, "objects": objs})
    json.dump(man, open(os.path.join(a.out, "manifest.json"), "w"), indent=1)
    no = sum(len(f["objects"]) for f in man["frames"])
    print(f"{a.frames} frames, {no} labelled objects, {len(insts)} instances ({len(SPRITES)} classes) -> {a.out}")


if __name__ == "__main__":
    main()
