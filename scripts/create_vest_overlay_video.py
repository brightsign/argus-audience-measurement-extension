#!/usr/bin/env python3
"""
Create a test video by compositing employee vest images onto the shopping-area video.

Background is removed from each vest PNG using rembg (U2Net) to produce clean
transparent cutouts, which are then scaled to realistic person height and overlaid
at different positions / time segments with ffmpeg.

Usage:
    python3 scripts/create_vest_overlay_video.py

Outputs:
    /home/sree/bs/argus_demo/000921246-vest-overlay.mp4

Then stream as RTSP:
    bash scripts/stream_as_rtsp.sh
"""

import os
import glob
import random
import subprocess
import tempfile
import math

from PIL import Image

# ── Paths ──────────────────────────────────────────────────────────────────────
VEST_DIR    = "/home/sree/bs/ml_training/model-training/vest_images/extracted/"
INPUT_VIDEO = "/home/sree/bs/argus_demo/000921246-shopping-area.mov"
OUTPUT_VIDEO = "/home/sree/bs/argus_demo/000921246-vest-overlay-kf.mp4"
TMP_DIR     = "/tmp/vest_overlay_work"

# ── Layout constants ───────────────────────────────────────────────────────────
VIDEO_W, VIDEO_H = 1920, 1080
PERSON_H = 380        # px — scaled height of each overlaid person
FLOOR_Y  = VIDEO_H - PERSON_H - 30   # y of person top-left   (≈670)

# Horizontal anchor points for persons (x of their left edge)
X_POSITIONS = [80, 350, 680, 1020, 1380, 1650]

# How many vest people visible at any moment and how many time segments
PEOPLE_PER_SEGMENT = 3
VIDEO_DURATION = 22.0   # seconds (from ffprobe)


# ── Step 1: Remove backgrounds ─────────────────────────────────────────────────

def remove_backgrounds(vest_dir: str, out_dir: str) -> list[str]:
    """Run rembg on every vest PNG and save as RGBA to out_dir."""
    try:
        from rembg import remove as rembg_remove
    except ImportError:
        raise SystemExit("rembg not installed — run: pip install rembg --break-system-packages")

    os.makedirs(out_dir, exist_ok=True)

    paths = sorted(glob.glob(os.path.join(vest_dir, "*.png")))
    if not paths:
        raise SystemExit(f"No PNG files found in {vest_dir}")

    print(f"[1/3] Removing backgrounds from {len(paths)} images …")
    out_paths = []
    for i, src in enumerate(paths):
        dst = os.path.join(out_dir, os.path.basename(src))
        if os.path.exists(dst):
            out_paths.append(dst)
            continue
        with open(src, "rb") as f:
            raw = f.read()
        result = rembg_remove(raw)

        # Verify we have real transparency (not all-white alpha)
        from io import BytesIO
        img = Image.open(BytesIO(result)).convert("RGBA")
        out_paths.append(dst)
        img.save(dst)

        if (i + 1) % 10 == 0 or (i + 1) == len(paths):
            print(f"   {i+1}/{len(paths)} done")

    print(f"   Transparent PNGs saved to {out_dir}")
    return out_paths


# ── Step 2: Scale PNGs to uniform height ──────────────────────────────────────

def scale_to_height(src: str, dst: str, target_h: int) -> None:
    """Scale image to target_h, preserve aspect ratio, keep RGBA."""
    img = Image.open(src).convert("RGBA")
    w, h = img.size
    new_w = max(1, int(w * target_h / h))
    img = img.resize((new_w, target_h), Image.LANCZOS)
    img.save(dst)


# ── Step 3: Build ffmpeg filter_complex ───────────────────────────────────────

def build_filter(scaled_paths: list[str], duration: float) -> tuple[list[str], str]:
    """
    Return (extra_ffmpeg_inputs, filter_complex_string).

    Groups the selected images into PEOPLE_PER_SEGMENT people per time segment.
    Each segment lasts approximately duration / n_segments seconds.
    """
    # Pick a representative subset: cycle across X_POSITIONS uniformly
    n_images = len(scaled_paths)
    n_segments = max(1, math.ceil(n_images / PEOPLE_PER_SEGMENT))
    seg_len = duration / n_segments

    groups = []  # list of list of (img_path, x_pos, y_pos)
    idx = 0
    for seg in range(n_segments):
        group = []
        for slot in range(PEOPLE_PER_SEGMENT):
            if idx >= n_images:
                break
            x = X_POSITIONS[slot % len(X_POSITIONS)]
            y = FLOOR_Y
            group.append((scaled_paths[idx], x, y))
            idx += 1
        if group:
            groups.append(group)

    # Build ffmpeg inputs and filter chain
    extra_inputs = []
    filter_parts = []
    cur_stream = "0:v"   # starts as the raw video
    input_idx = 1        # ffmpeg input index (0 = video, 1+ = images)

    for seg_i, group in enumerate(groups):
        t_start = seg_i * seg_len
        t_end   = (seg_i + 1) * seg_len
        enable  = f"between(t,{t_start:.2f},{t_end:.2f})"

        for img_path, x, y in group:
            label_in  = f"v_scale{input_idx}"
            label_out = f"v_ov{input_idx}"
            filter_parts.append(f"[{input_idx}:v]copy[{label_in}]")
            filter_parts.append(
                f"[{cur_stream}][{label_in}]overlay={x}:{y}:enable='{enable}'[{label_out}]"
            )
            extra_inputs.extend(["-i", img_path])
            cur_stream = label_out
            input_idx += 1

    filter_str = ";\n  ".join(filter_parts)
    return extra_inputs, filter_str, cur_stream


# ── Step 4: Encode with ffmpeg ─────────────────────────────────────────────────

def encode_video(input_video: str, extra_inputs: list[str],
                 filter_str: str, out_stream: str, output_path: str) -> None:
    cmd = [
        "ffmpeg", "-y",
        "-i", input_video,
        *extra_inputs,
        "-filter_complex", filter_str,
        "-map", f"[{out_stream}]",
        "-c:v", "libx264",
        "-preset", "fast",
        "-crf", "20",
        # Force an IDR keyframe every 30 frames (= 1 s at 30 fps).
        # This is essential for RTSP: clients that join mid-stream get a
        # decodable frame within 1 second instead of having to wait ~8 s.
        "-g", "30", "-keyint_min", "30", "-sc_threshold", "0",
        "-pix_fmt", "yuv420p",
        "-an",          # no audio
        output_path,
    ]
    print("[3/3] Running ffmpeg …")
    print("   ", " ".join(cmd[:8]), "… [truncated]")
    result = subprocess.run(cmd, capture_output=False)
    if result.returncode != 0:
        raise SystemExit(f"ffmpeg failed with code {result.returncode}")


# ── Main ──────────────────────────────────────────────────────────────────────

def main():
    random.seed(42)
    os.makedirs(TMP_DIR, exist_ok=True)

    # 1. Remove backgrounds → RGBA PNGs
    rgba_dir = os.path.join(TMP_DIR, "rgba")
    rgba_paths = remove_backgrounds(VEST_DIR, rgba_dir)

    # 2. Scale all to PERSON_H
    print(f"[2/3] Scaling {len(rgba_paths)} images to height={PERSON_H}px …")
    scaled_dir = os.path.join(TMP_DIR, "scaled")
    os.makedirs(scaled_dir, exist_ok=True)
    scaled_paths = []
    for p in rgba_paths:
        dst = os.path.join(scaled_dir, os.path.basename(p))
        if not os.path.exists(dst):
            scale_to_height(p, dst, PERSON_H)
        scaled_paths.append(dst)

    # Use all images; they get cycled across time segments
    extra_inputs, filter_str, out_stream = build_filter(scaled_paths, VIDEO_DURATION)

    # 3. Encode
    encode_video(INPUT_VIDEO, extra_inputs, filter_str, out_stream, OUTPUT_VIDEO)

    size_mb = os.path.getsize(OUTPUT_VIDEO) / 1024 / 1024
    print(f"\nDone! Output: {OUTPUT_VIDEO} ({size_mb:.1f} MB)")
    print("  Stream as RTSP:  bash scripts/stream_as_rtsp.sh")


if __name__ == "__main__":
    main()
