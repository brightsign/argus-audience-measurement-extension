#!/usr/bin/env python3
"""Read-only resource sampler for the object-indexing performance comparison (HOST tool; talks to the player's dashboard API only).

  measure_object_indexing.py --api http://PLAYER:20300 --label camera_only --seconds 300 --out DIR [--interval 2] [--require-session SID]

Run it three times, once per controlled configuration (the operator changes objects.auto.* between runs, see the hardware plan):
  baseline_idle   no indexing at all (both sources off)     -> the reference for Argus FPS / Weston FPS / CPU
  camera_only     objects.auto.camera on, display off
  display_only    camera off, display on (while the product video plays)
  both            both sources on (while the product video plays)

It never changes configuration, never starts a backfill, writes nothing to the player. Every sample comes from existing read-only endpoints:
  /api/pop/system             system + per-process CPU jiffies, RSS/HWM, NPU load text, SD (mmcblk1) sectors written, memory
  /api/vm/status              Argus camera FPS (capture.fps), store/object counts
  /api/display/status         Weston live-preview FPS (fps; only meaningful while a viewer or a Proof-of-Play session holds the preview)
  /api/vm/objects/auto/status the worker: detector / MobileNetV2 (per crop) / index / total latency (moving averages, also per source), counters, queue depth, drops

Output: DIR/<label>.csv (one row per sample), DIR/<label>.json (summary: median / p95 / max). Honest limits are written into the summary:
  * SD writes are the whole card's sectors_written delta (all processes; the player is otherwise quiet), not SQLite alone.
  * process I/O counters are unavailable on this firmware (-1), so SQLite bytes are estimated as the objects.db + crop growth between samples.
  * NPU load is the OS text "Core0: x%, Core1: y%, Core2: z%" sampled at the interval (a sample, not an integral).
"""
import argparse, csv, json, os, re, statistics, sys, time, urllib.request

ap = argparse.ArgumentParser()
ap.add_argument("--api", required=True); ap.add_argument("--label", required=True); ap.add_argument("--seconds", type=int, default=300)
ap.add_argument("--interval", type=float, default=2.0); ap.add_argument("--out", required=True)
ap.add_argument("--require-session", default="", help="abort unless this Proof-of-Play session is the current one (display runs)")
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
PROCS = ["attention_demo", "vm_api_server", "dashboard-serve", "bs-rendered-fra", "argus-exporter", "image-stream-se"]


def get(path, timeout=6):
    try:
        with urllib.request.urlopen(a.api + path, timeout=timeout) as r:
            return json.loads(r.read())
    except Exception as e:
        return {"_error": str(e)}


def npu(txt):
    m = re.findall(r"Core(\d):\s*(\d+)%", txt or "")
    return {int(i): int(p) for i, p in m}


def pct(vals, p):
    v = sorted(x for x in vals if x is not None)
    if not v: return None
    return v[min(len(v) - 1, int(round(p / 100 * (len(v) - 1))))]


def summ(vals):
    v = [x for x in vals if x is not None]
    if not v: return None
    return {"n": len(v), "min": round(min(v), 3), "median": round(statistics.median(v), 3), "p95": round(pct(v, 95), 3), "max": round(max(v), 3), "mean": round(statistics.fmean(v), 3)}


rows, prev = [], None
t_end = time.time() + a.seconds
fields = ["t", "sys_cpu_pct", "argus_cpu_pct", "vm_cpu_pct", "vm_rss_kb", "argus_rss_kb", "weston_rss_kb", "mem_avail_kb", "npu0", "npu1", "npu2", "sd_written_kb_s", "argus_fps", "weston_fps",
          "cam_frames", "cam_objects", "cam_avg_detect_ms", "cam_avg_embed_ms", "cam_avg_index_ms", "cam_avg_total_ms", "cam_queue", "cam_dropped",
          "disp_frames", "disp_objects", "disp_avg_detect_ms", "disp_avg_embed_ms", "disp_avg_index_ms", "disp_avg_total_ms", "disp_queue", "disp_dropped",
          "worker_state", "db_bytes", "crop_bytes", "worker_detect_ms", "worker_embed_ms", "worker_index_ms", "worker_total_ms", "db_errors", "detector_errors", "provider_errors"]
if a.require_session:
    ps = get("/api/pop/sessions?limit=1")
    cur = (ps.get("sessions") or [{}])[0].get("session_id", "")
    if cur != a.require_session: sys.exit("current Proof-of-Play session is %r, not %r: start the product video first" % (cur, a.require_session))
print("sampling %s for %d s every %.1f s ..." % (a.label, a.seconds, a.interval), flush=True)
t0 = time.time()
while time.time() < t_end:
    ts = time.time()
    sysd, vm, disp = get("/api/pop/system"), get("/api/vm/status"), get("/api/display/status")
    cam = get("/api/vm/objects/auto/status?source=camera")                       # the worker block and the per-source list are the same for both sources
    procs = {p["comm"]: p for p in sysd.get("processes", [])}
    cpu = sysd.get("cpu", {}); sd = sysd.get("disks", {}).get("mmcblk1", {}).get("sectors_written")
    cur = {"tot": cpu.get("total_jiffies"), "idle": cpu.get("idle_jiffies"), "sd": sd, "t": ts, **{p: procs.get(p, {}).get("cpu_jiffies") for p in PROCS}}
    r = {k: None for k in fields}; r["t"] = round(ts - t0, 1)
    if prev and None not in (cur["tot"], prev["tot"]) and cur["tot"] > prev["tot"]:
        dt = cur["tot"] - prev["tot"]; r["sys_cpu_pct"] = round(100.0 * (1 - (cur["idle"] - prev["idle"]) / dt), 1)      # of ALL cores: 100% = 8 cores busy
        hz, cores = cpu.get("hz", 100), cpu.get("cores", 8)
        for key, col in (("attention_demo", "argus_cpu_pct"), ("vm_api_server", "vm_cpu_pct")):
            if cur[key] is not None and prev[key] is not None: r[col] = round(100.0 * (cur[key] - prev[key]) / (dt / cores) / 1.0, 1)   # of ONE core: 100% = one core busy
        if None not in (cur["sd"], prev["sd"]): r["sd_written_kb_s"] = round((cur["sd"] - prev["sd"]) * 512 / 1024.0 / max(0.001, cur["t"] - prev["t"]), 2)
    prev = cur
    r["vm_rss_kb"] = procs.get("vm_api_server", {}).get("rss_kb"); r["argus_rss_kb"] = procs.get("attention_demo", {}).get("rss_kb"); r["weston_rss_kb"] = procs.get("bs-rendered-fra", {}).get("rss_kb")
    r["mem_avail_kb"] = sysd.get("mem_kb", {}).get("MemAvailable")
    n = npu(sysd.get("npu_load", "")); r["npu0"], r["npu1"], r["npu2"] = n.get(0), n.get(1), n.get(2)
    r["argus_fps"] = (vm.get("capture") or {}).get("fps"); r["weston_fps"] = disp.get("fps") if disp.get("active") else None
    w = cam.get("worker", {}) if isinstance(cam, dict) else {}
    r["worker_state"] = w.get("state"); r["worker_detect_ms"] = w.get("avg_detect_ms"); r["worker_embed_ms"] = w.get("avg_embed_ms"); r["worker_index_ms"] = w.get("avg_index_ms"); r["worker_total_ms"] = w.get("last_frame_total_ms")
    r["db_errors"] = w.get("db_errors"); r["detector_errors"] = w.get("detector_errors"); r["provider_errors"] = w.get("provider_errors")
    for pre, typ in (("cam", "camera"), ("disp", "display")):
        for s in (cam.get("sources") or []):
            if s.get("type") == typ:
                r[pre + "_frames"] = s.get("frames_processed"); r[pre + "_objects"] = s.get("objects_indexed"); r[pre + "_queue"] = s.get("queue_depth"); r[pre + "_dropped"] = s.get("dropped_queue_full")
                r[pre + "_avg_detect_ms"] = s.get("avg_detect_ms"); r[pre + "_avg_embed_ms"] = s.get("avg_embed_ms"); r[pre + "_avg_index_ms"] = s.get("avg_index_ms"); r[pre + "_avg_total_ms"] = s.get("avg_frame_total_ms")
    o = vm.get("objects") or {}; r["db_bytes"] = o.get("db_bytes"); r["crop_bytes"] = o.get("crop_bytes")
    rows.append(r)
    time.sleep(max(0.0, a.interval - (time.time() - ts)))

with open(os.path.join(a.out, a.label + ".csv"), "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=fields); w.writeheader(); w.writerows(rows)
col = lambda k: [r[k] for r in rows]
last = lambda k: next((r[k] for r in reversed(rows) if r[k] is not None), None)
first = lambda k: next((r[k] for r in rows if r[k] is not None), None)
delta = lambda k: (last(k) - first(k)) if first(k) is not None and last(k) is not None else None
dur = max(1.0, rows[-1]["t"] - rows[0]["t"]) if rows else 1.0
summary = {
    "label": a.label, "seconds": a.seconds, "interval_s": a.interval, "samples": len(rows), "api": a.api,
    "argus_fps": summ(col("argus_fps")), "weston_preview_fps_while_active": summ(col("weston_fps")),
    "cpu_pct_all_cores_of_100": summ(col("sys_cpu_pct")), "argus_cpu_pct_of_one_core": summ(col("argus_cpu_pct")), "vm_api_server_cpu_pct_of_one_core": summ(col("vm_cpu_pct")),
    "vm_api_server_rss_kb": summ(col("vm_rss_kb")), "argus_rss_kb": summ(col("argus_rss_kb")), "mem_available_kb": summ(col("mem_avail_kb")),
    "npu_core0_pct": summ(col("npu0")), "npu_core1_pct": summ(col("npu1")), "npu_core2_pct": summ(col("npu2")),
    "sd_written_kb_per_s": summ(col("sd_written_kb_s")),
    "sd_written_total_mb": round(sum(x for x in col("sd_written_kb_s") if x) * a.interval / 1024.0, 2) if any(col("sd_written_kb_s")) else None,
    "objects_db_growth_bytes": delta("db_bytes"), "crop_growth_bytes": delta("crop_bytes"),
    "latency_ms_moving_avg_at_end": {"worker_detect": last("worker_detect_ms"), "worker_mobilenet_per_crop": last("worker_embed_ms"), "worker_index_per_frame_with_objects": last("worker_index_ms"),
                                     "camera": {k: last("cam_avg_" + k + "_ms") for k in ("detect", "embed", "index", "total")}, "display": {k: last("disp_avg_" + k + "_ms") for k in ("detect", "embed", "index", "total")}},
    "frames_indexed": {"camera": delta("cam_frames"), "display": delta("disp_frames")}, "objects_indexed": {"camera": delta("cam_objects"), "display": delta("disp_objects")},
    "frames_per_min": {"camera": round(60.0 * (delta("cam_frames") or 0) / dur, 2), "display": round(60.0 * (delta("disp_frames") or 0) / dur, 2)},
    "queue_depth_max": {"camera": max([x for x in col("cam_queue") if x is not None] or [0]), "display": max([x for x in col("disp_queue") if x is not None] or [0])},
    "dropped_queue_full": {"camera": last("cam_dropped"), "display": last("disp_dropped")},
    "errors_at_end": {"db": last("db_errors"), "detector": last("detector_errors"), "provider": last("provider_errors")},
    "notes": ["SD writes are the whole card (mmcblk1) sectors_written delta, not SQLite alone", "per-process I/O counters are unavailable on this firmware; objects.db and crop growth are reported instead",
              "NPU load is sampled text, not an integral; Weston FPS exists only while a viewer or a Proof-of-Play session holds the preview",
              "per-source latencies are moving averages (factor 0.2) of the worker; MobileNetV2 is per crop"],
}
json.dump(summary, open(os.path.join(a.out, a.label + ".json"), "w"), indent=1)
print(json.dumps(summary, indent=1))
