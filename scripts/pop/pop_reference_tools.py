#!/usr/bin/env python3
"""Reference images and similarity calibration for Proof of Play with the OS rendered frames (HOST tool; ffmpeg, numpy, PIL, onnxruntime).

  pop_reference_tools.py make-refs  --video ad.mp4 --media-id ad-a --out DIR [--interval 1] [--max-seconds 0]
  pop_reference_tools.py calibrate  --video-a a.mp4 --video-b b.mp4 --real-a DIR_OF_REAL_FRAMES --out calibration.json

make-refs writes DIR/<media_id>.display_letterbox/<offset_ms>.jpg (640x360, picture 576x360 centred on black, built from the ORIGINAL video with the
geometry that scripts/pop/validate_display_geometry.py verified against real captured frames) and DIR/<media_id>.manifest.json (media hash, ffprobe data,
exact filter, JPEG parameters, interval, tool versions, geometry evidence). Never uses captured frames as references.

calibrate compares cosine similarities of the MobileNetV2 embedding (ONNX, the same pre-processing as the device's "pp1": Pillow-style bilinear resize to
224x224 + ImageNet normalisation) for GENUINE captured frames of ad A, against references of A (positives) and B (negatives), plus geometry-faithful simulated
frames of B against A's references (negatives). The device runs the same network as FP16 RKNN; values differ slightly, so this is a proxy until the hardware run.
"""
import argparse, glob, hashlib, io, json, os, random, re, statistics, subprocess, sys
import numpy as np
from PIL import Image

# Verified on 40 real frames of the XT5 (median MSE 17.4 vs >= 3600 for every other hypothesis): the player puts the video into a 1920x1080 rectangle and the
# 1280x800 output shows its top-left part; the OS producer letterboxes that 1280x800 picture into 640x360 (576x360 centred).
DISPLAY_VF = "scale=1920:1080,crop=1280:800:0:0,scale=576:360:flags=lanczos,pad=640:360:32:0:black"
JPEG_Q = 5      # ffmpeg -q:v (about quality 90)
LEGACY_VF = "scale=640:400"       # the old screenshot-backend assumption (full frame stretched), kept only to quantify why it is not used here


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""): h.update(c)
    return h.hexdigest()


def probe(p):
    j = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=codec_name,width,height,r_frame_rate,pix_fmt:format=duration", "-of", "json", p]))
    s = j["streams"][0]
    return {"codec": s["codec_name"], "width": s["width"], "height": s["height"], "frame_rate": s["r_frame_rate"], "pix_fmt": s["pix_fmt"], "duration_s": float(j["format"]["duration"])}


def extract(video, vf, out_dir, interval, max_s=0, q=JPEG_Q):
    os.makedirs(out_dir, exist_ok=True)
    lim = ["-t", str(max_s)] if max_s else []
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", video] + lim + ["-vf", f"fps=1/{interval},{vf}", "-q:v", str(q), "-start_number", "0", os.path.join(out_dir, "frame_%05d.jpg")], check=True)
    n = 0
    for f in sorted(os.listdir(out_dir)):
        m = re.fullmatch(r"frame_(\d{5})\.jpg", f)
        if m:
            os.rename(os.path.join(out_dir, f), os.path.join(out_dir, "%d.jpg" % round(int(m.group(1)) * interval * 1000))); n += 1
    return n


def cmd_make_refs(a):
    d = os.path.join(a.out, a.media_id + ".display_letterbox")
    n = extract(a.video, DISPLAY_VF, d, a.interval, a.max_seconds)
    ver = subprocess.run(["ffmpeg", "-version"], capture_output=True, text=True).stdout.splitlines()[0]
    man = {"media_id": a.media_id, "profile": "display_letterbox", "video_file": os.path.basename(a.video), "video_sha256": sha256(a.video), "video": probe(a.video),
           "frames": n, "interval_s": a.interval, "max_seconds": a.max_seconds, "ffmpeg_filter": DISPLAY_VF, "jpeg": {"encoder": "mjpeg", "ffmpeg_qscale": JPEG_Q},
           "output_geometry": "640x360 JPEG, picture 576x360 centred (32 px black bars)", "tool": ver,
           "geometry_evidence": "scripts/pop/validate_display_geometry.py on 40 real XT5 frames of meet-brightsign.mp4: best hypothesis rect_1920x1080_top_left_native_crop, median MSE 17.4 (next best 3621); for 3840x2160 media the player is ASSUMED to scale into the same 1920x1080 rectangle (not verified with real frames)",
           "source_of_images": "ORIGINAL video only (no captured frames)", "embedding_model": json.load(open(os.path.join(os.path.dirname(__file__), "../../libs/visual-memory/models/mobilenet_embedder.json"))).get("model_version"),
           "generated_by": "scripts/pop/pop_reference_tools.py make-refs"}
    json.dump(man, open(os.path.join(a.out, a.media_id + ".manifest.json"), "w"), indent=1)
    print(json.dumps({k: man[k] for k in ("media_id", "video_sha256", "frames", "output_geometry")}))


# ----------------------------------------------------------------------------- calibration
class Embedder:
    def __init__(self, onnx):
        import onnxruntime as ort
        self.s = ort.InferenceSession(onnx, providers=["CPUExecutionProvider"])
        self.mean = np.array([0.485, 0.456, 0.406], np.float32); self.std = np.array([0.229, 0.224, 0.225], np.float32)

    def __call__(self, jpg_bytes):
        im = Image.open(io.BytesIO(jpg_bytes)).convert("RGB").resize((224, 224), Image.BILINEAR)
        x = ((np.asarray(im, np.float32) / 255.0 - self.mean) / self.std).transpose(2, 0, 1)[None]
        e = self.s.run(None, {"input": x})[0][0]
        return e / np.linalg.norm(e)


def load_dir(d):
    out = []
    for p in sorted(glob.glob(os.path.join(d, "*.jpg")), key=lambda p: int(re.findall(r"\d+", os.path.basename(p))[-1])):
        out.append(open(p, "rb").read())
    return out


def pct(v, p): v = sorted(v); return v[min(len(v) - 1, int(p * len(v)))]


def summ(v): return {"n": len(v), "min": round(min(v), 4), "p5": round(pct(v, .05), 4), "median": round(statistics.median(v), 4), "p95": round(pct(v, .95), 4), "max": round(max(v), 4)}


def cmd_calibrate(a):
    emb = Embedder(os.path.join(os.path.dirname(__file__), "../../libs/visual-memory/models/mobilenet_v2_embedder.onnx"))
    tmp = a.work
    refs = {}
    for name, vid in (("a", a.video_a), ("b", a.video_b)):
        d = os.path.join(tmp, name + "_display"); extract(vid, DISPLAY_VF, d, 1.0); refs[name] = np.stack([emb(b) for b in load_dir(d)])
    dleg = os.path.join(tmp, "a_legacy"); extract(a.video_a, LEGACY_VF, dleg, 1.0); refs["a_legacy"] = np.stack([emb(b) for b in load_dir(dleg)])
    real = [open(p, "rb").read() for p in sorted(glob.glob(os.path.join(a.real_a, "*.jpg")), key=lambda p: int(re.findall(r"\d+", os.path.basename(p))[-1]))]
    R = np.stack([emb(b) for b in real])
    best = lambda E, S: [float((S @ e).max()) for e in E]
    res = {"model": "mobilenet_v2_embedder tv-imagenet1k-v1 (ONNX proxy for the device's FP16 RKNN)", "reference_spacing_s": 1.0, "real_frames_of_A": len(real)}
    pos = best(R, refs["a"]); neg_ab = best(R, refs["b"]); pos_leg = best(R, refs["a_legacy"])
    res["positives_real_A_vs_A_display_refs"] = summ(pos)
    res["negatives_real_A_vs_B_display_refs"] = summ(neg_ab)
    res["positives_real_A_vs_legacy_stretched_refs"] = summ(pos_leg)
    # simulated, geometry-faithful frames of B (no real B frames exist yet) against A's references: what a wrong advertisement on screen looks like to the expected-A verification
    rnd = random.Random(7)
    dur = probe(a.video_b)["duration_s"]
    sim_dir = os.path.join(tmp, "b_sim"); os.makedirs(sim_dir, exist_ok=True)
    sims = []
    for i in range(60):
        t = rnd.uniform(0, dur - 0.5); p = os.path.join(sim_dir, "%d.jpg" % i)
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-ss", f"{t:.3f}", "-i", a.video_b, "-frames:v", "1", "-vf", DISPLAY_VF, "-q:v", "5", p], check=True)
        sims.append(emb(open(p, "rb").read()))
    res["negatives_simulated_B_frames_vs_A_display_refs"] = summ(best(sims, refs["a"]))
    res["note_simulated_B"] = "geometry-faithful simulation from the original B video (no real B frames captured yet)"
    # the unrelated-content floor: A's own references vs B's references
    res["floor_A_refs_vs_B_refs"] = summ([float((refs["b"] @ e).max()) for e in refs["a"]])
    negs = neg_ab + best(sims, refs["a"])
    res["decision"] = {"existing_thresholds": {"match_sim": 0.80, "reject_sim": 0.60},
                       "positives_below_0_80": sum(1 for v in pos if v < 0.80), "positives_below_0_60": sum(1 for v in pos if v < 0.60),
                       "negatives_at_or_above_0_60": sum(1 for v in negs if v >= 0.60), "negatives_at_or_above_0_80": sum(1 for v in negs if v >= 0.80),
                       "separation_gap_min_pos_minus_max_neg": round(min(pos) - max(negs), 4),
                       "suggested_midpoint_between_min_pos_and_max_neg": round((min(pos) + max(negs)) / 2, 4)}
    json.dump(res, open(a.out, "w"), indent=1)
    print(json.dumps(res, indent=1))


ap = argparse.ArgumentParser()
sp = ap.add_subparsers(dest="cmd", required=True)
m = sp.add_parser("make-refs"); m.add_argument("--video", required=True); m.add_argument("--media-id", required=True); m.add_argument("--out", required=True)
m.add_argument("--interval", type=float, default=1.0); m.add_argument("--max-seconds", type=float, default=0)
c = sp.add_parser("calibrate"); c.add_argument("--video-a", required=True); c.add_argument("--video-b", required=True); c.add_argument("--real-a", required=True)
c.add_argument("--out", default="calibration.json"); c.add_argument("--work", default="/tmp/pop_calib")
a = ap.parse_args()
{"make-refs": cmd_make_refs, "calibrate": cmd_calibrate}[a.cmd](a)
