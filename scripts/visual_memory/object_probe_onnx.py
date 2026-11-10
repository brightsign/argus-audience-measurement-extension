#!/usr/bin/env python3
"""Host-side feasibility probe for D4 automatic object indexing (development tool, not shipped).

Runs the FLOAT YOLOX-S ONNX of the rknn_model_zoo (the model the packaged yolox_s.rknn is converted from) on stored
camera JPEGs with the same letterbox / decode / per-class NMS as the zoo, at several worker-side thresholds, and prints
the detections of interest. It is an UPPER BOUND for the INT8 RKNN model on the player: it says whether the YOLOX-S
architecture can find the object at all, not what the quantised model does on the NPU. Do not commit frames.

usage: object_probe_onnx.py --onnx yolox_s.onnx --out DIR --thresholds 0.5,0.3,0.25 frame1.jpg frame2.jpg ...
"""
import argparse
import json
import os
import time

import numpy as np
import onnxruntime as ort
from PIL import Image, ImageDraw

LABELS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "install", "RK3588", "model", "coco_80_labels_list.txt")
COCO = [line.strip() for line in open(LABELS)]
SIZE = 640


def letterbox(img):
    w, h = img.size
    s = min(SIZE / w, SIZE / h)
    nw, nh = int(round(w * s)), int(round(h * s))
    pad_x, pad_y = (SIZE - nw) // 2, (SIZE - nh) // 2
    canvas = Image.new("RGB", (SIZE, SIZE), (114, 114, 114))
    canvas.paste(img.resize((nw, nh), Image.BILINEAR), (pad_x, pad_y))
    return canvas, s, pad_x, pad_y


def decode(outs):
    boxes, objs, cls = [], [], []
    for o in outs:
        o = o.reshape(1, 85, o.shape[-2], o.shape[-1])
        gh, gw = o.shape[2:]
        col, row = np.meshgrid(np.arange(gw), np.arange(gh))
        stride = SIZE // gh
        xy = (o[0, :2] + np.stack([col, row])) * stride
        wh = np.exp(o[0, 2:4]) * stride
        b = np.concatenate([xy - wh / 2, xy + wh / 2], axis=0)  # x1 y1 x2 y2
        boxes.append(b.reshape(4, -1).T)
        objs.append(o[0, 4].reshape(-1))
        cls.append(o[0, 5:].reshape(80, -1).T)
    return np.concatenate(boxes), np.concatenate(objs), np.concatenate(cls)


def nms(b, s, thr=0.45):
    order = s.argsort()[::-1]
    keep = []
    while order.size:
        i = order[0]
        keep.append(i)
        xx1 = np.maximum(b[i, 0], b[order[1:], 0])
        yy1 = np.maximum(b[i, 1], b[order[1:], 1])
        xx2 = np.minimum(b[i, 2], b[order[1:], 2])
        yy2 = np.minimum(b[i, 3], b[order[1:], 3])
        inter = np.maximum(0, xx2 - xx1) * np.maximum(0, yy2 - yy1)
        ai = (b[i, 2] - b[i, 0]) * (b[i, 3] - b[i, 1])
        ao = (b[order[1:], 2] - b[order[1:], 0]) * (b[order[1:], 3] - b[order[1:], 1])
        order = order[np.where(inter / (ai + ao - inter + 1e-9) <= thr)[0] + 1]
    return keep


def detect(sess, img, floor=0.05):
    lb, s, px, py = letterbox(img)
    x = np.asarray(lb, dtype=np.float32).transpose(2, 0, 1)[None]
    t = time.perf_counter()
    outs = sess.run(None, {sess.get_inputs()[0].name: x})
    ms = (time.perf_counter() - t) * 1000
    boxes, obj, cls = decode(outs)
    score = obj[:, None] * cls  # per class score
    res = []
    for c in np.unique(np.where(score >= floor)[1]):
        idx = np.where(score[:, c] >= floor)[0]
        for k in nms(boxes[idx], score[idx, c]):
            i = idx[k]
            b = boxes[i]
            x1, y1, x2, y2 = (b[0] - px) / s, (b[1] - py) / s, (b[2] - px) / s, (b[3] - py) / s
            res.append({"class_id": int(c), "class": COCO[c], "confidence": round(float(score[i, c]), 4),
                        "box": [int(max(0, x1)), int(max(0, y1)), int(min(img.width, x2)), int(min(img.height, y2))]})
    res.sort(key=lambda d: -d["confidence"])
    return res, ms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--onnx", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--thresholds", default="0.5,0.3,0.25")
    ap.add_argument("--classes", default="bottle,cup,wine_glass,laptop,cell_phone,book")
    ap.add_argument("frames", nargs="+")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    sess = ort.InferenceSession(a.onnx, providers=["CPUExecutionProvider"])
    want = {COCO.index(c) for c in a.classes.split(",")}
    thr = [float(t) for t in a.thresholds.split(",")]
    report = []
    for f in a.frames:
        img = Image.open(f).convert("RGB")
        dets, ms = detect(sess, img)
        row = {"frame": os.path.basename(f), "size": list(img.size), "latency_ms_cpu_float": round(ms, 1), "thresholds": {}}
        for t in thr:
            row["thresholds"][str(t)] = [d for d in dets if d["confidence"] >= t and d["class_id"] in want]
        row["all_top"] = dets[:8]
        report.append(row)
        im = img.copy()
        dr = ImageDraw.Draw(im)
        for d in dets:
            if d["confidence"] >= min(thr) and d["class_id"] in want:
                dr.rectangle(d["box"], outline=(255, 0, 0), width=2)
                dr.text((d["box"][0] + 2, d["box"][1] + 2), f'{d["class"]} {d["confidence"]:.2f}', fill=(255, 255, 0))
        im.save(os.path.join(a.out, os.path.splitext(os.path.basename(f))[0] + "_annotated.png"))
    print(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
