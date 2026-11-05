#!/usr/bin/env python3
"""Find and verify the geometry of the OS rendered frames against the ORIGINAL advertisement video (HOST tool; needs ffmpeg, numpy, PIL).

  validate_display_geometry.py --video meet-brightsign.mp4 --frames DIR_OF_REAL_FRAMES --out report.json [--max-frames 40]

The OS producer delivers 640x360 JPEGs of the 1280x800 display, letterboxed (picture 576x360 centred on black). How the player puts a 16:9 video on the
16:10 display (crop to fill? stretch? fit?) is not documented, so several hypotheses are simulated from the ORIGINAL video (never from captured frames) and
each real frame is matched against every simulated frame of the video. The right hypothesis explains the real frames with a clearly lower error, and the
matched video times of consecutive real frames advance at the real elapsed rate (a wrong geometry or wrong content does not).
"""
import argparse, glob, json, os, re, statistics, subprocess, sys
import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("--video", required=True); ap.add_argument("--frames", required=True); ap.add_argument("--out", default="geometry_report.json")
ap.add_argument("--max-frames", type=int, default=40); ap.add_argument("--fps", type=float, default=5.0)
a = ap.parse_args()

GW, GH = 144, 90                      # comparison size (the 576x360 picture / 4)
# hypotheses: how the 1920x1080 (16:9) video becomes the 1280x800 (16:10) display picture, then the 576x360 letterboxed picture
HYP = {
    # The player application places the video in a 1920x1080 rectangle (roRectangle 0,0,1920,1080); the 1280x800 output shows the part of it that fits.
    "rect_1920x1080_top_left_native_crop": "scale=1920:1080,crop=1280:800:0:0",
    "rect_1920x1080_centred_crop":         "scale=1920:1080,crop=1280:800:(iw-1280)/2:(ih-800)/2",
    "rect_1920x1080_top_right_crop":       "scale=1920:1080,crop=1280:800:iw-1280:0",
    "fill_crop_left_justified": "scale=-2:800,crop=1280:800:0:0",                     # scale to the display height, keep the left, cut the right
    "fill_crop_quarter":        "scale=-2:800,crop=1280:800:(iw-1280)/4:0",
    "fill_crop_centred":        "scale=-2:800,crop=1280:800:(iw-1280)/2:0",
    "fill_crop_three_quarter":  "scale=-2:800,crop=1280:800:(iw-1280)*3/4:0",
    "fill_crop_right_justified": "scale=-2:800,crop=1280:800:iw-1280:0",
    "stretch_to_display":       "scale=1280:800",
    "fit_letterbox":            "scale=1280:-2,pad=1280:800:0:(800-ih)/2:black",
}


def simulate(vf):
    cmd = ["ffmpeg", "-v", "error", "-i", a.video, "-vf", f"fps={a.fps},{vf},scale=576:360:flags=lanczos,scale={GW}:{GH}:flags=area,format=gray",
           "-f", "rawvideo", "-pix_fmt", "gray", "-"]
    raw = subprocess.run(cmd, capture_output=True).stdout
    n = len(raw) // (GW * GH)
    return np.frombuffer(raw[: n * GW * GH], np.uint8).reshape(n, GH, GW).astype(np.float32)


def real_gray(p):
    im = Image.open(p).convert("L")
    content = im.crop((32, 0, 608, 360)).resize((GW, GH), Image.BOX)
    bars = np.asarray(im, np.float32)
    return np.asarray(content, np.float32), float(bars[:, :28].mean()), float(bars[:, 612:].mean())


files = sorted(glob.glob(os.path.join(a.frames, "*.jpg")), key=lambda p: int(re.findall(r"\d+", os.path.basename(p))[-1]))
step = max(1, len(files) // a.max_frames)
files = files[::step][: a.max_frames]
real = [real_gray(p) for p in files]
R = np.stack([r[0] for r in real])
report = {"video": os.path.basename(a.video), "real_frames_used": len(files), "side_bar_mean_level": {"left": round(float(np.mean([r[1] for r in real])), 3), "right": round(float(np.mean([r[2] for r in real])), 3)},
          "simulated_fps": a.fps, "hypotheses": {}}
for name, vf in HYP.items():
    V = simulate(vf)
    if len(V) < 5: report["hypotheses"][name] = {"error": "no frames"}; continue
    best_t, best_e = [], []
    for g in R:
        e = ((V - g) ** 2).mean(axis=(1, 2))
        i = int(e.argmin()); best_t.append(i / a.fps); best_e.append(float(e[i]))
    # the real frames are consecutive in time (producer cadence ~0.2 s, stride `step`): the matched video times must advance accordingly (modulo the loop)
    d = np.diff(best_t)
    report["hypotheses"][name] = {"median_mse": round(statistics.median(best_e), 1), "mean_mse": round(float(np.mean(best_e)), 1),
                                  "monotonic_fraction": round(float(np.mean((d >= -a.fps ** -1) & (d <= 3.0))), 2),
                                  "matched_video_time_range_s": [round(min(best_t), 1), round(max(best_t), 1)]}
ranked = sorted((h for h in report["hypotheses"] if "median_mse" in report["hypotheses"][h]), key=lambda h: report["hypotheses"][h]["median_mse"])
report["best_hypothesis"] = ranked[0]
report["runner_up"] = {"name": ranked[1], "median_mse": report["hypotheses"][ranked[1]]["median_mse"]}
report["separation_ratio_runner_up_over_best"] = round(report["hypotheses"][ranked[1]]["median_mse"] / max(report["hypotheses"][ranked[0]]["median_mse"], 1e-6), 2)
json.dump(report, open(a.out, "w"), indent=1)
print(json.dumps(report, indent=1))
