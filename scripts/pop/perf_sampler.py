#!/usr/bin/env python3
"""D5 performance sampler (HOST tool). Polls the player once per second while Proof-of-Play sessions run and reports CPU, RAM,
NPU load, temperature and Argus FPS for a baseline phase and the active phase.

  perf_sampler.py --api http://PLAYER:20300 --exporter http://PLAYER:9101 --baseline 45 --sessions 3 --media ad-a --out result.json

Sources (all read-only): GET /api/pop/system (per-process /proc counters, system CPU jiffies, MemAvailable, NPU load, thermal) and
the Argus Prometheus exporter (argus_fps_current, argus_npu_load_percent). Sessions are scheduled through POST /api/pop/sessions
(they run back to back on the player); sampling continues until all have ended plus a short tail.
"""
import argparse, json, re, statistics, sys, time, urllib.request

ap = argparse.ArgumentParser()
ap.add_argument("--api", required=True); ap.add_argument("--exporter", required=True)
ap.add_argument("--baseline", type=int, default=45); ap.add_argument("--sessions", type=int, default=3)
ap.add_argument("--media", default="ad-a"); ap.add_argument("--prefix", default="perf"); ap.add_argument("--out", default="perf.json")
a = ap.parse_args()


def get(url, t=5):
    with urllib.request.urlopen(url, timeout=t) as r:
        return r.read().decode()


def post(url, body):
    req = urllib.request.Request(url, data=json.dumps(body).encode(), method="POST", headers={"Content-Type": "application/json", "Origin": a.api})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode())


def sample():
    t0 = time.time()
    s = json.loads(get(a.api + "/api/pop/system"))
    m = get(a.exporter + "/metrics")
    fps = re.search(r"^argus_fps_current\{[^}]*\} ([0-9.eE+-]+)", m, re.M)
    npu = re.search(r"^argus_npu_load_percent\{[^}]*\} ([0-9.eE+-]+)", m, re.M)
    return {"t": t0, "sys": s, "fps": float(fps.group(1)) if fps else None, "argus_npu_pct": float(npu.group(1)) if npu else None}


def run_until(seconds=None, done=None, tail=0):
    out = []; start = time.time(); end_seen = None
    while True:
        t = time.time()
        try: out.append(sample())
        except Exception as e: out.append({"t": t, "error": str(e)})
        if seconds is not None and t - start >= seconds: break
        if done is not None:
            if done() and end_seen is None: end_seen = t
            if end_seen is not None and t - end_seen >= tail: break
        if t - start > 600: break
        time.sleep(max(0.0, 1.0 - (time.time() - t)))
    return out


def summarize(samples):
    ok = [s for s in samples if "sys" in s]
    res = {"samples": len(ok)}
    if len(ok) < 3: return res
    hz = ok[0]["sys"]["cpu"]["hz"]; cores = ok[0]["sys"]["cpu"]["cores"]
    dt = ok[-1]["t"] - ok[0]["t"]
    tot = ok[-1]["sys"]["cpu"]["total_jiffies"] - ok[0]["sys"]["cpu"]["total_jiffies"]
    idle = ok[-1]["sys"]["cpu"]["idle_jiffies"] - ok[0]["sys"]["cpu"]["idle_jiffies"]
    res["system_cpu_busy_pct_of_all_cores"] = round(100.0 * (tot - idle) / tot, 2) if tot else None
    res["duration_s"] = round(dt, 1)
    per = {}
    for p in ok[-1]["sys"]["processes"]:
        first = next((q for q in ok[0]["sys"]["processes"] if q["pid"] == p["pid"]), None)
        if not first: continue
        cpu = 100.0 * (p["cpu_jiffies"] - first["cpu_jiffies"]) / hz / dt        # % of ONE core (can exceed 100 with threads)
        rss = [q["rss_kb"] for s in ok for q in s["sys"]["processes"] if q["pid"] == p["pid"]]
        per[p["comm"]] = {"cpu_pct_of_one_core": round(cpu, 2), "rss_mb_mean": round(statistics.mean(rss) / 1024, 1), "rss_mb_max": round(max(rss) / 1024, 1), "hwm_mb": round(p["hwm_kb"] / 1024, 1)}
    res["processes"] = per
    fps = [s["fps"] for s in ok if s.get("fps") is not None]
    if fps: res["argus_fps"] = {"mean": round(statistics.mean(fps), 2), "min": min(fps), "below_25": sum(1 for f in fps if f < 25)}
    mem = [s["sys"]["mem_kb"]["MemAvailable"] for s in ok]
    res["mem_available_mb"] = {"min": round(min(mem) / 1024), "mean": round(statistics.mean(mem) / 1024)}
    cores_npu = []
    for s in ok:
        m = re.findall(r"Core(\d):\s*(\d+)%", s["sys"].get("npu_load", ""))
        if m: cores_npu.append({int(c): int(v) for c, v in m})
    if cores_npu:
        res["npu_core_load_pct"] = {f"core{c}": {"mean": round(statistics.mean(x.get(c, 0) for x in cores_npu), 1), "max": max(x.get(c, 0) for x in cores_npu)} for c in (0, 1, 2)}
    temps = [max((z["milli_c"] for z in s["sys"]["thermal"]), default=0) / 1000 for s in ok]
    if temps: res["soc_temp_c"] = {"mean": round(statistics.mean(temps), 1), "max": round(max(temps), 1)}
    return res


print("baseline %d s (no job)..." % a.baseline, flush=True)
base = run_until(seconds=a.baseline)
ids = []
for i in range(a.sessions):
    sid = "%s-%d-%d" % (a.prefix, int(time.time()), i + 1)
    ids.append(sid)
    post(a.api + "/api/pop/sessions", {"expected_media_id": a.media, "session_id": sid})
    time.sleep(1.1)
print("scheduled", ids, flush=True)


def finished():
    try:
        return all(json.loads(get(a.api + "/api/pop/sessions/" + s))["state"] == "ended" for s in ids)
    except Exception:
        return False


active = run_until(done=finished, tail=15)
res = {"baseline": summarize(base), "active": summarize(active), "sessions": ids}
res["results"] = [json.loads(get(a.api + "/api/pop/sessions/" + s)) for s in ids]
res["results"] = [{"session_id": r["session_id"], "status": r["status"], "reason": r["reason"], "actual_play_ms": r["actual_play_ms"],
                   "captures": [(e["idx"], e["status"], e["took_ms"]) for e in r["evidence"]]} for r in res["results"]]
json.dump(res, open(a.out, "w"), indent=1)
print(json.dumps({k: res[k] for k in ("baseline", "active")}, indent=1))
for r in res["results"]: print(r["session_id"], r["status"], r["actual_play_ms"], r["captures"])
