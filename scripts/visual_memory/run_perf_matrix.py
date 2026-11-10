#!/usr/bin/env python3
"""Controlled four-mode performance comparison on a real player (HOST driver; stdlib + the sampler next to it).

  run_perf_matrix.py --api http://PLAYER:20300 --base-config ACTIVE_CONFIG.json --restore-config FINAL_CONFIG.json --out DIR [--seconds 300] [--warmup 45]
                     [--modes baseline,camera_only,display_only,both]

For each mode it (1) uploads a configuration that differs from the base ONLY in objects.auto (the player restarts vm_api_server in place, no reboot),
(2) keeps the advertisement ad-c playing back-to-back through the normal Proof-of-Play job mode (so Weston preview, display history and D5 evidence capture run identically
in every mode), (3) waits for a warm-up, (4) runs the read-only sampler for --seconds, (5) measures search latency under that load, (6) records the D5 verdict and display-history
frame count of every session of the run and the camera luminance (a dark scene changes the camera FPS by itself). At the end the configuration --restore-config is uploaded.
SSH credentials are read from ~/.bsp_pw by sshpass -f (never printed). Nothing else on the player is changed.
"""
import argparse, json, os, statistics, subprocess, sys, threading, time, urllib.request, urllib.error, io

ap = argparse.ArgumentParser()
ap.add_argument("--api", required=True); ap.add_argument("--player", default="192.168.0.165"); ap.add_argument("--base-config", required=True); ap.add_argument("--restore-config", required=True)
ap.add_argument("--out", required=True); ap.add_argument("--seconds", type=int, default=300); ap.add_argument("--warmup", type=int, default=45)
ap.add_argument("--modes", default="baseline,camera_only,display_only,both"); ap.add_argument("--media", default="ad-c")
a = ap.parse_args(); os.makedirs(a.out, exist_ok=True)
HERE = os.path.dirname(os.path.abspath(__file__)); SAMPLER = os.path.join(HERE, "measure_object_indexing.py")
SSH = ["-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null", "-o", "ConnectTimeout=10"]


def call(method, path, body=None, timeout=60):
    req = urllib.request.Request(a.api + path, data=json.dumps(body).encode() if body is not None else None, method=method, headers={"Content-Type": "application/json", "Origin": a.api} if body is not None else {})
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            b = r.read(); return r.status, (json.loads(b) if "json" in r.headers.get("Content-Type", "") else b), (time.time() - t0) * 1000
    except urllib.error.HTTPError as e:
        b = e.read()
        try: return e.code, json.loads(b), (time.time() - t0) * 1000
        except Exception: return e.code, b, (time.time() - t0) * 1000
    except Exception as e:
        return 0, str(e), (time.time() - t0) * 1000


def upload_config(path):
    r = subprocess.run(["sshpass", "-f", os.path.expanduser("~/.bsp_pw"), "scp", "-q"] + SSH + [path, "brightsign@%s:/storage/sd/configs/argus-config.json" % a.player], capture_output=True, text=True)
    if r.returncode: sys.exit("config upload failed: " + r.stderr[:200])


def mode_config(mode):
    d = json.load(open(a.base_config)); au = d["visual_memory"]["objects"]["auto"]
    au.setdefault("camera", {}); au.setdefault("display", {})
    if mode == "baseline": au["enabled"] = False
    else:
        au["enabled"] = True; au["camera"]["enabled"] = mode in ("camera_only", "both")
        au["display"].update({"enabled": mode in ("display_only", "both"), "live_scan": True, "frames_per_minute": 12, "queue_depth": 8}); au["live_scan"] = True
    p = os.path.join(a.out, "config_%s.json" % mode); json.dump(d, open(p, "w"), indent=2); return p


def wait_config(mode, timeout=60):
    want_cam = mode in ("camera_only", "both"); want_dsp = mode in ("display_only", "both"); t0 = time.time()
    time.sleep(4)
    while time.time() - t0 < timeout:
        st, s, _ = call("GET", "/api/vm/objects/auto/status?source=%s" % ("display" if mode == "display_only" else "camera"))
        if st == 200 and isinstance(s, dict):
            c = s.get("source_config", {})
            if mode == "baseline" and s.get("enabled") is False and not s.get("running"): return True
            if mode != "baseline" and s.get("running") and s.get("enabled") is True and c.get("camera", {}).get("enabled") == want_cam and c.get("display", {}).get("enabled") == want_dsp and c.get("display", {}).get("live_scan") is True: return True
        time.sleep(1.5)
    return False


class AdLoop(threading.Thread):
    """Back-to-back ad-c sessions through the normal job mode; records every session id."""
    def __init__(self): super().__init__(daemon=True); self.stop = threading.Event(); self.sessions = []; self.errors = []
    def run(self):
        while not self.stop.is_set():
            st, r, _ = call("POST", "/api/pop/sessions", {"expected_media_id": a.media, "capture_backend": "resilience_grpc", "capture_offsets_s": [3, 15, 27]})
            if st != 201: self.errors.append((st, str(r)[:120])); self.stop.wait(5); continue
            sid = r["session_id"]; self.sessions.append(sid); t0 = time.time()
            while time.time() - t0 < 120 and not self.stop.is_set():
                st, s, _ = call("GET", "/api/pop/sessions/" + sid)
                if st == 200 and s.get("end_utc_ms", 0) > 0 and s.get("verification"): break
                time.sleep(2)
            self.stop.wait(3)


def luminance():
    try:
        from PIL import Image, ImageStat
        st, d, _ = call("GET", "/api/vm/frames?limit=1"); f = d["frames"][0]
        st, b, _ = call("GET", "/api/vm/frames/%s/image" % f["id"]); return round(ImageStat.Stat(Image.open(io.BytesIO(b)).convert("L")).mean[0], 1)
    except Exception: return None


def searches(n=20):
    out = {}
    for src in ("camera", "display"):
        st, l, _ = call("GET", "/api/vm/objects?source_type=%s&limit=40" % src)
        objs = l.get("objects", []) if st == 200 and isinstance(l, dict) else []
        ms = []
        for o in objs[:n]:
            st, r, t = call("POST", "/api/vm/objects/search", {"region_id": o["region_id"], "top_k": 10})
            if st == 200: ms.append(t)
        if ms: s = sorted(ms); out[src] = {"n": len(ms), "median_ms": round(statistics.median(ms), 1), "p95_ms": round(s[min(len(s) - 1, int(0.95 * len(s)))], 1), "max_ms": round(max(ms), 1)}
    return out


def session_report(ids):
    rep = []
    for sid in ids:
        st, s, _ = call("GET", "/api/pop/sessions/" + sid)
        if st != 200: rep.append({"session": sid, "error": st}); continue
        v = s.get("verification") or {}
        st, f, _ = call("GET", "/api/vm/display-frames?session_id=%s&limit=100" % sid)
        rep.append({"session": sid, "status": s.get("status"), "actual_play_ms": s.get("actual_play_ms"), "evidence_ok": s.get("evidence_ok"), "frames_matched": v.get("frames_matched"), "mean_sim": round(v.get("mean_sim_expected") or 0, 4),
                    "history_frames": f.get("total") if st == 200 else None})
    return rep


summary = {}
for mode in a.modes.split(","):
    print("=== mode %s" % mode, flush=True)
    upload_config(mode_config(mode))
    ok = wait_config(mode); print("  configuration active: %s" % ok, flush=True)
    if not ok: summary[mode] = {"error": "configuration did not become active"}; continue
    loop = AdLoop(); loop.start()
    lum0 = luminance(); time.sleep(a.warmup)
    t0 = time.time()
    r = subprocess.run([sys.executable, SAMPLER, "--api", a.api, "--label", mode, "--seconds", str(a.seconds), "--out", a.out], capture_output=True, text=True)
    srch = searches(); lum1 = luminance()
    loop.stop.set(); loop.join(timeout=90)
    time.sleep(35)                                       # the last session of the run finishes and is verified
    sm = json.load(open(os.path.join(a.out, mode + ".json"))) if os.path.exists(os.path.join(a.out, mode + ".json")) else {"error": r.stderr[-300:]}
    sm["search_latency_under_load"] = srch; sm["camera_luminance_start_end"] = [lum0, lum1]
    sm["sessions"] = session_report(loop.sessions); sm["session_loop_errors"] = loop.errors
    json.dump(sm, open(os.path.join(a.out, mode + ".json"), "w"), indent=1); summary[mode] = sm
    print("  fps median %s | sessions %d | search %s" % ((sm.get("argus_fps") or {}).get("median"), len(loop.sessions), srch), flush=True)
upload_config(a.restore_config); time.sleep(12); print("final configuration restored (uploaded %s)" % a.restore_config, flush=True)
json.dump(summary, open(os.path.join(a.out, "matrix.json"), "w"), indent=1)
