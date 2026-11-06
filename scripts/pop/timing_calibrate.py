#!/usr/bin/env python3
"""Measure the real timing offset of Proof of Play evidence frames (HOST tool; ffmpeg, numpy, PIL, HTTP API read-only).

  timing_calibrate.py --api http://PLAYER:20300 --session grpc-a --video meet-brightsign.mp4 [--session ...] [--loop-s 0]

Every stored evidence image of the given sessions is matched against the ORIGINAL video at 25 fps in the verified display geometry
(scripts/pop/validate_display_geometry.py). Its content time minus the time the session assumed (actual_offset_ms = capture time - play_started event time)
is the end-to-end timing error of that frame: producer capture latency + OS notify + the player's event latency + the adapter's event timing. That is a
MEASUREMENT, unlike the configured uncertainty budget (producer_latency_ms), and shows whether the budget is adequate. Use sessions of one advertisement
played from its start.
"""
import argparse, io, json, statistics, subprocess, urllib.request
import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("--api", required=True); ap.add_argument("--session", action="append", required=True); ap.add_argument("--video", required=True)
ap.add_argument("--loop-s", type=float, default=0.0, help="if the ad loops inside the slot: its duration; assumed times are taken modulo this")
ap.add_argument("--out", default="timing_calibration.json")
a = ap.parse_args()
GW, GH, FPS = 144, 90, 25
vf = "scale=1920:1080,crop=1280:800:0:0,scale=576:360:flags=lanczos,scale=%d:%d:flags=area,format=gray" % (GW, GH)
raw = subprocess.run(["ffmpeg", "-v", "error", "-i", a.video, "-vf", "fps=%d,%s" % (FPS, vf), "-f", "rawvideo", "-pix_fmt", "gray", "-"], capture_output=True).stdout
V = np.frombuffer(raw, np.uint8).reshape(-1, GH, GW).astype(np.float32)
errs, rows = [], []
for sid in a.session:
    d = json.loads(urllib.request.urlopen(a.api.rstrip("/") + "/api/pop/sessions/" + sid, timeout=15).read())
    for e in d.get("evidence", []):
        if e["status"] != "ok" or not e.get("image_url"):
            continue
        img = Image.open(io.BytesIO(urllib.request.urlopen(a.api.rstrip("/") + e["image_url"], timeout=15).read())).convert("L")
        if img.size == (640, 360):
            img = img.crop((32, 0, 608, 360))
        g = np.asarray(img.resize((GW, GH), Image.BOX), np.float32)
        m = ((V - g) ** 2).mean(axis=(1, 2))
        i = int(m.argmin())
        t = i / FPS
        assumed = e["actual_offset_ms"] / 1000.0
        if a.loop_s:
            assumed %= a.loop_s
        row = {"session": sid, "idx": e["idx"], "method": e.get("capture_method"), "assumed_s": round(assumed, 3), "content_s": round(t, 3),
               "error_ms": round((t - assumed) * 1000), "match_mse": round(float(m[i]), 1)}
        rows.append(row)
        if m[i] < 60:
            errs.append(row["error_ms"])
for r in rows:
    print(r)
if errs:
    s = {"frames": len(errs), "median_ms": statistics.median(errs), "p5_ms": float(np.percentile(errs, 5)), "p95_ms": float(np.percentile(errs, 95)),
         "min_ms": min(errs), "max_ms": max(errs),
         "reading": "error = content time - assumed time. Positive: the frame shows LATER content than assumed (the play_started event was early or the player lagged); "
                    "negative: earlier. Compare with display.producer_latency_ms and the start/end guards."}
    print(json.dumps(s, indent=1))
    json.dump({"summary": s, "rows": rows}, open(a.out, "w"), indent=1)
