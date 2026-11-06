#!/usr/bin/env python3
"""Register an advertisement in the Proof-of-Play reference library (HOST tool: needs ffprobe).

  register_media.py --api http://PLAYER:20398 --id ad-a --name "Advertisement A" --file meet-brightsign.mp4 --uri meet-brightsign.mp4 [--slot 30]
  register_media.py --api ... --id live-cam --rtsp rtsp://host:8554/cam1          (live stream: no hash/duration)

Computes the SHA-256 and reads codec/size/duration with ffprobe from the original file (read only), then POSTs the asset.
Reference frames are built separately (make_reference_frames.sh, copy to <store>/pop/refs_incoming/<id>/, then
POST /api/pop/media/<id>/references/build).
"""
import argparse, hashlib, json, subprocess, sys, urllib.error, urllib.request

ap = argparse.ArgumentParser()
ap.add_argument("--api", required=True); ap.add_argument("--id", required=True); ap.add_argument("--name", default="")
ap.add_argument("--file"); ap.add_argument("--rtsp"); ap.add_argument("--uri", help="how the PLAYER is told to play it (default: file name / URL)")
ap.add_argument("--slot", type=float, default=30.0)
a = ap.parse_args()
src = a.file or a.rtsp
if not src:
    sys.exit("give --file or --rtsp")
cmd = ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=codec_name,width,height,r_frame_rate:format=duration", "-of", "json"]
if a.rtsp:
    cmd += ["-rtsp_transport", "tcp"]
pr = json.loads(subprocess.check_output(cmd + [src]))
st = pr["streams"][0]
num, den = st["r_frame_rate"].split("/")
body = {"media_id": a.id, "display_name": a.name or a.id, "kind": "rtsp" if a.rtsp else "file", "uri": a.uri or (a.rtsp or a.file.split("/")[-1]),
        "codec": st["codec_name"], "width": st["width"], "height": st["height"], "fps": round(float(num) / float(den), 3), "slot_s": a.slot}
if a.file:
    h = hashlib.sha256()
    with open(a.file, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    body["sha256"] = h.hexdigest()
    body["duration_s"] = round(float(pr["format"]["duration"]), 3)
req = urllib.request.Request(a.api.rstrip("/") + "/api/pop/media", data=json.dumps(body).encode(), method="POST",
                             headers={"Content-Type": "application/json", "Origin": a.api.rstrip("/")})
try:
    with urllib.request.urlopen(req, timeout=30) as r:
        print(r.status, r.read().decode())
except urllib.error.HTTPError as e:
    print(e.code, e.read().decode())
    sys.exit(1)
