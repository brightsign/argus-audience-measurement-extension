#!/usr/bin/env python3
"""Dev-host only: export the MobileNetV2 embedding model to ONNX and write the model card.

Source weights: torchvision mobilenet_v2(weights=IMAGENET1K_V1)
  https://download.pytorch.org/models/mobilenet_v2-b0353104.pth  (ImageNet-1k, torchvision BSD-3-Clause code)
Graph: model.features -> mean over H,W -> flatten  (1280-d pooled vector, BEFORE dropout/classifier).
ONNX: opset 12, static input `input` [1,3,224,224] float32 NCHW (already normalised, no normalisation in graph),
      output `embedding` [1,D].
Not needed at normal build time (the .onnx/.rknn are committed).
"""
import hashlib, json, os, urllib.request
import numpy as np, torch, torch.nn as nn, torchvision
from torchvision.models import mobilenet_v2

HERE = os.path.dirname(os.path.abspath(__file__))
MODELS = os.path.normpath(os.path.join(HERE, "..", "models"))
URL = "https://download.pytorch.org/models/mobilenet_v2-b0353104.pth"
WEIGHTS_CACHE = os.path.join(os.path.expanduser("~"), ".cache", "visual-memory-weights")  # outside the repo
ONNX_PATH = os.path.join(MODELS, "mobilenet_v2_embedder.onnx")
CARD = os.path.join(MODELS, "mobilenet_embedder.json")


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


class Embedder(nn.Module):
    def __init__(self, m):
        super().__init__()
        self.features = m.features

    def forward(self, x):
        return self.features(x).mean(dim=(2, 3))  # [N,1280]


def load_torch_model():
    os.makedirs(WEIGHTS_CACHE, exist_ok=True)
    pth = os.path.join(WEIGHTS_CACHE, "mobilenet_v2-b0353104.pth")
    if not os.path.exists(pth):
        print("downloading", URL)
        urllib.request.urlretrieve(URL, pth)
    m = mobilenet_v2(weights=None)
    m.load_state_dict(torch.load(pth, map_location="cpu"))
    m.eval()
    return Embedder(m).eval(), pth


def load_card():
    return json.load(open(CARD)) if os.path.exists(CARD) else {}


def save_card(card):
    with open(CARD, "w") as f:
        json.dump(card, f, indent=2)
        f.write("\n")


def main():
    import onnx, onnxruntime as ort
    torch.manual_seed(0)
    os.makedirs(MODELS, exist_ok=True)
    model, pth = load_torch_model()
    dummy = torch.randn(1, 3, 224, 224)
    torch.onnx.export(model, dummy, ONNX_PATH, opset_version=12, input_names=["input"],
                      output_names=["embedding"], dynamo=False, do_constant_folding=True)
    om = onnx.load(ONNX_PATH)
    onnx.checker.check_model(om)
    out = om.graph.output[0]
    dims = [d.dim_value for d in out.type.tensor_type.shape.dim]
    inp = [d.dim_value for d in om.graph.input[0].type.tensor_type.shape.dim]
    print("onnx input", om.graph.input[0].name, inp, "output", out.name, dims)
    D = dims[-1]
    sess = ort.InferenceSession(ONNX_PATH, providers=["CPUExecutionProvider"])
    md = 0.0
    for _ in range(5):
        x = torch.randn(1, 3, 224, 224)
        with torch.no_grad():
            t = model(x).numpy()
        o = sess.run(None, {"input": x.numpy()})[0]
        md = max(md, float(np.abs(t - o).max()))
    print("torch vs onnxruntime max abs diff:", md)
    ws, os_ = sha256(pth), sha256(ONNX_PATH)
    print("weights_sha256", ws)
    print("onnx_sha256", os_)
    print("onnx size", os.path.getsize(ONNX_PATH))
    card = load_card()
    card.update(
        model_name="mobilenet_v2_embedder", model_version="tv-imagenet1k-v1",
        description="torchvision MobileNetV2 (ImageNet-1k) global-average-pooled features as a generic image embedding",
        source={"weights_url": URL, "code": "torchvision.models.mobilenet_v2 (BSD-3-Clause)",
                "weights": "MobileNet_V2_Weights.IMAGENET1K_V1, trained on ImageNet-1k"},
        licence="torchvision code BSD-3-Clause; weights as distributed by PyTorch (ImageNet-1k trained)",
        weights_sha256=ws, onnx_file="mobilenet_v2_embedder.onnx", onnx_sha256=os_,
        onnx={"opset": 12, "input_name": "input", "input_shape": inp, "input_layout": "NCHW",
              "output_name": "embedding", "output_shape": dims, "torch_vs_ort_max_abs_diff": md,
              "torch": torch.__version__, "torchvision": torchvision.__version__, "onnx": onnx.__version__},
        input={"size": [224, 224], "channels": "RGB", "onnx_layout": "NCHW", "rknn_layout": "NHWC",
               "dtype": "float32", "normalisation_inside_model": False},
        preprocessing={"id": "pp1",
                       "resize": "whole image -> 224x224, no crop, aspect NOT preserved; separable antialiased bilinear (triangle, support 1*filterscale) per Pillow precompute_coeffs, float32, no 8-bit rounding; horizontal pass then vertical",
                       "mean": [0.485, 0.456, 0.406], "std": [0.229, 0.224, 0.225],
                       "formula": "x=(v/255-mean[c])/std[c], RGB"},
        output={"dim": D, "post": "flatten, L2-normalise (reject NaN/Inf/norm<1e-6); similarity = dot product"},
    )
    card.setdefault("rknn_sha256", {})
    card.setdefault("variants", {})
    save_card(card)
    print("wrote", CARD)


if __name__ == "__main__":
    main()
