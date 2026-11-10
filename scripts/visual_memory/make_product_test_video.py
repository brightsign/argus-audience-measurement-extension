#!/usr/bin/env python3
"""Generate the original, reproducible 30-second "product test" video for display Object Search (HOST tool; Pillow, numpy, ffmpeg).

  make_product_test_video.py --out DIR [--fps 25] [--seed-offset 0]

Everything is drawn procedurally by this script (no photographs, no third-party artwork, no people), so the video is original and can be
regenerated bit-for-bit with the same ffmpeg/x264 build. The video is NOT stored in Git: it is written to --out, together with
manifest.json (SHA-256 of the video, scene timeline, exact object geometry, tool versions).

Timeline (1920x1080, H.264 High, 25 fps, no audio, 30.0 s):
  0-10 s  scene 1  water bottle (left) + coffee cup (right) on a table, warm wall
  10-20 s scene 2  the same two objects at different positions and sizes (cup left, bottle right), cool wall
  20-30 s scene 3  unrelated objects only (vase, orange, wooden blocks): no bottle, no cup

Geometry: the XT5 plays a video in a 1920x1080 rectangle and its 1280x800 output shows the TOP-LEFT 1280x800 of it (verified against real frames,
see scripts/pop/pop_reference_tools.py DISPLAY_VF). All objects are therefore kept inside x<1280, y<800 with a margin; the scene caption
is drawn OUTSIDE that window (it is visible when the file is opened on a PC, never on the player).
"""
import argparse, hashlib, json, math, os, subprocess, sys
import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

W, H = 1920, 1080
VIS_W, VIS_H = 1280, 800          # what the player's output shows (top-left of the 1080p rectangle)
SS = 2                            # supersampling for sprites
DURATION = 30.0
LIGHT = np.array([-0.45, -0.55, 0.70]); LIGHT = LIGHT / np.linalg.norm(LIGHT)


# ------------------------------------------------------------------------------------------------ shading helpers
def revolve(height, radius_px, profile, color_fn, spec=0.5, shininess=40, alpha_fn=None):
    """Body of revolution rendered with a cylinder normal. profile(t) in [0,1] -> radius fraction (t=0 top, 1 bottom). Returns RGBA float [0,1]."""
    hh, ww = int(height * SS), int(radius_px * 2 * SS)
    t = (np.arange(hh) + 0.5) / hh
    r = np.array([profile(x) for x in t])                      # radius fraction per row
    xs = (np.arange(ww) + 0.5) / ww * 2 - 1                    # -1..1 across the max width
    u = xs[None, :] / np.maximum(r[:, None], 1e-6)             # -1..1 across the local width
    inside = np.abs(u) <= 1.0
    nz = np.sqrt(np.clip(1 - u * u, 0, 1)); nx = np.clip(u, -1, 1)
    n = np.stack([nx, np.zeros_like(nx), nz], -1)
    diff = np.clip((n * LIGHT).sum(-1), 0, 1)
    refl = 2 * (n * LIGHT).sum(-1, keepdims=True) * n - LIGHT
    specv = np.clip(refl[..., 2], 0, 1) ** shininess
    base = color_fn(t[:, None] * np.ones_like(u), u)           # (hh, ww, 3) in 0..1
    rgb = base * (0.38 + 0.72 * diff[..., None]) + spec * specv[..., None]
    a = inside.astype(float)
    if alpha_fn is not None:
        a = a * alpha_fn(t[:, None] * np.ones_like(u), u)
    # soft edge (1 px antialias at supersampled size)
    edge = np.clip((1 - np.abs(u)) * r[:, None] * ww / 2, 0, 1)
    a = a * edge
    return np.dstack([np.clip(rgb, 0, 1), np.clip(a, 0, 1)])


def to_img(rgba):
    return Image.fromarray((rgba * 255 + 0.5).astype(np.uint8), "RGBA")


def downscale(im):
    return im.resize((im.width // SS, im.height // SS), Image.LANCZOS)


def lerp(a, b, f):
    return tuple(np.asarray(a) * (1 - f) + np.asarray(b) * f)


# ------------------------------------------------------------------------------------------------ objects
def bottle_sprite(h=500, w=170, cap_rgb=(0.96, 0.97, 0.99), tint=(0.40, 0.68, 0.93), label=True, opaque=0.0, shoulder=0.195, ribbed_cap=True):
    """PET-style water bottle: ribbed white cap, neck ring, S-curve shoulders, waist, translucent blue-tinted body with a waterline,
    paper label with a wave stripe, petaloid base. Drawn translucent so the table/wall show through like real clear plastic."""
    def ease(x): x = min(1.0, max(0.0, x)); return x * x * (3 - 2 * x)
    def prof(t):                       # t=0 top
        if t < 0.055: return 0.30                                   # cap
        if t < 0.072: return 0.345                                  # neck ring (flange)
        if t < 0.105: return 0.27                                   # neck
        if t < 0.105 + shoulder:  return 0.27 + 0.73 * ease((t - 0.105) / shoulder)   # shoulder (S-curve)
        if t < 0.58:  return 1.0
        if t < 0.70:  return 1.0 - 0.07 * math.sin((t - 0.58) / 0.12 * math.pi)  # grip waist
        if t < 0.90:  return 1.0
        return 1.0 - 0.18 * ease((t - 0.90) / 0.10)                # rounded base
    def col(t, u):
        c = np.zeros(t.shape + (3,))
        c[:] = np.array([0.55, 0.80, 0.96])                          # clear plastic, blue tint
        c[t > 0.215] = np.array(tint)                                # water
        c[(np.abs(t - 0.215) < 0.006)] = np.array([0.90, 0.96, 1.0]) # waterline / meniscus
        cap = (t < 0.055) | ((t >= 0.055) & (t < 0.072))
        c[cap] = np.array(cap_rgb) * ((0.86 + 0.14 * np.sin(u[cap] * 38)) if ribbed_cap else 1.0)[:, None] if ribbed_cap else np.array(cap_rgb)   # white ribbed cap + ring
        lab = ((t > 0.40) & (t < 0.64)) if label else np.zeros(t.shape, bool)   # label
        c[lab] = np.array([0.97, 0.98, 1.0])
        wave = lab & (np.abs(t - (0.52 + 0.035 * np.sin(u * 5.0))) < 0.022); c[wave] = np.array([0.06, 0.40, 0.82])
        wave2 = lab & (np.abs(t - (0.585 + 0.03 * np.sin(u * 5.0 + 1.0))) < 0.010); c[wave2] = np.array([0.35, 0.72, 0.95])
        ribs = (t > 0.78) & (t < 0.93) & (np.sin(t * 130) > 0.55); c[ribs] = c[ribs] * 0.86
        return c
    def alpha(t, u):
        a = np.where(t < 0.072, 1.0, opaque + (1 - opaque) * (0.50 + 0.38 * np.abs(u) ** 2))   # translucent plastic, denser at the edges
        a = np.where(t > 0.215, np.maximum(a, 0.62), a)
        a = np.where(((t > 0.40) & (t < 0.64)) if label else np.zeros(t.shape, bool), 0.97, a)
        return a
    return downscale(to_img(revolve(h, w / 2, prof, col, spec=0.65, shininess=24, alpha_fn=alpha)))


def cup_sprite(w=290, h=205, body_rgb=(0.78, 0.16, 0.14)):
    """Red ceramic coffee mug with coffee surface, a handle and a white saucer. Returns RGBA with a transparent background."""
    sw, sh = int(w * 1.40), int(h * 1.22)
    # body: truncated cone (wider at the rim)
    def prof(t): return 0.70 + 0.30 * (1 - t)
    def col(t, u): return np.ones(t.shape + (3,)) * np.array(body_rgb)
    body = to_img(revolve(h * 0.92, w / 2 * 0.86, prof, col, spec=0.6, shininess=35))
    canvas = Image.new("RGBA", (sw * SS, sh * SS), (0, 0, 0, 0))
    cx = int(sw * SS * 0.50); base_y = int(sh * SS * 0.86)
    d = ImageDraw.Draw(canvas)
    # saucer (ellipse, white) and its shadow
    sr = int(w * SS * 0.64); sry = int(h * SS * 0.13)
    d.ellipse([cx - sr, base_y - sry, cx + sr, base_y + sry], fill=(236, 236, 240, 255), outline=(190, 190, 200, 255), width=3)
    d.ellipse([cx - int(sr * 0.62), base_y - int(sry * 0.55), cx + int(sr * 0.62), base_y + int(sry * 0.45)], fill=(220, 220, 228, 255))
    # handle (ring on the right)
    bw = body.width; bh = body.height
    hx = cx + int(bw * 0.46); hy = base_y - int(bh * 0.78)
    hr = int(bh * 0.34)
    d.ellipse([hx - hr // 2, hy, hx + hr * 1.25, hy + int(hr * 1.6)], outline=tuple(int(c * 255) for c in body_rgb) + (255,), width=int(h * SS * 0.085))
    # body
    canvas.alpha_composite(body, (cx - bw // 2, base_y - bh - int(sry * 0.2)))
    d = ImageDraw.Draw(canvas)
    # rim ellipse + coffee surface
    rim_w = int(bw * 0.995); rim_h = int(bh * 0.17); ry = base_y - bh - int(sry * 0.2)
    d.ellipse([cx - rim_w // 2, ry - rim_h // 2, cx + rim_w // 2, ry + rim_h // 2], fill=(250, 246, 240, 255), outline=(150, 40, 30, 255), width=2)
    d.ellipse([cx - int(rim_w * 0.44), ry - int(rim_h * 0.40), cx + int(rim_w * 0.44), ry + int(rim_h * 0.40)], fill=(70, 38, 20, 255))
    d.ellipse([cx - int(rim_w * 0.26), ry - int(rim_h * 0.22), cx + int(rim_w * 0.10), ry + int(rim_h * 0.05)], fill=(112, 70, 38, 255))   # crema highlight
    return downscale(canvas)


def vase_sprite(h=360, w=210):
    def prof(t): return 0.35 + 0.65 * math.sin(math.pi * min(1.0, 0.1 + t * 0.95)) ** 0.9 if t > 0.02 else 0.32
    def col(t, u):
        c = np.ones(t.shape + (3,)) * np.array([0.12, 0.45, 0.42]); c[(t > 0.4) & (t < 0.48)] = np.array([0.95, 0.80, 0.30]); return c
    return downscale(to_img(revolve(h, w / 2, prof, col, spec=0.7, shininess=50)))


def orange_sprite(d=170):
    n = d * SS; yy, xx = np.mgrid[0:n, 0:n]; u = (xx + 0.5) / n * 2 - 1; v = (yy + 0.5) / n * 2 - 1
    rr = np.sqrt(u * u + v * v); inside = rr <= 1
    nz = np.sqrt(np.clip(1 - rr * rr, 0, 1)); nrm = np.stack([u, v, nz], -1)
    diff = np.clip((nrm * LIGHT).sum(-1), 0, 1); refl = 2 * (nrm * LIGHT).sum(-1, keepdims=True) * nrm - LIGHT
    sp = np.clip(refl[..., 2], 0, 1) ** 25
    bump = 0.04 * np.sin(xx * 0.9) * np.sin(yy * 0.9)
    rgb = np.array([0.98, 0.52, 0.05]) * (0.4 + 0.7 * diff[..., None] + bump[..., None]) + 0.3 * sp[..., None]
    a = np.clip((1 - rr) * n / 2, 0, 1) * inside
    return downscale(to_img(np.dstack([np.clip(rgb, 0, 1), a])))


def block_sprite(s=150, rgb=(0.80, 0.55, 0.25), rot=0.0):
    n = s * SS; im = Image.new("RGBA", (n, n), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    c = lambda f: tuple(int(min(1, x * f) * 255) for x in rgb) + (255,)
    top = int(n * 0.22)
    d.polygon([(0, top), (int(n * 0.2), 0), (n, 0), (n, n - top), (int(n * 0.8), n), (0, n)], fill=c(0.8))
    d.polygon([(0, top), (int(n * 0.8), top), (int(n * 0.8), n), (0, n)], fill=c(1.0))                       # front face
    d.polygon([(int(n * 0.8), top), (n, 0), (n, n - top), (int(n * 0.8), n)], fill=c(0.62))                   # side
    d.polygon([(0, top), (int(n * 0.2), 0), (n, 0), (int(n * 0.8), top)], fill=c(1.18))                       # top
    d.line([(int(n * 0.1), int(n * 0.55)), (int(n * 0.7), int(n * 0.55))], fill=c(0.7), width=max(2, SS * 3))
    return downscale(im).rotate(rot, expand=True, resample=Image.BICUBIC)


# ------------------------------------------------------------------------------------------------ scenes
def scene_background(wall_a, wall_b, table_a, table_b, horizon=640):
    """Soft wall gradient + wooden table with a faint grain; fully deterministic."""
    yy = np.linspace(0, 1, H)[:, None]; xx = np.linspace(0, 1, W)[None, :]
    wall = np.zeros((H, W, 3))
    for k in range(3):
        wall[..., k] = wall_a[k] * (1 - yy * 0.5) + wall_b[k] * (yy * 0.5) + 0.05 * np.sin(xx * 3.0 + k) * 0
    # vignette
    vig = 1 - 0.16 * ((xx - 0.33) ** 2 + (yy - 0.4) ** 2)
    wall *= vig[..., None]
    img = (np.clip(wall, 0, 1) * 255).astype(np.uint8)
    pil = Image.fromarray(img, "RGB")
    d = ImageDraw.Draw(pil)
    for y in range(horizon, H):
        f = (y - horizon) / (H - horizon)
        col = tuple(int(255 * (table_a[k] * (1 - f) + table_b[k] * f)) for k in range(3))
        d.line([(0, y), (W, y)], fill=col)
    for i in range(0, 60):                                      # wood grain streaks
        y = horizon + 8 + i * 9 + (i * 37) % 7
        shade = 14 if i % 2 else -10
        d.line([(0, y), (W, y + ((i * 13) % 5 - 2))], fill=tuple(int(max(0, min(255, (table_a[k] * 255) + shade))) for k in range(3)), width=2)
    d.rectangle([0, horizon - 3, W, horizon + 3], fill=tuple(int(c * 255 * 0.75) for c in table_a))   # table edge line
    return pil.convert("RGBA")


def shadow_sprite(w, h, strength=110):
    im = Image.new("RGBA", (w * 2, h * 4), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    d.ellipse([w // 2, h, w * 3 // 2, h * 3], fill=(20, 12, 5, strength))
    return im.filter(ImageFilter.GaussianBlur(h * 0.45))


def paste(base, sprite, cx, base_y, dx=0.0):
    """Stand a sprite on the table: centre x, bottom y."""
    x = int(round(cx + dx - sprite.width / 2)); y = int(round(base_y - sprite.height))
    base.alpha_composite(sprite, (max(0, x), max(0, y))) if x >= 0 and y >= 0 else base.alpha_composite(sprite.crop((max(0, -x), max(0, -y), sprite.width, sprite.height)), (max(0, x), max(0, y)))


def steam_layer(t, cx, y, scale=1.0):
    im = Image.new("RGBA", (260, 260), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    for k, off in enumerate((-30, 0, 30)):
        pts = [(130 + off + 14 * math.sin(t * 2.2 + k + i * 0.7), 250 - i * 18) for i in range(13)]
        d.line(pts, fill=(255, 255, 255, 120), width=7, joint="curve")
    im = im.filter(ImageFilter.GaussianBlur(5))
    if scale != 1.0: im = im.resize((int(im.width * scale), int(im.height * scale)), Image.LANCZOS)
    return im, int(cx - im.width / 2), int(y - im.height)


class Scene:
    def __init__(self, name, bg, items, caption):
        self.name, self.bg, self.items, self.caption = name, bg, items, caption   # items: (sprite, cx, base_y, drift_amp, drift_period, label)


def build_scenes():
    bottle = bottle_sprite(215, 65, cap_rgb=(0.1, 0.3, 0.85)); cup = cup_sprite(235, 165)
    bottle_big = bottle_sprite(240, 72, cap_rgb=(0.1, 0.3, 0.85)); cup_small = cup_sprite(205, 145)
    vase = vase_sprite(360, 210); orange = orange_sprite(170); blk1 = block_sprite(170, (0.82, 0.58, 0.28), 6); blk2 = block_sprite(140, (0.32, 0.55, 0.80), -10)
    s1 = Scene("scene1", scene_background((0.93, 0.89, 0.80), (0.86, 0.78, 0.66), (0.50, 0.33, 0.19), (0.36, 0.22, 0.12)),
               [(bottle, 330, 700, 14, 7.0, "water bottle"), (cup, 760, 700, 10, 5.0, "coffee cup")],
               "SCENE 1  bottle (left) + cup (right)")
    s2 = Scene("scene2", scene_background((0.80, 0.88, 0.94), (0.66, 0.76, 0.86), (0.60, 0.44, 0.28), (0.42, 0.29, 0.17)),
               [(cup_small, 360, 705, 10, 5.0, "coffee cup"), (bottle_big, 860, 715, 14, 7.0, "water bottle")],
               "SCENE 2  cup (left) + bottle (right), other positions and sizes")
    s3 = Scene("scene3", scene_background((0.90, 0.92, 0.84), (0.78, 0.84, 0.70), (0.55, 0.40, 0.25), (0.40, 0.27, 0.15)),
               [(vase, 250, 700, 10, 6.0, "vase"), (orange, 620, 700, 8, 4.0, "orange"), (blk1, 860, 700, 0, 1, "block"), (blk2, 1040, 700, 0, 1, "block")],
               "SCENE 3  unrelated objects (no bottle, no cup)")
    return [s1, s2, s3]


def render_frame(scene, t_local, font):
    f = scene.bg.copy()
    for sprite, cx, by, amp, per, label in scene.items:
        sh = shadow_sprite(sprite.width, max(14, sprite.height // 14))
        f.alpha_composite(sh, (int(cx - sh.width / 2), int(by - sh.height / 2 - 4))) if cx - sh.width / 2 >= 0 else None
        dx = amp * math.sin(2 * math.pi * t_local / per)
        paste(f, sprite, cx, by, dx)
        if label == "coffee cup":
            st, sx, sy = steam_layer(t_local, cx + dx - 14, by - sprite.height * 0.78, 0.7)
            f.alpha_composite(st, (max(0, sx), max(0, sy)))
    d = ImageDraw.Draw(f)
    d.text((60, 900), scene.caption, fill=(30, 30, 30, 255), font=font)        # outside the 1280x800 player window
    d.text((60, 950), "t=%5.2fs  (original generated test media)" % t_local, fill=(60, 60, 60, 255), font=font)
    return f.convert("RGB")


def encode(out_path, fps):
    cmd = ["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}", "-r", str(fps), "-i", "-", "-an",
           "-c:v", "libx264", "-preset", "medium", "-crf", "18", "-profile:v", "high", "-level", "4.1", "-pix_fmt", "yuv420p", "-g", str(fps * 2),
           "-bf", "2", "-threads", "1", "-movflags", "+faststart", "-metadata", "title=Argus product test (generated)", "-fflags", "+bitexact", "-flags:v", "+bitexact", out_path]
    return subprocess.Popen(cmd, stdin=subprocess.PIPE)


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""): h.update(c)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True); ap.add_argument("--fps", type=int, default=25)
    ap.add_argument("--stills-only", action="store_true", help="write one still per scene (PNG) instead of the video, for quick iteration")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    try: font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 34)
    except Exception: font = ImageFont.load_default()
    scenes = build_scenes()
    if a.stills_only:
        for s in scenes: render_frame(s, 1.0, font).save(os.path.join(a.out, s.name + ".png"))
        return
    out = os.path.join(a.out, "product_test_30s.mp4")
    enc = encode(out, a.fps); nfr = int(DURATION * a.fps); per = nfr // len(scenes)
    for i in range(nfr):
        s = scenes[min(i // per, len(scenes) - 1)]; tl = (i - (i // per) * per) / a.fps
        enc.stdin.write(render_frame(s, tl, font).tobytes())
    enc.stdin.close(); enc.wait()
    if enc.returncode: sys.exit("ffmpeg failed")
    ver = subprocess.check_output(["ffmpeg", "-version"]).decode().split("\n")[0]
    man = {"file": os.path.basename(out), "sha256": sha256(out), "bytes": os.path.getsize(out), "duration_s": DURATION, "size": [W, H], "fps": a.fps,
           "codec": "h264 high yuv420p crf18, no audio", "generator": "scripts/visual_memory/make_product_test_video.py", "ffmpeg": ver,
           "player_window": [0, 0, VIS_W, VIS_H], "note": "objects are kept inside the top-left 1280x800 window that the XT5 output shows; caption outside it",
           "scenes": [{"name": s.name, "start_s": k * (DURATION / len(scenes)), "end_s": (k + 1) * (DURATION / len(scenes)), "caption": s.caption,
                       "objects": [{"label": it[5], "centre_x": it[1], "base_y": it[2], "size_px": [it[0].width, it[0].height], "drift_amp_px": it[3]} for it in s.items]}
                      for k, s in enumerate(scenes)]}
    json.dump(man, open(os.path.join(a.out, "product_test_30s.manifest.json"), "w"), indent=1)
    print(json.dumps({"video": out, "sha256": man["sha256"]}))


if __name__ == "__main__":
    main()
