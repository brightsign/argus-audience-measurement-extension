#!/usr/bin/env python3
"""A/B performance sampler for the display preview (HOST tool, read-only).

  perf_ab.py --base http://PLAYER:20300 --exporter http://PLAYER:9101 --seconds 600 --label display_off --out result.json [--display]

Once per second it reads GET /api/pop/system (per-process /proc counters, system CPU, memory, NPU load, thermal; needs visual_memory.pop.enabled) and the
Argus exporter (argus_fps_current, argus_npu_load_percent). With --display it also reads GET /api/display/status every 5 s (preview fps, acquisition latency,
duplicate / stale / ambiguous counters, frame age and timing uncertainty). Reports per process CPU (% of ONE core, from jiffies) and RSS, Argus FPS
(mean / median / min / p5), NPU load, and the display counters at the start and end.

Only processes that /api/pop/system covers are reported; other processes (the OS producer, the bridge when the installed vm_api_server does not list it)
need a build whose process list includes them.
"""
import argparse, json, re, statistics, sys, time, urllib.request

ap = argparse.ArgumentParser()
ap.add_argument("--base", required=True); ap.add_argument("--exporter", required=True)
ap.add_argument("--seconds", type=int, default=600); ap.add_argument("--label", default="run"); ap.add_argument("--out", default="perf_ab.json")
ap.add_argument("--display", action="store_true")
a = ap.parse_args()


def get(url, t=5):
    with urllib.request.urlopen(url, timeout=t) as r:
        return r.read().decode()


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(p * len(v)))] if v else None


samples, fps, npu_exp, disp = [], [], [], []
t_end = time.time() + a.seconds
nxt = time.time()
errors = 0
while time.time() < t_end:
    t0 = time.time()
    try:
        s = json.loads(get(a.base + "/api/pop/system"))
        m = get(a.exporter + "/metrics")
        f = re.search(r"^argus_fps_current\{[^}]*\} ([0-9.eE+-]+)", m, re.M)
        n = re.search(r"^argus_npu_load_percent\{[^}]*\} ([0-9.eE+-]+)", m, re.M)
        samples.append({"t": t0, "sys": s})
        if f: fps.append(float(f.group(1)))
        if n: npu_exp.append(float(n.group(1)))
        if a.display and len(samples) % 5 == 1:
            disp.append({"t": t0, "d": json.loads(get(a.base + "/api/display/status"))})
    except Exception as e:
        errors += 1
    nxt += 1.0
    time.sleep(max(0.0, nxt - time.time()))

ok = [x for x in samples if "sys" in x]
res = {"label": a.label, "seconds": a.seconds, "samples": len(ok), "errors": errors}
if len(ok) >= 3:
    first, last = ok[0]["sys"], ok[-1]["sys"]
    dt = ok[-1]["t"] - ok[0]["t"]
    hz = first["cpu"]["hz"]
    tot = last["cpu"]["total_jiffies"] - first["cpu"]["total_jiffies"]
    idle = last["cpu"]["idle_jiffies"] - first["cpu"]["idle_jiffies"]
    res["system_cpu_busy_pct_of_all_cores"] = round(100.0 * (tot - idle) / tot, 2) if tot else None
    res["duration_s"] = round(dt, 1)
    per = {}
    for p in last["processes"]:
        q = next((x for x in first["processes"] if x["pid"] == p["pid"]), None)
        if not q: continue
        rss = [y["rss_kb"] for x in ok for y in x["sys"]["processes"] if y["pid"] == p["pid"]]
        per[p["comm"]] = {"cpu_pct_of_one_core": round(100.0 * (p["cpu_jiffies"] - q["cpu_jiffies"]) / hz / dt, 2),
                          "rss_mb_mean": round(statistics.mean(rss) / 1024, 1), "rss_mb_max": round(max(rss) / 1024, 1)}
    res["processes"] = per
    mem = [x["sys"]["mem_kb"]["MemAvailable"] for x in ok]
    res["mem_available_mb"] = {"min": round(min(mem) / 1024), "mean": round(statistics.mean(mem) / 1024)}
    cores = []
    for x in ok:
        mm = re.findall(r"Core(\d):\s*(\d+)%", x["sys"].get("npu_load", ""))
        if mm: cores.append({int(c): int(v) for c, v in mm})
    if cores:
        res["npu_core_load_pct"] = {f"core{c}": {"mean": round(statistics.mean(x.get(c, 0) for x in cores), 1), "max": max(x.get(c, 0) for x in cores)} for c in (0, 1, 2)}
    temps = [max((z["milli_c"] for z in x["sys"]["thermal"]), default=0) / 1000 for x in ok]
    res["soc_temp_c"] = {"mean": round(statistics.mean(temps), 1), "max": round(max(temps), 1)}
if fps:
    res["argus_fps"] = {"mean": round(statistics.mean(fps), 2), "median": statistics.median(fps), "min": min(fps), "p5": pct(fps, 0.05), "samples": len(fps),
                        "below_25": sum(1 for v in fps if v < 25)}
    res["argus_fps_per_minute_mean"] = [round(statistics.mean(fps[i:i + 60]), 1) for i in range(0, len(fps), 60)]       # shows lighting-driven camera changes
if npu_exp:
    res["argus_npu_load_pct"] = {"mean": round(statistics.mean(npu_exp), 1), "max": max(npu_exp)}
if disp:
    d0, d1 = disp[0]["d"], disp[-1]["d"]
    res["display"] = {"first": {k: d0.get(k) for k in ("state", "fps", "counters", "acquire_ms")}, "last": {k: d1.get(k) for k in ("state", "fps", "counters", "acquire_ms", "last_frame", "bridge", "config")},
                      "fps_samples": [x["d"].get("fps") for x in disp], "frame_age_ms_samples": [(x["d"].get("last_frame") or {}).get("age_ms") for x in disp],
                      "uncertainty_ms": (d1.get("last_frame") or {}).get("uncertainty_ms")}
json.dump(res, open(a.out, "w"), indent=1)
print(json.dumps({k: v for k, v in res.items() if k != "display"}, indent=1))
if "display" in res: print(json.dumps({"display_last": res["display"]["last"]}, indent=1)[:1800])
