#!/bin/sh
# Extract reference frames from an advertisement video for the Proof-of-Play reference library (HOST tool, needs ffmpeg).
#   make_reference_frames.sh <video|rtsp-url> <out_dir> [interval_s] [max_seconds] [profile]
#   profile "screenshot" (default): 640x400, aspect not preserved (matches roVideoMode.Screenshot() of a 1280x800 output).
#   profile "display_letterbox": 640x360 canvas with the picture scaled to 576x360 and centred on black (matches the OS rendered frames:
#            the 1280x800 display letterboxed into 640x360). VALIDATED geometry (native top-left crop of a 1920x1080 rectangle, median MSE 17.4 on real frames); still compare the first reference with a
#            real frame before trusting verdicts. Copy these to <store>/pop/refs_incoming/<media_id>.display_letterbox/ and build with
#            POST /api/pop/media/<id>/references/build?profile=display_letterbox.
# Writes <out_dir>/<offset_ms>.jpg at the display-capture geometry (640x400, aspect NOT preserved: the player's
# Screenshot() of a 1280x800 output stretches 16:9 media the same way) so reference and screenshot look alike.
# The original video is only read. The offset is the position in the media, which is what the verifier reports.
src="$1"; out="$2"; every="${3:-1}"; maxs="${4:-0}"; profile="${5:-screenshot}"
case "$profile" in
  screenshot)        vf_scale="scale=640:400" ;;
  display_letterbox) vf_scale="scale=1920:1080,crop=1280:800:0:0,scale=576:360:flags=lanczos,pad=640:360:32:0:black" ;;  # validated on real XT5 frames (scripts/pop/validate_display_geometry.py)
  *) echo "unknown profile '$profile' (screenshot|display_letterbox)" >&2; exit 2 ;;
esac
[ -n "$src" ] && [ -n "$out" ] || { echo "usage: $0 <video|rtsp-url> <out_dir> [interval_s] [max_seconds]" >&2; exit 2; }
mkdir -p "$out" || exit 1
lim=""; [ "$maxs" != "0" ] && lim="-t $maxs"
ffmpeg -v error -y -i "$src" $lim -vf "fps=1/${every},${vf_scale}" -q:v 3 -start_number 0 "$out/frame_%05d.jpg" || exit 1
python3 - "$out" "$every" <<'PY'
import os, sys
out, every = sys.argv[1], float(sys.argv[2])
for f in sorted(os.listdir(out)):
    if f.startswith("frame_") and f.endswith(".jpg"):
        n = int(f[6:11]); os.rename(os.path.join(out, f), os.path.join(out, "%d.jpg" % round(n * every * 1000)))
PY
ls "$out" | wc -l
