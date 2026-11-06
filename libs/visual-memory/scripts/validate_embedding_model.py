#!/usr/bin/env python3
"""Validate the embedding model pipeline; writes models/validation_results.{json,md}.

  validate_embedding_model.py [--no-rknn]
RKNN checks use the rknn-toolkit2 x86 simulator (init_runtime() with no target) on a model built in-memory
from the ONNX (same config as convert_rknn.py); load_rknn() models cannot run on the simulator.
"""
import argparse, glob, io, json, os, random, sys, time
import numpy as np
from PIL import Image, ImageEnhance, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import reference_embedder as ref  # noqa: E402
import convert_rknn as cv  # noqa: E402

MODELS = ref.MODELS


def transforms(im):
    w, h = im.size
    cw, ch = int(w * 0.9), int(h * 0.9)
    l, t = (w - cw) // 2, (h - ch) // 2
    buf = io.BytesIO()
    im.save(buf, "JPEG", quality=30)
    buf.seek(0)
    return {
        "flip": im.transpose(Image.FLIP_LEFT_RIGHT),
        "bright+20": ImageEnhance.Brightness(im).enhance(1.2),
        "bright-20": ImageEnhance.Brightness(im).enhance(0.8),
        "crop90": im.crop((l, t, l + cw, t + ch)),
        "blur": im.filter(ImageFilter.GaussianBlur(radius=2)),
        "jpeg_q30": Image.open(buf).convert("RGB"),
    }


def stats(v):
    v = np.asarray(v, float)
    return {"n": int(v.size), "min": float(v.min()), "mean": float(v.mean()), "max": float(v.max())}


def retrieval(E, groups):
    """E: [N,D] L2-normalised. groups: list of group ids. Query every item against all others (excl. self)."""
    S = E @ E.T
    np.fill_diagonal(S, -np.inf)
    top1 = top5 = 0
    prec5 = []
    for i in range(len(E)):
        idx = np.argsort(-S[i])[:5]
        same = [groups[j] == groups[i] for j in idx]
        top1 += same[0]
        top5 += any(same)
        prec5.append(np.mean(same))
    n = len(E)
    return {"queries": n, "top1_hit": top1 / n, "top5_any_hit": top5 / n, "top5_precision": float(np.mean(prec5))}


def topk_sets(E, k=5):
    S = E @ E.T
    np.fill_diagonal(S, -np.inf)
    return [set(np.argsort(-S[i])[:k].tolist()) for i in range(len(E))]


def overlap(Ea, Eb, k=5):
    A, B = topk_sets(Ea, k), topk_sets(Eb, k)
    return float(np.mean([len(a & b) / k for a, b in zip(A, B)]))


def run_rknn(variant, tensors_nhwc, workdir):
    from rknn.api import RKNN
    quant = variant == "int8"
    r = RKNN(verbose=False)
    assert r.config(mean_values=[[0, 0, 0]], std_values=[[1, 1, 1]], target_platform="rk3588", optimization_level=3,
                    **(dict(quantized_dtype="asymmetric_quantized-8", quantized_algorithm="normal",
                            quantized_method="channel") if quant else {})) == 0
    assert r.load_onnx(model=ref.ONNX_PATH, inputs=["input"], input_size_list=[[1, 3, 224, 224]]) == 0
    ds = cv.build_calibration(os.path.join(workdir, "calib"))[0] if quant else None
    assert r.build(do_quantization=quant, dataset=ds) == 0
    rc = r.init_runtime()  # no target -> x86 simulator
    if rc != 0:
        raise RuntimeError(f"init_runtime failed rc={rc}")
    out = []
    for t in tensors_nhwc:
        o = r.inference(inputs=[t[None].astype(np.float32)], data_format=["nhwc"])
        out.append(np.asarray(o[0], np.float32).reshape(-1))
    r.release()
    return np.stack(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-rknn", action="store_true")
    ap.add_argument("--workdir", default=cv.WORK)
    a = ap.parse_args()
    random.seed(0); np.random.seed(0)
    os.makedirs(a.workdir, exist_ok=True)
    res = {"tools": {"numpy": np.__version__}}

    paths = cv.calib_images()
    names, groups, imgs, kinds = [], [], [], []
    for gi, p in enumerate(paths):
        im = Image.open(p).convert("RGB")
        names.append(os.path.basename(p)); groups.append(gi); kinds.append("base"); imgs.append(np.asarray(im, np.uint8))
        for k, t in transforms(im).items():
            names.append(os.path.basename(p) + ":" + k); groups.append(gi); kinds.append(k)
            imgs.append(np.asarray(t, np.uint8))
    N = len(imgs)
    print("images:", len(paths), "bases;", N, "total")
    res["dataset"] = {"bases": len(paths), "total_with_transforms": N,
                      "transforms": sorted(set(kinds) - {"base"}),
                      "note": "public ImageNet-val samples, COCO subset, Argus artwork"}

    tens = [ref.preprocess_array(x) for x in imgs]
    # (a) ORT vs torch
    import torch
    from export_embedding_model import load_torch_model
    tm, _ = load_torch_model()
    E_ort = np.stack([ref.l2_normalise(ref.embed_tensor(t)) for t in tens])
    with torch.no_grad():
        X = torch.from_numpy(np.stack([np.transpose(t, (2, 0, 1)) for t in tens]))
        T = tm(X).numpy()
    T = T / np.linalg.norm(T, axis=1, keepdims=True)
    cos_ot = (E_ort * T).sum(1)
    res["a_ort_vs_torch"] = {"cos": stats(cos_ot), "max_abs_diff": float(np.abs(E_ort - T).max()),
                             "pass": bool(cos_ot.min() > 0.99999)}
    # (b) determinism
    e1 = ref.embed_array(imgs[0]); e2 = ref.embed_array(imgs[0].copy())
    res["b_identical_input"] = {"bitwise_equal": bool(np.array_equal(e1, e2))}
    # (c) norms
    nr = np.linalg.norm(E_ort, axis=1)
    res["c_l2_norms"] = {"min": float(nr.min()), "max": float(nr.max()), "pass": bool(np.abs(nr - 1).max() < 1e-5)}
    # (d) similarity distributions
    S = E_ort @ E_ort.T
    g = np.array(groups); isbase = np.array([k == "base" for k in kinds])
    same_t = {}
    for k in sorted(set(kinds) - {"base"}):
        vals = [S[i, j] for i in range(N) if kinds[i] == "base" for j in range(N) if g[j] == g[i] and kinds[j] == k]
        same_t[k] = stats(vals)
    allsame = [S[i, j] for i in range(N) if kinds[i] == "base" for j in range(N) if g[j] == g[i] and kinds[j] != "base"]
    diff_bb = S[np.ix_(np.where(isbase)[0], np.where(isbase)[0])]
    diff_bb = diff_bb[~np.eye(len(diff_bb), dtype=bool)]
    diff_bt = [S[i, j] for i in range(N) if kinds[i] == "base" for j in range(N) if g[j] != g[i] and kinds[j] != "base"]
    res["d_cosine_distribution"] = {"same_image_identical": 1.0,
                                    "base_vs_own_transform_all": stats(allsame), "base_vs_own_transform_by_type": same_t,
                                    "different_images_base_vs_base": stats(diff_bb),
                                    "different_images_base_vs_other_transforms": stats(diff_bt)}
    # (e) retrieval
    res["e_retrieval_ort"] = retrieval(E_ort, groups)
    # (f) invalid inputs
    def raises(fn):
        try:
            fn()
        except Exception as e:  # noqa
            return type(e).__name__
        return None
    bad = {
        "nan_float_image": raises(lambda: ref.embed_array(np.full((32, 32, 3), np.nan, np.float32))),
        "nan_tensor": raises(lambda: ref.l2_normalise(ref.embed_tensor(np.full((224, 224, 3), np.nan, np.float32)))),
        "zero_by_zero": raises(lambda: ref.embed_array(np.zeros((0, 0, 3), np.uint8))),
        "wrong_dtype_float32": raises(lambda: ref.embed_array(np.zeros((8, 8, 3), np.float32))),
        "wrong_channels": raises(lambda: ref.embed_array(np.zeros((8, 8, 4), np.uint8))),
        "zero_vector_l2": raises(lambda: ref.l2_normalise(np.zeros(1280, np.float32))),
    }
    res["f_invalid_inputs"] = {"raised": bad, "pass": all(v is not None for v in bad.values())}

    # (g) RKNN simulator
    res["g_rknn_simulator"] = {}
    if not a.no_rknn:
        import importlib.metadata as md
        res["tools"]["rknn-toolkit2"] = md.version("rknn-toolkit2")
        for variant in ("fp16", "int8"):
            t0 = time.time()
            try:
                R = run_rknn(variant, tens, a.workdir)
                R = np.stack([ref.l2_normalise(r) for r in R])
                c = (R * E_ort).sum(1)
                res["g_rknn_simulator"][variant] = {
                    "cos_vs_ort": stats(c), "max_abs_diff_l2normed": float(np.abs(R - E_ort).max()),
                    "mean_abs_diff_l2normed": float(np.abs(R - E_ort).mean()),
                    "retrieval": retrieval(R, groups),
                    "top5_overlap_vs_ort": overlap(R, E_ort), "seconds": round(time.time() - t0, 1)}
            except Exception as e:  # noqa
                res["g_rknn_simulator"][variant] = {"error": f"{type(e).__name__}: {str(e)[:600]}"}
            print(variant, json.dumps(res["g_rknn_simulator"][variant])[:300])
    else:
        res["g_rknn_simulator"] = {"skipped": True}

    with open(os.path.join(MODELS, "validation_results.json"), "w") as f:
        json.dump(res, f, indent=2); f.write("\n")
    write_md(res)
    print("wrote validation_results.json/.md")


def fmt(s):
    return f"{s['min']:.4f} / {s['mean']:.4f} / {s['max']:.4f}"


def write_md(r):
    L = ["# Validation results", "",
         f"Dataset: {r['dataset']['bases']} public base images x ({len(r['dataset']['transforms'])} transforms) = "
         f"{r['dataset']['total_with_transforms']} images. Transforms: {', '.join(r['dataset']['transforms'])}.",
         f"Tools: {r['tools']}", ""]
    a = r["a_ort_vs_torch"]
    L += ["## (a)-(c) correctness", "", "| check | result |", "|---|---|",
          f"| ORT vs torch cosine min/mean/max | {fmt(a['cos'])} (pass={a['pass']}) |",
          f"| ORT vs torch max abs diff (L2-normed) | {a['max_abs_diff']:.2e} |",
          f"| identical image -> bitwise-identical embedding | {r['b_identical_input']['bitwise_equal']} |",
          f"| L2 norm min/max | {r['c_l2_norms']['min']:.7f} / {r['c_l2_norms']['max']:.7f} (pass={r['c_l2_norms']['pass']}) |", ""]
    d = r["d_cosine_distribution"]
    L += ["## (d) cosine similarity (min / mean / max)", "", "| group | n | min / mean / max |", "|---|---|---|"]
    L.append(f"| same image (identical) | - | 1.0000 |")
    L.append(f"| base vs own transforms (all) | {d['base_vs_own_transform_all']['n']} | {fmt(d['base_vs_own_transform_all'])} |")
    for k, v in d["base_vs_own_transform_by_type"].items():
        L.append(f"| &nbsp;&nbsp;{k} | {v['n']} | {fmt(v)} |")
    L.append(f"| different images, base vs base | {d['different_images_base_vs_base']['n']} | {fmt(d['different_images_base_vs_base'])} |")
    L.append(f"| different images, base vs other-image transforms | {d['different_images_base_vs_other_transforms']['n']} | {fmt(d['different_images_base_vs_other_transforms'])} |")
    e = r["e_retrieval_ort"]
    L += ["", "## (e) retrieval (ORT FP32), every image queried against all others excluding itself", "",
          "| top-1 is same-group | top-5 contains same-group | top-5 precision |", "|---|---|---|",
          f"| {e['top1_hit']:.3f} | {e['top5_any_hit']:.3f} | {e['top5_precision']:.3f} |", ""]
    f = r["f_invalid_inputs"]
    L += ["## (f) invalid inputs (exception raised)", "", "| case | exception |", "|---|---|"]
    L += [f"| {k} | {v} |" for k, v in f["raised"].items()]
    L += ["", "## (g) RKNN simulator (x86, rk3588 target config) vs ORT", ""]
    g = r["g_rknn_simulator"]
    if g.get("skipped"):
        L.append("skipped")
    else:
        L += ["| variant | cos min / mean / max | max abs diff | top-5 overlap vs ORT | top-1 same-group | top-5 any | top-5 precision |",
              "|---|---|---|---|---|---|---|"]
        for k, v in g.items():
            if "error" in v:
                L.append(f"| {k} | ERROR: {v['error']} | | | | | |")
            else:
                rt = v["retrieval"]
                L.append(f"| {k} | {fmt(v['cos_vs_ort'])} | {v['max_abs_diff_l2normed']:.4f} | {v['top5_overlap_vs_ort']:.3f} | "
                         f"{rt['top1_hit']:.3f} | {rt['top5_any_hit']:.3f} | {rt['top5_precision']:.3f} |")
    L += ["", "Note: simulator results approximate, not bit-exact with, NPU hardware (esp. INT8). Hardware numbers must be checked on device.", ""]
    open(os.path.join(MODELS, "validation_results.md"), "w").write("\n".join(L))


if __name__ == "__main__":
    main()
