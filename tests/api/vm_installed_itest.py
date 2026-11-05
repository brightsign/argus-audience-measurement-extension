#!/usr/bin/env python3
"""Installed-package validation of D3 (full-frame search) and D4 (object search): latency, RAM, FPS, errors.

  vm_installed_itest.py http://PLAYER:20300 http://PLAYER:9101 OBJECT_DATASET_DIR out.json

Needs a store whose frames 1..N are the make_object_scenes.py frames in manifest order (vm_ingest_dir) and whose
object index is small. Indexes 100 ground-truth regions, then measures ROI / region / full-frame searches. RAM and CPU come from
GET /api/pop/system (needs visual_memory.pop.enabled for the measurement only), FPS from the Argus exporter.
Uses people-free synthetic images only. Exit 0 = no request failed.
"""
import json, re, statistics, sys, time, urllib.error, urllib.request

BASE, EXP, DATA, OUT = sys.argv[1].rstrip("/"), sys.argv[2].rstrip("/"), sys.argv[3].rstrip("/"), sys.argv[4]
errors = []


def call(method, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(BASE + path, data=data, method=method, headers=({"Content-Type": "application/json"} if data else {}) | ({"Origin": BASE} if method == "POST" else {}))
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, json.loads(r.read()), (time.time() - t0) * 1000
    except urllib.error.HTTPError as e:
        try: b = json.loads(e.read())
        except Exception: b = {}
        errors.append((method, path, e.code, str(b)[:120]))
        return e.code, b, (time.time() - t0) * 1000


def pct(v, p):
    v = sorted(v)
    return round(v[min(len(v) - 1, int(p / 100 * len(v)))], 1)


def stats(v):
    return {"n": len(v), "p50_ms": pct(v, 50), "p95_ms": pct(v, 95), "max_ms": round(max(v), 1)}


def fps():
    m = urllib.request.urlopen(EXP + "/metrics", timeout=5).read().decode()
    g = re.search(r"^argus_fps_current\{[^}]*\} ([0-9.]+)", m, re.M)
    return float(g.group(1)) if g else None


def procs():
    s = json.loads(urllib.request.urlopen(BASE + "/api/pop/system", timeout=10).read())
    return {p["comm"]: p for p in s["processes"]}, s


man = json.load(open(DATA + "/manifest.json"))
items = man["frames"] if isinstance(man, dict) and "frames" in man else man
n_frames = len(items)
st, fr, _ = call("GET", "/api/vm/frames?limit=1")
first = call("GET", "/api/vm/frames/1")[1]
print("store frames:", fr["total"], "| frame #1 source:", first.get("source_id") or first.get("frame", {}).get("source_id"))

p0, s0 = procs(); fps0 = fps()
rss0 = {k: v["rss_kb"] for k, v in p0.items()}

# ---- index 100 ground-truth regions (even manifest frames), measuring the index call
index_ms = []; region_ids = []
for i, it in enumerate(items):
    if len(region_ids) >= 100: break
    if i % 2: continue
    for o in it["objects"][:1]:
        b = o["bbox"]
        st, r, ms = call("POST", "/api/vm/objects", {"frame_id": i + 1, "x": b[0], "y": b[1], "w": b[2], "h": b[3], "label": o.get("cls", "obj")})
        if st in (200, 201): index_ms.append(ms); region_ids.append(r["region"]["region_id"])
print("indexed", len(region_ids), "regions")

# ---- ROI searches (crop + decode + NPU embedding + scan) on odd frames
roi_total, roi_scan = [], []
for i, it in enumerate(items):
    if len(roi_total) >= 40: break
    if i % 2 == 0: continue
    b = it["objects"][0]["bbox"]
    st, r, wall = call("POST", "/api/vm/objects/search", {"frame_id": i + 1, "x": b[0], "y": b[1], "w": b[2], "h": b[3], "top_k": 5})
    if st == 200: roi_total.append(r["status"]["total_ms"]); roi_scan.append(r["status"]["elapsed_ms"])
# ---- region-query searches (scan only)
reg = []
for rid in region_ids[:40]:
    st, r, wall = call("POST", "/api/vm/objects/search", {"region_id": rid, "top_k": 5})
    if st == 200: reg.append(r["status"]["elapsed_ms"])
# ---- D3 full-frame search on frames with embeddings
d3 = []; d3_top = None
for fid in list(range(250, 304, 3)):
    st, r, wall = call("POST", "/api/vm/search/frame?frame_id=%d&top_k=5" % fid)
    if st == 200: d3.append(r["status"].get("total_ms", r["status"].get("elapsed_ms", wall)))
st, r, _ = call("POST", "/api/vm/search/frame?frame_id=259&top_k=3")
d3_top = [(x["frame_id"], round(x["similarity"], 4)) for x in r.get("results", [])] if st == 200 else None

p1, s1 = procs(); fps1 = fps()
dt = (s1["utc_ms"] - s0["utc_ms"]) / 1000.0; hz = s1["cpu"]["hz"]
res = {"regions_indexed": len(region_ids), "index_call": stats(index_ms), "roi_search_total_server": stats(roi_total), "roi_search_scan_only": stats(roi_scan),
       "region_search_scan_only": stats(reg), "d3_full_frame_search": stats(d3) if d3 else None, "d3_frame259_top": d3_top,
       "vm_api_server": {"rss_mb_before": round(rss0.get("vm_api_server", 0) / 1024, 1), "rss_mb_after": round(p1["vm_api_server"]["rss_kb"] / 1024, 1),
                         "hwm_mb": round(p1["vm_api_server"]["hwm_kb"] / 1024, 1),
                         "cpu_pct_of_one_core": round(100.0 * (p1["vm_api_server"]["cpu_jiffies"] - p0["vm_api_server"]["cpu_jiffies"]) / hz / dt, 1)},
       "attention_demo": {"rss_mb_after": round(p1["attention_demo"]["rss_kb"] / 1024, 1), "hwm_mb": round(p1["attention_demo"]["hwm_kb"] / 1024, 1)},
       "mem_available_mb": {"before": round(s0["mem_kb"]["MemAvailable"] / 1024), "after": round(s1["mem_kb"]["MemAvailable"] / 1024)},
       "argus_fps": {"before": fps0, "after": fps1}, "window_s": round(dt, 1), "http_errors": errors}
json.dump(res, open(OUT, "w"), indent=1)
print(json.dumps(res, indent=1))
sys.exit(1 if errors else 0)
