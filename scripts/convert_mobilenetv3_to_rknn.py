#!/usr/bin/env python3
"""
Convert trained MobileNetV3-Small vest classifier to RKNN INT8.

Usage:
    python3 scripts/convert_mobilenetv3_to_rknn.py \
        --pth /home/sree/bs/ml_training/model-training/models/finetuned/best_model.pth \
        --rknn install/rk3588_linux_aarch64/lib/Mobilenetv3_small.rknn \
        [--onnx /tmp/mobilenetv3_vest.onnx] \
        [--dataset dataset_path/] \
        [--target rk3588|rk3576|rk3568]

Requirements:
    pip install torch torchvision timm onnx onnxscript onnxruntime rknn-toolkit2
    pip install "protobuf>=4.21.6,<=4.25.4"   # rknn-toolkit2 2.3.0 constraint
    (rknn-toolkit2 must match your board SDK version)

Notes:
    The model uses timm's mobilenetv3_small_100 (key 'conv_stem'), NOT torchvision.
    rknn.build() requires a text-file dataset (one image path per line, images
    pre-resized to 224×224), NOT an in-memory list.

Classes:
    0 = no_vest
    1 = employee_vest
"""

import argparse
import os
import sys
import numpy as np

# --------------------------------------------------------------------------- #
# Argument parsing
# --------------------------------------------------------------------------- #

def parse_args():
    p = argparse.ArgumentParser(description="PyTorch MobileNetV3-Small → ONNX → RKNN INT8")
    p.add_argument("--pth",    required=True,
                   help="Path to best_model.pth (MobileNetV3-Small, 2 classes)")
    p.add_argument("--rknn",   required=True,
                   help="Output .rknn file path")
    p.add_argument("--onnx",   default="/tmp/mobilenetv3_vest.onnx",
                   help="Intermediate ONNX file (default /tmp/mobilenetv3_vest.onnx)")
    p.add_argument("--dataset", default=None,
                   help="Directory with representative images for INT8 calibration "
                        "(JPEG/PNG, any size — will be auto-resized to 224×224). "
                        "If not provided, random noise is used (lower accuracy).")
    p.add_argument("--target", default="rk3588",
                   choices=["rk3588", "rk3576", "rk3568", "rk3562"],
                   help="Target SoC (default: rk3588 for XT5)")
    p.add_argument("--n-classes", type=int, default=2,
                   help="Number of output classes (default: 2)")
    return p.parse_args()


# --------------------------------------------------------------------------- #
# Step 1 – Export PyTorch → ONNX
# --------------------------------------------------------------------------- #

def export_to_onnx(pth_path: str, onnx_path: str, n_classes: int) -> None:
    """Load the finetuned MobileNetV3-Small (timm) and export it to ONNX."""
    try:
        import torch
    except ImportError:
        sys.exit("ERROR: torch not installed. Run: pip install torch")
    try:
        import timm
    except ImportError:
        sys.exit("ERROR: timm not installed. Run: pip install timm")

    print(f"\n[1/3] Loading PyTorch model from {pth_path}")

    # The training used timm's mobilenetv3_small_100 (keys: conv_stem, bn1, blocks, ...)
    # NOT torchvision (which uses keys: features, classifier).
    model = timm.create_model("mobilenetv3_small_100", pretrained=False, num_classes=n_classes)

    # Load weights — support raw state_dict or checkpoint dict
    state = torch.load(pth_path, map_location="cpu")
    if isinstance(state, dict) and "state_dict" in state:
        state = state["state_dict"]
    elif isinstance(state, dict) and "model_state_dict" in state:
        state = state["model_state_dict"]

    model.load_state_dict(state, strict=True)
    model.eval()

    # Dummy input: 1 × 3 × 224 × 224 (ImageNet standard)
    dummy = torch.zeros(1, 3, 224, 224)

    print(f"[1/3] Exporting to ONNX: {onnx_path}")
    torch.onnx.export(
        model,
        dummy,
        onnx_path,
        opset_version=18,
        input_names=["input"],
        output_names=["logits"],
        dynamic_axes=None,      # Fixed batch=1 for embedded deployment
        do_constant_folding=True,
        export_params=True,
    )

    # Verify
    try:
        import onnx
        m = onnx.load(onnx_path)
        onnx.checker.check_model(m)
        print(f"[1/3] ONNX model validated OK — {onnx_path}")
    except ImportError:
        print("[1/3] WARNING: 'onnx' package not installed, skipping validation")

    print(f"[1/3] Done — {onnx_path}")


# --------------------------------------------------------------------------- #
# Step 2 – Build calibration dataset
# --------------------------------------------------------------------------- #

def build_calibration_dataset(dataset_dir: str | None, n: int = 60,
                               tmp_dir: str = "/tmp/vest_cal_images") -> str:
    """
    Pre-resize up to *n* calibration images to 224×224, write them to *tmp_dir*,
    and return the path to a text file listing one image path per line.

    rknn.build() requires a .txt dataset file — it does NOT accept in-memory arrays.
    If dataset_dir is None or empty, random noise PNGs are generated instead.
    """
    try:
        from PIL import Image
    except ImportError:
        sys.exit("ERROR: Pillow not installed. Run: pip install Pillow")

    os.makedirs(tmp_dir, exist_ok=True)
    out_paths = []

    if dataset_dir and os.path.isdir(dataset_dir):
        import glob, random
        exts = ("*.jpg", "*.jpeg", "*.png", "*.bmp", "*.webp")
        paths = []
        for ext in exts:
            paths.extend(glob.glob(os.path.join(dataset_dir, "**", ext), recursive=True))

        if not paths:
            print(f"[2/3] WARNING: No images found in {dataset_dir}, using random noise")
        else:
            random.shuffle(paths)
            for i, p in enumerate(paths[:n]):
                out = os.path.join(tmp_dir, f"cal_{i:04d}.png")
                Image.open(p).convert("RGB").resize((224, 224)).save(out)
                out_paths.append(out)
            print(f"[2/3] {len(out_paths)} calibration images resized → {tmp_dir}")

    if not out_paths:
        print("[2/3] Generating random-noise calibration images (lower quantization quality)")
        for i in range(n):
            arr = np.random.randint(0, 256, (224, 224, 3), dtype=np.uint8)
            out = os.path.join(tmp_dir, f"noise_{i:04d}.png")
            Image.fromarray(arr).save(out)
            out_paths.append(out)

    txt_path = os.path.join(tmp_dir, "dataset.txt")
    with open(txt_path, "w") as f:
        f.write("\n".join(out_paths) + "\n")
    print(f"[2/3] Dataset list → {txt_path}")
    return txt_path


# --------------------------------------------------------------------------- #
# Step 3 – Convert ONNX → RKNN INT8
# --------------------------------------------------------------------------- #

def convert_to_rknn(onnx_path: str, rknn_path: str,
                    dataset_txt: str, target: str) -> None:
    """Convert ONNX to RKNN INT8 with quantization."""
    try:
        from rknn.api import RKNN
    except ImportError:
        sys.exit(
            "ERROR: rknn-toolkit2 not installed.\n"
            "Install guide: https://github.com/rockchip-linux/rknn-toolkit2"
        )

    print(f"\n[3/3] Converting ONNX → RKNN (target={target})")

    rknn = RKNN(verbose=False)

    # ------------------------------------------------------------------ #
    # Configure: ImageNet normalisation baked into the RKNN graph so the
    # C++ runtime can pass raw uint8 RGB without any pre-processing.
    #
    #   normalised = (raw_uint8 / 255 - mean) / std
    #   → mean_values = mean * 255
    #   → std_values  = std  * 255
    # ------------------------------------------------------------------ #
    IMAGENET_MEAN = [0.485 * 255, 0.456 * 255, 0.406 * 255]
    IMAGENET_STD  = [0.229 * 255, 0.224 * 255, 0.225 * 255]

    ret = rknn.config(
        mean_values=[IMAGENET_MEAN],   # per-channel mean (R,G,B)
        std_values=[IMAGENET_STD],     # per-channel std  (R,G,B)
        target_platform=target,
        quantized_algorithm="normal",
        quantized_method="channel",
        optimization_level=3,
    )
    if ret != 0:
        sys.exit(f"rknn.config() failed: {ret}")

    # Load ONNX — do NOT pass inputs= here; rknn infers them automatically
    ret = rknn.load_onnx(model=onnx_path)
    if ret != 0:
        sys.exit(f"rknn.load_onnx() failed: {ret}")

    # Build with INT8 quantization
    # dataset must be a .txt file path (one 224×224 image per line)
    print("[3/3] Building INT8 model (quantization calibration) …")
    ret = rknn.build(do_quantization=True, dataset=dataset_txt)
    if ret != 0:
        sys.exit(f"rknn.build() failed: {ret}")

    # Export
    os.makedirs(os.path.dirname(os.path.abspath(rknn_path)), exist_ok=True)
    ret = rknn.export_rknn(rknn_path)
    if ret != 0:
        sys.exit(f"rknn.export_rknn() failed: {ret}")

    print(f"[3/3] Saved RKNN model → {rknn_path}")

    # Quick accuracy check with a zero image
    print("[3/3] Running sanity inference on a black frame …")
    ret = rknn.init_runtime()
    if ret == 0:
        dummy = np.zeros((1, 224, 224, 3), dtype=np.uint8)
        outputs = rknn.inference(inputs=[dummy])
        if outputs and outputs[0] is not None:
            logits = outputs[0][0]
            # Softmax
            exp = np.exp(logits - logits.max())
            probs = exp / exp.sum()
            print(f"[3/3] Sanity check probs: no_vest={probs[0]:.3f}, employee_vest={probs[1]:.3f}")
        else:
            print("[3/3] WARNING: sanity inference returned no output")
    else:
        print(f"[3/3] WARNING: could not init simulator runtime (ret={ret}), skipping sanity check")

    rknn.release()
    print(f"\n✓ Conversion complete: {rknn_path}")


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #

def main():
    args = parse_args()

    # Step 1: PyTorch → ONNX
    export_to_onnx(args.pth, args.onnx, args.n_classes)

    # Step 2: Calibration dataset (returns path to .txt file)
    dataset_txt = build_calibration_dataset(args.dataset)

    # Step 3: ONNX → RKNN
    convert_to_rknn(args.onnx, args.rknn, dataset_txt, args.target)


if __name__ == "__main__":
    main()
