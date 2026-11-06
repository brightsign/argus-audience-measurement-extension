#!/usr/bin/env python3
"""D4 object-search evaluation against a running vm_api_server / dashboard (objects enabled).

  eval_object_search.py BASE DATASET_DIR [--origin URL] [--jitter 0.06] [--unseen 4] [--scale 100,1000,10000] [--out result.json]

DATASET_DIR is the output of make_object_scenes.py; its frames must already be ingested into the store the server reads, in
manifest order (vm_ingest_dir does that), and the object index must be EMPTY (use a fresh store).

Protocol (instance-level retrieval, held-out frames):
  * even-numbered frames are indexed (every labelled object, ROI = ground-truth box with +-jitter*size random edge error,
    emulating a hand-drawn rectangle); the last --unseen instances are never indexed ("unseen" objects);
  * odd-numbered frames are queries (the same jittered ROI protocol). Relevant = indexed regions of the SAME instance;
    class-level relevance = same class (cup/bottle/box/can);
  * metrics: P@1, P@5, hit@5, recall@5/@10, MRR (instance); class P@1, P@5; unseen-object behaviour;
  * ROI/crop accuracy: the server crop of a ground-truth box vs the same crop cut locally from the stored frame;
  * latency at the requested index sizes (extra jittered regions are added until the size is reached).
Scores are cosine similarities of image embeddings; nothing here supports a claim about general object recognition.
"""
import argparse, io, json, random, statistics, sys, time, urllib.error, urllib.request

ap = argparse.ArgumentParser()
ap.add_argument("base"); ap.add_argument("dataset"); ap.add_argument("--origin", default=None)
ap.add_argument("--jitter", type=float, default=0.06); ap.add_argument("--unseen", type=int, default=4)
ap.add_argument("--scale", default=""); ap.add_argument("--scale-only", action="store_true"); ap.add_argument("--out", default=None); ap.add_argument("--seed", type=int, default=7)
A = ap.parse_args()
BASE = A.base.rstrip("/"); ORIGIN = A.origin or BASE
rng = random.Random(A.seed)


def call(method, path, body=None, raw_ok=False):
    data = json.dumps(body).encode() if body is not None else None
    h = {"Content-Type": "application/json", "Origin": ORIGIN} if data is not None else {}
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=h)
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            b = r.read(); ct = r.headers.get("Content-Type", "")
            return r.status, (b if raw_ok or "json" not in ct else json.loads(b)), (time.perf_counter() - t0) * 1000
    except urllib.error.HTTPError as e:
        b = e.read(); ct = e.headers.get("Content-Type", "")
        return e.code, (json.loads(b) if "json" in ct else b), (time.perf_counter() - t0) * 1000


def pct(v, p):
    v = sorted(v); return v[min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))] if v else float("nan")


man = json.load(open(A.dataset + "/manifest.json"))
W, H = man["width"], man["height"]
st, fr, _ = call("GET", "/api/vm/frames?limit=100&offset=0")
total = fr["total"]; frames = []
off = 0
while off < total:
    st, fr, _ = call("GET", "/api/vm/frames?limit=100&offset=%d" % off); frames += fr["frames"]; off += 100
frames.sort(key=lambda f: (f["capture_utc_ms"], f["id"]))
assert len(frames) >= len(man["frames"]), "store has fewer frames than the manifest"
fid = {i: frames[i]["id"] for i in range(len(man["frames"]))}
st, s, _ = call("GET", "/api/vm/status")
assert s["objects"]["enabled"] and s["objects"]["regions"] == 0, "object index must be enabled and empty: %s" % s["objects"]
FW, FH = frames[0]["width"], frames[0]["height"]
insts = list(man["instances"]); unseen = set(insts[-A.unseen:]) if A.unseen else set()


def jitter_box(b, j):
    x, y, w, h = b
    dx0, dx1, dy0, dy1 = (rng.uniform(-j, j) * w, rng.uniform(-j, j) * w, rng.uniform(-j, j) * h, rng.uniform(-j, j) * h)
    x0, y0, x1, y1 = max(0, x + dx0), max(0, y + dy0), min(FW, x + w + dx1), min(FH, y + h + dy1)
    return [int(x0), int(y0), max(16, int(x1 - x0)), max(16, int(y1 - y0))]


def clip_roi(r):
    x, y, w, h = r; x = min(max(0, x), FW - 16); y = min(max(0, y), FH - 16)
    return [x, y, min(w, FW - x), min(h, FH - y)]


def run_scale(count, targets):
    scale = {}
    objs = [(i, o) for i, f in enumerate(man["frames"]) for o in f["objects"]]
    qs = [(i, o) for i, f in enumerate(man["frames"]) if i % 2 for o in f["objects"]][:40]
    for target in targets:
        t0 = time.time()
        while count < target:
            i, o = rng.choice(objs); b = jitter_box(o["bbox"], 0.25); x, y, w, h = clip_roi(b)
            st, r, _ = call("POST", "/api/vm/objects", {"frame_id": fid[i], "x": x, "y": y, "w": w, "h": h})
            if st == 201: count += 1
        scan, tot, cli, scanned = [], [], [], 0
        for i, o in qs:                                   # ROI queries: crop + NPU embedding + scan
            x, y, w, h = clip_roi(o["bbox"])
            st, r, ms = call("POST", "/api/vm/objects/search", {"frame_id": fid[i], "x": x, "y": y, "w": w, "h": h, "top_k": 5})
            if st == 200: scan.append(r["status"]["elapsed_ms"]); tot.append(r["status"].get("total_ms", float("nan"))); cli.append(ms); scanned = r["status"]["candidates_scanned"]
        rscan = []
        st, lst, _ = call("GET", "/api/vm/objects?limit=40")
        for ob in lst["objects"]:                         # stored-region queries: scan only, no inference
            st, r, ms = call("POST", "/api/vm/objects/search", {"region_id": ob["region_id"], "top_k": 5})
            if st == 200: rscan.append(r["status"]["elapsed_ms"])
        scale[str(target)] = {"regions": count, "scanned": scanned,
                              "roi_query": {"scan_p50": round(pct(scan, 50), 1), "scan_p95": round(pct(scan, 95), 1), "server_total_p50": round(pct(tot, 50), 1), "server_total_p95": round(pct(tot, 95), 1),
                                            "client_p50": round(pct(cli, 50), 1), "client_p95": round(pct(cli, 95), 1), "n": len(scan)},
                              "region_query_scan": {"p50": round(pct(rscan, 50), 1), "p95": round(pct(rscan, 95), 1), "max": round(max(rscan), 1), "n": len(rscan)},
                              "seconds_to_reach": round(time.time() - t0)}
        print("scale", target, scale[str(target)], flush=True)
    return scale


if A.scale_only:
    res = {"latency_vs_index_size": run_scale(call("GET", "/api/vm/status")[1]["objects"]["regions"], [int(x) for x in A.scale.split(",")])}
    print(json.dumps(res, indent=1))
    if A.out: json.dump(res, open(A.out, "w"), indent=1)
    sys.exit(0)

# ---- ROI / crop accuracy ---------------------------------------------------------------------------------------
roi_err = None
try:
    from PIL import Image
    import numpy as np
    errs = []
    sample = [(i, o) for i, f in enumerate(man["frames"]) for o in f["objects"]][:: max(1, sum(len(f["objects"]) for f in man["frames"]) // 25)]
    for i, o in sample:
        x, y, w, h = clip_roi(o["bbox"])
        st, b, _ = call("GET", "/api/vm/frames/%d/crop?x=%d&y=%d&w=%d&h=%d&max=640" % (fid[i], x, y, w, h), raw_ok=True)
        st2, full, _ = call("GET", "/api/vm/frames/%d/image" % fid[i], raw_ok=True)
        if st != 200 or st2 != 200: continue
        srv = np.asarray(Image.open(io.BytesIO(b)).convert("RGB"), np.float32)
        loc = np.asarray(Image.open(io.BytesIO(full)).convert("RGB").crop((x, y, x + w, y + h)), np.float32)
        if srv.shape != loc.shape: errs.append(255.0); continue
        errs.append(float(np.abs(srv - loc).mean()))
    roi_err = {"samples": len(errs), "mean_abs_error_0_255": round(statistics.mean(errs), 3), "max": round(max(errs), 3)}
except Exception as e:  # PIL/numpy missing: skip
    roi_err = {"skipped": str(e)}

# ---- index the even frames --------------------------------------------------------------------------------------
region = {}   # region_id -> (inst, cls)
t_idx = []
for i, f in enumerate(man["frames"]):
    if i % 2: continue
    for o in f["objects"]:
        if o["inst"] in unseen: continue
        x, y, w, h = clip_roi(jitter_box(o["bbox"], A.jitter))
        st, r, ms = call("POST", "/api/vm/objects", {"frame_id": fid[i], "x": x, "y": y, "w": w, "h": h})
        if st == 201: region[r["region"]["region_id"]] = (o["inst"], o["cls"]); t_idx.append(ms)
        else: print("index failed", st, r, file=sys.stderr)
print("indexed %d regions (median %.1f ms per index call)" % (len(region), statistics.median(t_idx)))
# duplicate handling
i0 = next(i for i, f in enumerate(man["frames"]) if i % 2 == 0 and f["objects"] and f["objects"][0]["inst"] not in unseen)
dup_ok = None
rid0 = next(iter(region))
st, r0, _ = call("GET", "/api/vm/objects/%d" % rid0)
st, rd, _ = call("POST", "/api/vm/objects", {"frame_id": r0["frame_id"], "x": r0["bbox"]["x"], "y": r0["bbox"]["y"], "w": r0["bbox"]["w"], "h": r0["bbox"]["h"]})
dup_ok = (st == 200 and rd["status"] == "duplicate" and rd["region"]["region_id"] == rid0)

# ---- queries --------------------------------------------------------------------------------------------------------
n_rel = {}
for inst, cls in region.values(): n_rel[inst] = n_rel.get(inst, 0) + 1
m_seen = {"p1": [], "p5": [], "hit5": [], "rec5": [], "rec5n": [], "rec10": [], "rr": [], "cls_p1": [], "cls_p5": [], "top1_sim": []}
m_unseen = {"cls_p1": [], "top1_sim": []}
lat_server, lat_total, lat_client, rank_examples = [], [], [], []
for i, f in enumerate(man["frames"]):
    if i % 2 == 0: continue
    for o in f["objects"]:
        x, y, w, h = clip_roi(jitter_box(o["bbox"], A.jitter))
        st, r, ms = call("POST", "/api/vm/objects/search", {"frame_id": fid[i], "x": x, "y": y, "w": w, "h": h, "top_k": 10})
        if st != 200: print("search failed", st, r, file=sys.stderr); continue
        lat_server.append(r["status"]["elapsed_ms"]); lat_total.append(r["status"].get("total_ms", float("nan"))); lat_client.append(ms)
        res = r["results"]
        if not res: continue
        if o["inst"] in unseen:
            m_unseen["cls_p1"].append(1.0 if region[res[0]["region_id"]][1] == o["cls"] else 0.0); m_unseen["top1_sim"].append(res[0]["similarity"]); continue
        rel = [1 if region[x_["region_id"]][0] == o["inst"] else 0 for x_ in res]
        clsrel = [1 if region[x_["region_id"]][1] == o["cls"] else 0 for x_ in res]
        nrel = n_rel.get(o["inst"], 0)
        m_seen["p1"].append(rel[0]); m_seen["p5"].append(sum(rel[:5]) / 5); m_seen["hit5"].append(1.0 if any(rel[:5]) else 0.0)
        if nrel: m_seen["rec5"].append(sum(rel[:5]) / nrel); m_seen["rec5n"].append(sum(rel[:5]) / min(5, nrel)); m_seen["rec10"].append(sum(rel[:10]) / nrel)
        m_seen["rr"].append(next((1.0 / (k + 1) for k, v in enumerate(rel) if v), 0.0))
        m_seen["cls_p1"].append(clsrel[0]); m_seen["cls_p5"].append(sum(clsrel[:5]) / 5); m_seen["top1_sim"].append(res[0]["similarity"])


def mean(v): return round(statistics.mean(v), 4) if v else None


def auc(pos, neg):  # P(pos > neg) for top-1 similarity of seen vs unseen queries
    if not pos or not neg: return None
    return round(sum((p > n) + 0.5 * (p == n) for p in pos for n in neg) / (len(pos) * len(neg)), 4)


out = {"dataset": {"frames": len(man["frames"]), "instances": len(insts), "unseen_instances": sorted(unseen), "jitter": A.jitter,
                   "indexed_regions": len(region), "queries_seen": len(m_seen["p1"]), "queries_unseen": len(m_unseen["cls_p1"])},
       "crop_accuracy_vs_local_crop": roi_err, "duplicate_handling_ok": dup_ok,
       "instance_retrieval": {"P@1": mean(m_seen["p1"]), "P@5": mean(m_seen["p5"]), "hit@5": mean(m_seen["hit5"]), "recall@5": mean(m_seen["rec5"]), "recall@5_vs_min(5,relevant)": mean(m_seen["rec5n"]),
                              "recall@10": mean(m_seen["rec10"]), "MRR": mean(m_seen["rr"])},
       "class_retrieval": {"P@1": mean(m_seen["cls_p1"]), "P@5": mean(m_seen["cls_p5"])},
       "unseen_objects": {"top1_class_correct": mean(m_unseen["cls_p1"]), "top1_similarity_mean_unseen": mean(m_unseen["top1_sim"]),
                          "top1_similarity_mean_seen": mean(m_seen["top1_sim"]), "AUC_seen_vs_unseen_top1_similarity": auc(m_seen["top1_sim"], m_unseen["top1_sim"])},
       "latency_ms_at_%d_regions" % len(region): {"scan_p50": round(pct(lat_server, 50), 2), "scan_p95": round(pct(lat_server, 95), 2), "scan_max": round(max(lat_server), 2),
                                                  "server_total_p50": round(pct(lat_total, 50), 2), "server_total_p95": round(pct(lat_total, 95), 2),
                                                  "client_p50": round(pct(lat_client, 50), 2), "client_p95": round(pct(lat_client, 95), 2), "n": len(lat_server)},
       "index_call_ms": {"p50": round(pct(t_idx, 50), 1), "p95": round(pct(t_idx, 95), 1)}}

scale = run_scale(len(region), [int(x) for x in A.scale.split(",")]) if A.scale else {}
out["latency_vs_index_size"] = scale
print(json.dumps(out, indent=1))
if A.out: json.dump(out, open(A.out, "w"), indent=1)
