#!/usr/bin/env python3
"""Phase 2 host test: 1 FPS display history during registered Proof-of-Play sessions, searched through the BrightSign Display source of Visual Search.

  display_history_cdp.py VM_API_SERVER DASHBOARD_SERVER ARGUS_DISPLAY_BRIDGE FAKE_RENDERED_OUTPUT_SERVER OUTDIR

Real processes: the OS-service test double (real gRPC, 5 FPS, content that changes every second), argus_display_bridge, vm_api_server (resilience_grpc
backend, display history ON, isolated schema-2 store), the Go dashboard server and headless Chrome. The player application is emulated (events + jobs).
Nothing in a result is pre-set: every history frame is a frame the bridge pulled over gRPC during a session. Synthetic pictures only.

Tests A (correct ad), B (wrong ad, 15 s loop in a 30 s slot), C (interrupted at 10 s), D (producer stalls mid-play), E (historical search boundaries),
F (storage pressure: recording pauses, checkpoints unaffected), plus dashboard checks.
"""
import asyncio, base64, hashlib, urllib.parse, io, json, os, re, signal, sqlite3, zoneinfo, datetime, subprocess, sys, tempfile, threading, time, traceback, urllib.request, urllib.error, websockets
from PIL import Image, ImageDraw

API_BIN, DASH_BIN, BRIDGE_BIN, FAKE_BIN, OUT = sys.argv[1:6]
os.makedirs(OUT, exist_ok=True)
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
API_PORT, DASH_PORT, CDP_PORT = 20693, 20692, 9594
BASE = f"http://127.0.0.1:{DASH_PORT}/"
SLOT_A, SLOT_C = 30, 14
tmp = tempfile.mkdtemp(prefix="dhist_")
results, logs, procs = [], [], []
dsock, bsock = os.path.join(tmp, "d.sock"), os.path.join(tmp, "b.sock")
sd, root, proot = os.path.join(tmp, "sd"), os.path.join(tmp, "store"), os.path.join(tmp, "popstore")
events = os.path.join(sd, "pop", "events.jsonl"); jobs = os.path.join(sd, "pop", "jobs")
REPORT = {}


def check(name, cond, extra=""):
    results.append(bool(cond)); print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(extra)[:600]) if not cond and extra != "" else ""), flush=True)


def start(cmd, log):
    p = subprocess.Popen(cmd, stdout=open(os.path.join(tmp, log), "w"), stderr=subprocess.STDOUT); procs.append(p); return p


def stop(p):
    try: p.send_signal(signal.SIGTERM); p.wait(timeout=8)
    except Exception:
        try: p.kill()
        except Exception: pass


def ctl(**kw):
    c = {"frame": "IDLE", "pause": False, "black": False, "frozen": False, "unavailable": False, "clock_skew_ms": 0, "provider": 7}; c.update(kw)
    p = os.path.join(tmp, "control.json"); json.dump(c, open(p + ".tmp", "w")); os.replace(p + ".tmp", p)


# ---- synthetic "advertisements": a picture that changes every second (so a reference from one moment finds that moment) -------------------------------
def scene(kind, t, w=576, h=360):
    """kind A: 60 s ad, a block moving right; palette changes after 30 s. kind B: 15 s loop, a vertical bar sweeping. kind IDLE: grey."""
    im = Image.new("RGB", (w, h)); d = ImageDraw.Draw(im)
    if kind == "A":
        late = t >= 30
        d.rectangle([0, 0, w, h], fill=(30, 70, 140) if not late else (150, 60, 40))
        d.rectangle([0, h * 0.62, w, h], fill=(20, 120, 60) if not late else (230, 200, 40))
        x = int((t % 30) / 30.0 * (w - 90)); d.rectangle([x, int(h * 0.25), x + 90, int(h * 0.55)], fill=(250, 250, 250))
        d.ellipse([w * 0.7, h * 0.08, w * 0.9, h * 0.28], fill=(240, 220, 60) if not late else (60, 60, 200))
    elif kind == "B":
        d.rectangle([0, 0, w, h], fill=(120, 30, 150)); d.rectangle([0, 0, w, int(h * 0.3)], fill=(200, 200, 40))
        x = int((t % 15) / 15.0 * (w - 60)); d.rectangle([x, 0, x + 60, h], fill=(20, 220, 220))
        d.ellipse([w * 0.1, h * 0.55, w * 0.4, h * 0.9], fill=(250, 90, 20))
    elif kind == "C":
        for i in range(8):
            d.rectangle([i * w / 8, 0, (i + 1) * w / 8, h], fill=((i * 31 + int(t) * 17) % 256, (i * 57 + 90) % 256, (255 - i * 29) % 256))
        d.ellipse([w * 0.3, h * 0.3, w * 0.7, h * 0.7], fill=(255, 255, 255))
    else:
        d.rectangle([0, 0, w, h], fill=(90, 90, 90)); d.rectangle([w * 0.45, h * 0.4, w * 0.55, h * 0.6], fill=(200, 200, 200))
    return im


def display_picture(kind, t):
    c = Image.new("RGB", (640, 360), (0, 0, 0)); c.paste(scene(kind, t), (32, 0)); return c


def jpeg_bytes(im, q=90):
    b = io.BytesIO(); im.save(b, "JPEG", quality=q); return b.getvalue()


def http(method, path, body=None, ctype="application/json", port=None, origin=True):
    port = port or API_PORT
    data = body if isinstance(body, (bytes, type(None))) else json.dumps(body).encode()
    h = {"Content-Type": ctype}
    if origin: h["Origin"] = f"http://127.0.0.1:{port}"
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", method=method, data=data, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=30) as r: return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try: return e.code, json.loads(e.read())
        except Exception: return e.code, {}


class Player(threading.Thread):
    """Emulated player application: runs jobs, and 'renders' the advertisement by switching the OS-service test double's picture every 100 ms."""
    def __init__(self):
        super().__init__(daemon=True); self.seen, self.stop_flag, self.seq, self.t0 = set(), False, 0, time.time(); self.stall = None

    def emit(self, ev, **kw):
        self.seq += 1
        with open(events, "a") as f: f.write(json.dumps({"ev": ev, "seq": self.seq, "mono_ms": int((time.time() - self.t0) * 1000), **kw}) + "\n")

    def frame_for(self, mid, t):
        return ("A_%d" % min(59, int(t))) if mid == "ad-a" else ("B_%d" % (int(t) % 15)) if mid == "ad-b" else "C_%d" % (int(t) % 14)

    def run(self):
        self.emit("boot", mode="jobs"); self.emit("idle_started", ok=True)
        while not self.stop_flag:
            names = sorted(n for n in os.listdir(jobs) if n.endswith(".json") and n not in self.seen) if os.path.isdir(jobs) else []
            if not names: time.sleep(0.2); continue
            n = names[0]; self.seen.add(n); job = json.load(open(os.path.join(jobs, n)))
            sid, mid, exp = job["session_id"], job["media_id"], job["expected_media_id"]
            self.emit("session_scheduled_run", session=sid, job_id=job["job_id"], media_id=mid, expected_media_id=exp, capture_count=0)
            self.emit("play_requested", session=sid, media_id=mid, expected_media_id=exp, ok=True)
            time.sleep(0.15); ctl(frame=self.frame_for(mid, 0))
            self.emit("play_started", session=sid, media_id=mid, expected_media_id=exp, vcode=3, latency_ms=200)
            t0 = time.time(); limit = job["max_play_ms"]; reason = "slot_end"
            if job.get("interrupt_at_ms"): limit = min(limit, job["interrupt_at_ms"]); reason = "interrupted" if job["interrupt_at_ms"] < job["max_play_ms"] else "slot_end"
            while (time.time() - t0) * 1000 < limit and not self.stop_flag:
                t = time.time() - t0
                stalled = self.stall and self.stall[0] <= t < self.stall[1]
                ctl(frame=self.frame_for(mid, t), pause=bool(stalled)); time.sleep(0.1)
            self.emit("play_stopped", session=sid, media_id=mid, reason=reason)
            self.emit("session_end", session=sid, media_id=mid, expected_media_id=exp, reason=reason, actual_play_ms=int((time.time() - t0) * 1000))
            ctl(frame="IDLE"); self.emit("idle_started", ok=True)


def wait_history_drained(timeout=40):
    t0 = time.time()
    while time.time() - t0 < timeout:
        st, h = http("GET", "/api/pop/history/status")
        if st == 200 and h["queue"]["depth"] == 0 and h["store"]["pending_embed"] == 0 and not h["active_sessions"]: return h
        time.sleep(0.5)
    return http("GET", "/api/pop/history/status")[1]


def run_session(expected, played=None, interrupt=None, offsets=(3, 15, 27), timeout=90):
    body = {"expected_media_id": expected, "play_media_id": played or expected, "capture_offsets_s": list(offsets), "capture_backend": "resilience_grpc"}
    if interrupt is not None: body["interrupt_at_s"] = interrupt
    st, r = http("POST", "/api/pop/sessions", body)
    if st != 201: return None, r
    sid = r["session_id"]; t0 = time.time()
    while time.time() - t0 < timeout:
        s = http("GET", "/api/pop/sessions/" + sid)[1]
        if s.get("verification"): break
        time.sleep(1)
    else: return None, {"error": "no verdict"}
    wait_history_drained()
    return http("GET", "/api/pop/sessions/" + sid)[1], None


def frames_of(sid):
    return http("GET", f"/api/pop/history/frames?session_id={sid}&limit=1000")[1].get("frames", [])


def search(img_bytes, **q):
    qs = "&".join(f"{k}={v}" for k, v in q.items())
    return http("POST", "/api/pop/search/display" + ("?" + qs if qs else ""), img_bytes, "image/jpeg")


def proc_snapshot(name="vm_api_server"):
    st, sysj = http("GET", "/api/pop/system")
    for p in sysj.get("processes", []):
        if p["comm"].startswith(name[:15]) and p.get("cpu_jiffies", 0) > 0 or p["comm"] == name[:15]: return {"cpu_jiffies": p["cpu_jiffies"], "rss_kb": p["rss_kb"], "hwm_kb": p["hwm_kb"], "io_write_bytes": p.get("io_write_bytes", -1)}
    return {}


def delta(a, b, secs):
    if not a or not b: return {}
    return {"cpu_pct_of_one_core": round((b["cpu_jiffies"] - a["cpu_jiffies"]) / secs, 2), "rss_kb_end": b["rss_kb"], "hwm_kb": b["hwm_kb"],
            "io_write_kb_per_s": round((b["io_write_bytes"] - a["io_write_bytes"]) / 1024 / secs, 1) if b["io_write_bytes"] >= 0 else None}


def stats_of(offs):
    return [round(b - a) for a, b in zip(offs, offs[1:])]


async def main():
    os.makedirs(os.path.join(tmp, "frames")); os.makedirs(jobs); os.makedirs(root); open(events, "w").close()
    for t in range(60): display_picture("A", t).save(os.path.join(tmp, "frames", f"A_{t}.jpg"), quality=88)
    for t in range(15): display_picture("B", t).save(os.path.join(tmp, "frames", f"B_{t}.jpg"), quality=88)
    for t in range(14): display_picture("C", t).save(os.path.join(tmp, "frames", f"C_{t}.jpg"), quality=88)
    display_picture("IDLE", 0).save(os.path.join(tmp, "frames", "IDLE.jpg"), quality=88)
    ctl()

    def config(root_dir, proot_dir, hist, bsock_name="b.sock"):
        return {"visual_memory": {"enabled": True, "vector_search_enabled": True, "storage_root": root_dir, "embedding": {"enabled": True},
                "pop": {"enabled": True, "storage_root": proot_dir, "events_path": events, "jobs_dir": jobs, "poll_ms": 50, "sd_path_prefix_from": "SD:/",
                        "sd_path_prefix_to": sd + "/", "allow_test_jobs": True, "capture_backend": "resilience_grpc", "history": hist, "device_timezone": "Europe/London"},
                "display": {"enabled": True, "bridge_socket": os.path.join(tmp, bsock_name), "poll_ms": 100, "max_age_ms": 1200, "bridge_binary": BRIDGE_BIN,
                            "os_socket": "unix://" + dsock, "interest_file": os.path.join(tmp, "interest")}}}
    cfg = os.path.join(tmp, "cfg.json")
    json.dump(config(root, proot, {"enabled": True, "phase_ms": 0}), open(cfg, "w"))
    start([FAKE_BIN, "--socket", dsock, "--frames-dir", os.path.join(tmp, "frames"), "--interest-file", os.path.join(tmp, "interest"), "--control-file", os.path.join(tmp, "control.json"),
           "--stats-file", os.path.join(tmp, "stats.json")], "fake.log")
    time.sleep(1.0)
    api = start([API_BIN, "--config", cfg, "--port", str(API_PORT), "--provider", "mock:64", "--status-file", os.path.join(tmp, "st.json")], "api.log")
    dash = start([DASH_BIN, "-port", str(DASH_PORT), "-dir", os.path.join(ROOT_DIR, "dashboard"), "-vm-upstream", f"127.0.0.1:{API_PORT}"], "dash.log")
    time.sleep(2.0)

    st, hs = http("GET", "/api/pop/history/status")
    check("history status: enabled, ready, 1 FPS target, recording only during registered sessions", st == 200 and hs["enabled"] and hs["ready"] and hs["interval_ms"] == 1000 and "registered" in hs["recording_scope"], hs)
    check("history starts empty (nothing recorded while idle)", hs["store"]["rows"] == 0 and hs["counters"]["written"] == 0)

    for mid, name, slot, dur in (("ad-a", "Meet BrightSign", SLOT_A, 60), ("ad-b", "Snowy River", SLOT_A, 15), ("ad-c", "Short spot", SLOT_C, 30)):
        st, r = http("POST", "/api/pop/media", {"media_id": mid, "display_name": name, "uri": mid + ".mp4", "duration_s": dur, "slot_s": slot, "width": 1920, "height": 1080, "sha256": "0" * 64})
        check(f"register {mid}", st == 201, r)
        for prof, mk in (("screenshot", lambda i, k=mid[-1].upper(): scene(k, i * 2).resize((640, 400))), ("display_letterbox", lambda i, k=mid[-1].upper(): display_picture(k, i * 2))):
            d = os.path.join(proot, "pop", "refs_incoming", mid if prof == "screenshot" else mid + ".display_letterbox"); os.makedirs(d, exist_ok=True)
            for i in range(0, 14): mk(i).save(os.path.join(d, f"{i * 2000}.jpg"), quality=90)
            st, r = http("POST", f"/api/pop/media/{mid}/references/build?profile={prof}")
            check(f"build {prof} references for {mid}", st == 200 and r["added"] == 14, r)
    player = Player(); player.start()
    idle_rows_before = http("GET", "/api/pop/history/status")[1]["store"]["rows"]
    time.sleep(3.0)
    check("an idle presentation produces no history frames (recording is session-bound)", http("GET", "/api/pop/history/status")[1]["store"]["rows"] == idle_rows_before == 0)

    # ===== Test A: correct advertisement, 30 s ==================================================================================================
    p0, t0m = proc_snapshot(), time.time()
    sa, bad = await asyncio.to_thread(run_session, "ad-a")
    REPORT["A_host_vm_api_server_during_30s_session_with_history"] = delta(p0, proc_snapshot(), time.time() - t0m)
    REPORT["A_history_writer_latency_ms"] = http("GET", "/api/pop/history/status")[1]["latency_ms"]
    check("A: the session ran and the verifier gave its verdict", sa is not None, bad)
    sid_a = sa["session_id"]
    fa = frames_of(sid_a)
    check("A: Playback corroborated by the existing three checkpoints (verification unchanged)", sa["status"] == "playback_corroborated" and sa["verification"]["frames_ok"] == 3, (sa["status"], sa.get("verification")))
    check("A: exactly three verification checkpoints, separate from the history", len(sa["evidence"]) == 3 and all(e["capture_method"] == "resilience_grpc" for e in sa["evidence"]), len(sa["evidence"]))
    check("A: about 30 history frames stored for the 30 s play (28 to 31)", 28 <= len(fa) <= 31, len(fa))
    offs = [f["actual_offset_ms"] for f in fa]
    gaps = stats_of(offs)
    check("A: frames are about one second apart and strictly ordered in time", offs == sorted(offs) and len(set(offs)) == len(offs) and all(600 <= g <= 1500 for g in gaps), gaps)
    check("A: every frame carries a session id, timestamp, source, provenance and uncertainty", all(f["session_id"] == sid_a and f["utc_ms"] > 0 and f["source"] == "main_display" and "weston_vop2_writeback" in f["provenance"]
          and f["time_uncertainty_ms"] is not None and 0 <= f["time_uncertainty_ms"] < 1500 and f["capture_method"] == "resilience_grpc" and f["expected_media_id"] == "ad-a" and f["played_media_id"] == "ad-a" for f in fa), fa[:1])
    check("A: no frame lies outside the play (after the start guard, before the end guard)", all(f["actual_offset_ms"] >= 400 and f["actual_offset_ms"] <= sa["actual_play_ms"] - 400 for f in fa), (offs[0], offs[-1], sa["actual_play_ms"]))
    check("A: every history frame was embedded asynchronously (none pending)", all(f["embedded"] for f in fa) and http("GET", "/api/pop/history/status")[1]["store"]["pending_embed"] == 0)
    check("A: checkpoint frames that coincide with a history target reuse the checkpoint (no second JPEG/vector)", sum(1 for f in fa if f["role"] == "history_and_checkpoint") >= 1,
          [(f["actual_offset_ms"], f["role"]) for f in fa if f["role"] != "history_sample"])
    check("A: all frames can be fetched as the unchanged 640x360 JPEG", all(Image.open(io.BytesIO(urllib.request.urlopen(f"http://127.0.0.1:{API_PORT}{f['image_url']}", timeout=10).read())).size == (640, 360) for f in fa[:6]))
    on_disk = []
    for dp, dn, fn in os.walk(os.path.join(proot, "pop", "history")): on_disk += [os.path.join(dp, x) for x in fn if x.endswith(".jpg")]
    check("A: files sit in the hourly partition pop/history/YYYY/MM/DD/HH/<source>/ and match the stored hash", on_disk and all(re.search(r"pop/history/\d{4}/\d\d/\d\d/\d\d/main_display/", p) for p in on_disk))
    import hashlib
    byid = {f["frame_id"]: f for f in fa}
    ok_hash = 0
    for p in on_disk:
        h = hashlib.sha256(open(p, "rb").read()).hexdigest()
        if any(f["sha256"] == h for f in fa): ok_hash += 1
    check("A: stored sha256 equals the file on disk (original bytes, no re-encode)", ok_hash == len(on_disk) > 0, (ok_hash, len(on_disk)))
    # reference near 12 s
    ref12 = jpeg_bytes(display_picture("A", 12))
    st, r = search(ref12, top_k=5)
    h0 = (r.get("results") or [{}])[0]
    check("E: an Ad A reference from 12 s finds a captured display frame near 12 s (top hit, history sample)", st == 200 and r["results"] and h0["role"] == "history_sample" and h0["similarity"] >= 0.95
          and abs(h0["offset_in_play_ms"] / 1000 - 12.3) <= 1.6 and h0["session_id"] == sid_a, (st, [(x["role"], round(x["similarity"], 4), x["offset_in_play_ms"]) for x in r.get("results", [])]))
    check("E: the hit carries session id, capture time, offset, advertisement, uncertainty, provenance and the link to the session", all(k in h0 for k in ("session_id", "capture_utc_ms", "offset_in_play_ms", "advertisement_id", "time_uncertainty_ms", "provenance", "session_link", "image_url", "history_id")) and h0["advertisement_id"] == "ad-a" and h0["session_link"] == "#pop?session=" + sid_a, h0)
    REPORT["A_ref12_top"] = {k: h0.get(k) for k in ("similarity", "offset_in_play_ms", "capture_utc_ms", "session_id", "role")}
    for sec in (5, 20, 28):
        st, r = search(jpeg_bytes(display_picture("A", sec)), top_k=3); x = (r.get("results") or [{}])[0]
        check(f"E: an Ad A reference from {sec} s is found near {sec} s", st == 200 and x and abs(x["offset_in_play_ms"] / 1000 - (sec + 0.3)) <= 1.7 and x["similarity"] >= 0.95, [(round(y["similarity"], 3), y["offset_in_play_ms"]) for y in r.get("results", [])])
    st, r = search(jpeg_bytes(display_picture("A", 40)), top_k=5)
    check("E: an Ad A reference from 40 s (the player only played a 30 s slot) is NOT presented as a match", st == 200 and r["no_match"] and r["message"] == "No matching stored evidence"
          and "does not establish that the content was never displayed" in r["message_detail"] and r.get("closest_below_minimum", {}).get("similarity", 1) < 0.8, (r.get("message"), r.get("closest_below_minimum", {}).get("similarity")))
    REPORT["E_ad_a_40s_closest"] = r.get("closest_below_minimum", {}).get("similarity")
    st, r2 = search(jpeg_bytes(display_picture("A", 12)), top_k=50, min_similarity=0)
    ids = [(x["role"], x.get("history_id") or x.get("evidence_id")) for x in r2["results"]]
    check("A/E: no frame is counted twice when a history frame reuses a checkpoint", len(ids) == len(set(ids)) and r2["status"]["candidates_scanned"] == len(fa) + 3 - sum(1 for f in fa if f["role"] == "history_and_checkpoint"), (r2["status"]["candidates_scanned"], len(fa)))
    st, r3 = search(ref12, top_k=50, min_similarity=0, role="verification_checkpoint")
    check("E: the role filter separates verification checkpoints from history samples", st == 200 and r3["status"]["candidates_scanned"] == 3 and all(x["role"] == "verification_checkpoint" for x in r3["results"]), r3["status"])
    st, r3 = search(ref12, top_k=50, min_similarity=0, role="history_sample")
    check("E: history-only search scans only the history samples", st == 200 and r3["status"]["candidates_scanned"] == len(fa) - sum(1 for f in fa if f["role"] == "history_and_checkpoint") and all(x["role"] == "history_sample" for x in r3["results"]), r3["status"])
    st, r3 = search(ref12, top_k=50, min_similarity=0, session_id=sid_a)
    check("E: the playback-session filter restricts the search to one session", st == 200 and all(x["session_id"] == sid_a for x in r3["results"]) and r3["status"]["candidates_scanned"] > 0)
    st, r3 = search(ref12, session_id="s-nosuch")
    check("E: an unknown session filter finds nothing", st == 200 and r3["no_match"] and r3["status"]["candidates_scanned"] == 0)
    st, r3 = search(ref12, role="bogus"); check("API: an invalid role is rejected", st == 400, r3)
    t_mid = fa[len(fa) // 2]["utc_ms"]
    st, r3 = search(ref12, top_k=50, min_similarity=0, start=t_mid, end=t_mid + 5000)
    check("E: the capture-time window restricts the candidates (half-open)", st == 200 and 4 <= r3["status"]["candidates_scanned"] <= 7 and all(t_mid <= x["capture_utc_ms"] < t_mid + 5000 for x in r3["results"]), r3["status"])

    # ===== Test B: wrong advertisement (expected A, B plays for 30 s, 15 s loop) ===========================================================================
    sb, bad = await asyncio.to_thread(run_session, "ad-a", "ad-b")
    check("B: the session ran", sb is not None, bad); sid_b = sb["session_id"]; fb = frames_of(sid_b)
    check("B: the D5 verifier still reports Mismatch (three checkpoints)", sb["status"] == "mismatch" and sb["verification"]["frames_ok"] == 3, (sb["status"], sb.get("reason")))
    check("B: about 30 history frames were stored", 28 <= len(fb) <= 31, len(fb))
    check("B: history rows say what was expected (ad-a) and what actually played (ad-b)", all(f["expected_media_id"] == "ad-a" and f["played_media_id"] == "ad-b" for f in fb), fb[:1])
    st, r = search(jpeg_bytes(display_picture("B", 5)), top_k=10)
    res = r["results"]
    check("B: an Ad B reference finds the Weston frames of Ad B in this session, labelled with the advertisement that really played", st == 200 and res and all(x["session_id"] == sid_b for x in res[:4]) and res[0]["advertisement_id"] == "ad-b"
          and res[0]["session_status"] == "mismatch", [(x["session_id"][-6:], x["advertisement_id"], round(x["similarity"], 3), x["offset_in_play_ms"]) for x in res[:6]])
    st, rb = search(jpeg_bytes(display_picture("B", 5)), top_k=50, min_similarity=0.95, session_id=sid_b)
    offs_b = sorted(x["offset_in_play_ms"] / 1000 for x in rb["results"])
    check("B: the 15 s loop boundary: the reference from 5 s of the loop is found at about 5 s AND at about 20 s (both loops)", st == 200 and any(abs(o - 5.3) <= 1.7 for o in offs_b) and any(abs(o - 20.3) <= 1.7 for o in offs_b), offs_b)
    st, r = search(jpeg_bytes(display_picture("A", 5)), top_k=50, min_similarity=0.8)
    check("B: no Ad B history frame is returned as a match for an Ad A reference (nothing is falsely labelled Ad A)", st == 200 and all(x["session_id"] != sid_b for x in r["results"]), [(x["session_id"][-6:], round(x["similarity"], 3)) for x in r["results"] if x["session_id"] == sid_b])
    st, r = search(jpeg_bytes(display_picture("B", 0)), top_k=10, session_id=sid_b)
    n0 = sorted(x["offset_in_play_ms"] / 1000 for x in r["results"][:3])
    check("B: loop start frames (0 s) appear at about 0, 15 and 30 s", st == 200 and any(abs(o - 15.3) <= 1.7 for o in n0) and any(o <= 2.0 for o in n0), n0)

    # ===== Test C: interrupted after about 10 s ==================================================================================================
    sc, bad = await asyncio.to_thread(run_session, "ad-a", None, 10)
    check("C: the session ran", sc is not None, bad); sid_c = sc["session_id"]
    fc = frames_of(sid_c)
    check("C: D5 still reports Incomplete", sc["status"] == "incomplete", (sc["status"], sc.get("reason")))
    check("C: about 9 to 10 history frames, all from the real playing interval", 7 <= len(fc) <= 11 and all(f["actual_offset_ms"] <= sc["actual_play_ms"] - 400 for f in fc), (len(fc), [f["actual_offset_ms"] for f in fc][-3:], sc["actual_play_ms"]))
    n_after = len(fc); time.sleep(6.0)
    check("C: no further frames are assigned to the session after its end (idle loop frames are not attributed to it)", len(frames_of(sid_c)) == n_after)
    st, r = search(jpeg_bytes(display_picture("IDLE", 0)), top_k=10, min_similarity=0.9, session_id=sid_c)
    check("C: the idle-presentation picture is not found in the interrupted session", st == 200 and r["no_match"], [(round(x["similarity"], 3)) for x in r.get("results", [])])
    hs = http("GET", "/api/pop/history/status")[1]
    check("C: the writer dropped (or never took) frames from after the end", hs["counters"]["dropped_after_end"] >= 0 and hs["store"]["rows"] == sum(len(frames_of(s)) for s in (sid_a, sid_b, sid_c)), hs["counters"])

    # ===== Test D: the producer stalls in the middle of the play (ad-c, 14 s slot, stall 4 s .. 9 s) ============================================================
    player.stall = (4.0, 9.0)
    sd_, bad = await asyncio.to_thread(run_session, "ad-c", None, None, (2, 7, 12))
    player.stall = None
    check("D: the session ran and a verdict was given", sd_ is not None, bad); sid_d = sd_["session_id"]; fd = frames_of(sid_d)
    gap_frames = [f for f in fd if 5200 <= f["actual_offset_ms"] <= 9000]
    check("D: no stale frame became history evidence during the stall (no frame captured 1.2 s after the stall began until it ended)", not gap_frames, [f["actual_offset_ms"] for f in gap_frames])
    check("D: history exists for the seconds before and after the stall, so the stall is a gap, not a fabricated stream", any(f["actual_offset_ms"] < 5000 for f in fd) and any(f["actual_offset_ms"] > 9000 for f in fd), [f["actual_offset_ms"] for f in fd])
    hd = (sd_.get("history") or {})
    hs = http("GET", "/api/pop/history/status")[1]
    check("D: the gap is counted (seconds without a usable frame) and the session history is flagged partial by its own numbers", hs["counters"]["missed_no_frame"] >= 3 and hd.get("frames") == len(fd) and len(fd) < 0.9 * ((sd_["actual_play_ms"] - 500) // 1000 + 1), (hs["counters"]["missed_no_frame"], hd, len(fd)))
    check("D: the checkpoint inside the stall failed honestly and the verdict reflects it (no usable evidence is invented)", any(e["status"] == "failed" for e in sd_["evidence"]), [(e["idx"], e["status"]) for e in sd_["evidence"]])
    check("D: normal presentation resumed after the test (IDLE picture on the display)", await asyncio.to_thread(lambda: json.load(open(os.path.join(tmp, "control.json")))["frame"] == "IDLE"))

    # ===== dashboard ================================================================================================================================
    st, sd2 = http("GET", "/api/pop/sessions/" + sid_a)
    check("API: session detail summarises the history (frames, embedded, first/last offset) and links to the frame list", sd2["history"]["frames"] == len(fa) and sd2["history"]["embedded"] == len(fa) and sd2["history"]["frames_url"].endswith(sid_a), sd2.get("history"))
    st, cov = http("GET", "/api/pop/search/display")
    check("API: coverage splits checkpoints and history and reports recording", st == 200 and cov["history_rows"] > 0 and cov["checkpoint_rows"] > 0 and cov["history_recording"] is True and cov["searchable_rows"] == cov["checkpoint_rows"] + cov["history_rows"], cov)
    st, r = http("GET", "/api/pop/history/frames?limit=0"); check("API: invalid limit rejected", st == 400)
    st, r = http("GET", "/api/pop/history/99999/image"); check("API: unknown frame is a 404", st == 404)
    st, r = http("POST", "/api/pop/history/status", b"{}"); check("API: history routes are read-only", st in (403, 405), st)
    ps = http("GET", "/api/pop/status")[1]
    check("API: pop status reports the history block", ps["history"]["enabled"] is True and ps["history"]["ready"] is True)


    # ===== Test G (API): time-based lookup of stored display frames ============================================================================
    def iso(ms): return time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(ms // 1000)) + ".%03dZ" % (ms % 1000)
    def near(t, tol=500, sid=None, raw=False):
        return http("GET", "/api/display/history/nearest?time=" + (t if raw else urllib.parse.quote(t)) + "&max_diff_ms=%d" % tol + (("&session_id=" + sid) if sid else ""))
    def tsearch(t, w=10, extra=""): return http("GET", "/api/display/history/search?time=%s&window_s=%d%s" % (urllib.parse.quote(t), w, extra))
    def fetch(url):
        try:
            with urllib.request.urlopen("http://127.0.0.1:%d%s" % (API_PORT, url), timeout=20) as r: return r.status, r.read()
        except urllib.error.HTTPError as e: return e.code, e.read()
    fa_s = sorted(fa, key=lambda f: f["utc_ms"]); fk = fa_s[10]
    dbp = os.path.join(proot, "pop.db")
    sha_before = hashlib.sha256(open(dbp, "rb").read()).hexdigest()
    st, r = near(iso(fk["utc_ms"] + 120))
    fr = r.get("frame") or {}
    check("G: nearest returns the one stored frame closest to the requested time", st == 200 and r["found"] and fr["frame_id"] == fk["frame_id"] and fr["time_diff_ms"] == -120, r)
    check("G: requested and recorded timestamps are UTC (ISO-8601 Z and epoch ms) and agree", fr["recorded_utc_ms"] == fk["utc_ms"] and fr["recorded_utc"] == iso(fk["utc_ms"]) and fr["requested_utc"] == iso(fk["utc_ms"] + 120) and fr["requested_utc_ms"] == fk["utc_ms"] + 120, fr)
    check("G: time difference, timing uncertainty and timestamp quality are reported", fr["time_diff_ms"] == -120 and fr["time_uncertainty_ms"] == fk["time_uncertainty_ms"] and fr["timestamp_quality"] in ("bounded", "unknown") and "timing_note" in fr, fr)
    check("G: provenance says Weston/VOP2 display output, with the capture method", "Weston/VOP2" in fr["provenance"]["origin"] and fr["provenance"]["capture_method"] == "resilience_grpc", fr["provenance"])
    sess = r["session"]
    check("G: the playback session, expected advertisement and the advertisement the player reported are returned", fr["session_id"] == sid_a and sess["playback"]["expected_media_id"] == "ad-a" and sess["playback"]["reported_media_id"] == "ad-a" and sess["playback"]["player_reported"] is True, sess["playback"])
    check("G: the verification result is separate from the player's report (D5 verdict: corroborated)", sess["visual_verification"]["status"] == "playback_corroborated" and sess["visual_verification"]["visually_corroborated"] is True and sess["visual_verification"]["checkpoints"]["matched"] == 3, sess["visual_verification"])
    check("G: the playback-relative offset equals the stored offset into the play", fr["playback_offset_ms"] == fk["actual_offset_ms"] and 9000 <= fr["playback_offset_ms"] <= 12000, fr["playback_offset_ms"])
    check("G: image dimensions and availability are reported", fr["width"] == 640 and fr["height"] == 360 and fr["image"]["available"] is True and fr["image"]["state"] == "available", fr["image"])
    sc, body = fetch(fr["image"]["url"])
    im = Image.open(io.BytesIO(body)) if sc == 200 else None
    check("G: the JPEG is served by a same-origin endpoint, separate from the JSON, as the unchanged 640x360 JPEG", sc == 200 and im is not None and im.size == (640, 360) and hashlib.sha256(body).hexdigest() == fk["sha256"], (sc, fr["image"]))
    check("G: the frame's similarity to the expected advertisement comes from the stored vector (no model run)", fr["similarity"]["available"] and fr["similarity"]["similarity_to_expected"] > 0.8 and fr["similarity"]["matches_expected_reference"] is True, fr["similarity"])
    check("G: the response states that only display output is searched and no model is used", "audience-camera" in r["source_scope"] and "no model" in r["query"], (r["source_scope"], r["query"]))
    # ISO-8601 forms of the same instant
    plus1 = datetime.datetime.fromtimestamp(fk["utc_ms"] / 1000, datetime.timezone(datetime.timedelta(hours=1))).strftime("%Y-%m-%dT%H:%M:%S") + ".%03d+01:00" % (fk["utc_ms"] % 1000)
    minus330 = datetime.datetime.fromtimestamp(fk["utc_ms"] / 1000, datetime.timezone(datetime.timedelta(hours=-3, minutes=-30))).strftime("%Y-%m-%dT%H:%M:%S") + ".%03d-03:30" % (fk["utc_ms"] % 1000)
    st1, r1 = near(plus1); st2, r2 = near(minus330); st3, r3 = near(plus1, raw=True)      # raw: "+" arrives as a space, as with an unencoded query string
    check("G: other UTC offsets (+01:00, -03:30, even an unencoded '+') resolve to the same instant and frame", all(x == 200 and y["found"] and y["frame"]["frame_id"] == fk["frame_id"] and y["frame"]["time_diff_ms"] == 0 for x, y in ((st1, r1), (st2, r2), (st3, r3))), (r1.get("error"), r2.get("error"), r3.get("error"), r3.get("message")))
    st, r = near("2026-11-07T14:30:00"); check("G: a wall-clock time without an offset is refused (ambiguous), not guessed", st == 400 and "offset" in r["message"], r)
    st, r = near("yesterday"); check("G: a malformed timestamp is a 400", st == 400, r)
    st, r = near("2026-02-30T10:00:00Z"); check("G: an impossible calendar day is a 400", st == 400, r)
    st, r = http("GET", "/api/display/history/nearest"); check("G: a missing time is a 400", st == 400, r)
    st, r = near(iso(fk["utc_ms"]), tol=4000000); check("G: an oversized tolerance is a 400", st == 400, r)
    st, r = http("GET", "/api/display/history/nearest?time=%s&max_diff_ms=-1" % urllib.parse.quote(iso(fk["utc_ms"]))); check("G: a negative tolerance is a 400", st == 400, r)
    st, r = http("POST", "/api/display/history/nearest", b"{}"); check("G: the lookup is GET only", st in (403, 405), st)
    # determinism: exactly between two frames -> the earlier; the same answer every time
    a1, a2 = fa_s[11], fa_s[12]; mid = a1["utc_ms"] + (a2["utc_ms"] - a1["utc_ms"]) // 2
    ans = [near(iso(mid), tol=2000)[1]["frame"]["frame_id"] for _ in range(4)]
    if (a2["utc_ms"] - a1["utc_ms"]) % 2 == 0:
        check("G: an exact tie resolves to the earlier frame, every time", set(ans) == {a1["frame_id"]}, ans)
    else:
        check("G: the choice is the same every time (odd gap: the nearer frame wins)", len(set(ans)) == 1, ans)
    st, r = near(iso(mid), tol=100)   # mid = halfway between two frames, about 500 ms from both
    check("G: when no frame is within the tolerance the answer is a clear no-evidence response, with the nearest retained frame named", st == 200 and r["found"] is False and r["evidence_state"] == "no_evidence_in_tolerance" and "frame" not in r and r["nearest_outside_tolerance"]["time_diff_ms"] in range(-1100, 1100), r)
    check("G: that response never claims that nothing was displayed", "does not establish that nothing was displayed" in r["message"] and "between the stored frames of a reported session" in r["message"] and "idle presentation" not in r["message"], r["message"])
    first = min(f["utc_ms"] for f in frames_of(sid_a) + fb)
    st, r = near(iso(first - 7 * 86400000))
    check("G: a time before the oldest retained frame says so (it may have been removed by retention)", st == 200 and not r["found"] and "earlier than the oldest retained frame" in r["message"] and "retention" in r["message"] and r["coverage"]["frames_retained"] > 0, r.get("message"))
    st, r = near(iso(int(time.time() * 1000) + 86400000))
    check("G: a future time says it is later than the newest retained frame", st == 200 and not r["found"] and "later than the newest retained frame" in r["message"], r.get("message"))
    # wrong advertisement and interrupted sessions
    fb_s = sorted(fb, key=lambda f: f["utc_ms"]); st, r = near(iso(fb_s[8]["utc_ms"]))
    check("G: a wrong-advertisement session: expected ad-a, the player reported ad-b, the stored frames contradict the schedule", st == 200 and r["found"] and r["session"]["playback"]["expected_media_id"] == "ad-a" and r["session"]["playback"]["reported_media_id"] == "ad-b" and r["session"]["visual_verification"]["status"] == "mismatch" and r["session"]["visual_verification"]["visual_verdict"] == "contradicts_expected" and r["frame"]["similarity"]["matches_expected_reference"] is False, r.get("session"))
    check("G: the player-reported advertisement is not presented as visually corroborated when the verdict is a mismatch", r["session"]["playback"]["reported_matches_schedule"] is False and r["session"]["visual_verification"]["visually_corroborated"] is False)
    fc_s = sorted(frames_of(sid_c), key=lambda f: f["utc_ms"]); sc_end = http("GET", "/api/pop/sessions/" + sid_c)[1]["end_utc_ms"]
    st, r = near(iso(sc_end + 3000), tol=800, sid=sid_c)
    check("G: after an interrupted session ended no frame is attributed to it (the session filter finds nothing)", st == 200 and r["found"] is False, r.get("message"))
    st, r = near(iso(fc_s[3]["utc_ms"]), sid=sid_c); check("G: an interrupted session is reported as Incomplete", st == 200 and r["found"] and r["session"]["visual_verification"]["status"] == "incomplete" and r["session"]["visual_verification"]["visually_corroborated"] is False, r.get("session"))
    # window search
    st, w = tsearch(iso(fk["utc_ms"]), 5)
    sw = [x for x in w["sessions"] if x["session_id"] == sid_a][0]
    offs_w = [f["playback_offset_ms"] for f in sw["frames"]]
    check("G: the window search lists the overlapping session with every stored frame in the window, in time order", st == 200 and 9 <= len(sw["frames"]) <= 11 and offs_w == sorted(offs_w) and sw["frames_in_window"] == len(sw["frames"]) and sw["evidence_state"] == "frames_retained", (len(sw["frames"]), offs_w))
    check("G: every window frame carries time, offset, image state and a similarity score", all(f["recorded_utc"] and f["similarity"]["available"] and f["image"]["available"] and isinstance(f["playback_offset_ms"], int) for f in sw["frames"]))
    check("G: the nearest frame of the session is identified", sw["frames"][sw["nearest_frame_index"]]["frame_id"] == fk["frame_id"], sw["nearest_frame_index"])
    check("G: player-reported playback and visual verification are separate blocks of the session", sw["playback"]["basis"].startswith("reported by the player") and "verification checkpoints" in sw["visual_verification"]["basis"] and sw["visual_verification"]["status"] == "playback_corroborated", (sw["playback"]["basis"], sw["visual_verification"]["basis"]))
    check("G: the window search states the requested time's offset inside the reported play", sw["playback"]["requested_within_reported_play"] is True and 9000 <= sw["playback"]["requested_offset_ms"] <= 12500, sw["playback"])
    st, w2 = tsearch(iso(first - 7 * 86400000), 30)
    check("G: a window with no retained evidence and no reported session says 'no evidence' and does not claim nothing was displayed", st == 200 and w2["evidence_state"] == "no_evidence" and w2["sessions"] == [] and "does not establish that nothing was displayed" in w2["message"], w2.get("message"))
    st, w3 = tsearch(iso(fk["utc_ms"]), 5, "&limit=3")
    sw3 = [x for x in w3["sessions"] if x["session_id"] == sid_a][0]
    check("G: the per-session frame limit keeps the frames nearest the requested time and flags truncation", len(sw3["frames"]) == 3 and sw3["frames_truncated"] is True and sw3["frames_in_window"] > 3 and any(f["frame_id"] == fk["frame_id"] for f in sw3["frames"]), (len(sw3["frames"]), sw3["frames_truncated"]))
    st, r = http("GET", "/api/display/history/search?time=%s&window_s=0" % urllib.parse.quote(iso(fk["utc_ms"]))); check("G: a zero window is a 400", st == 400, r)
    st, inf = http("GET", "/api/display/history/info")
    check("G: info reports the device timezone, retention and the retained range (UTC)", st == 200 and inf["device_timezone"]["name"] == "Europe/London" and inf["display_frames"]["retained"] > 0 and inf["display_frames"]["oldest_utc"].endswith("Z") and inf["history"]["jpeg_retention_days"] == 7 and "UTC" in inf["storage"], inf)
    sha_after = hashlib.sha256(open(dbp, "rb").read()).hexdigest()
    check("G: all those lookups were read-only (pop.db is byte-identical)", sha_before == sha_after, (sha_before[:12], sha_after[:12]))
    check("G: every frame returned is a Weston display frame (never the audience camera)", all(f["provenance"]["capture_method"] == "resilience_grpc" for x in w["sessions"] for f in x["frames"]))
    # retention removed one JPEG (what the pruner does: file removed, image flag cleared, row and vector kept)
    fx = fa_s[5]; row_path = None
    cx = sqlite3.connect(dbp, timeout=20); cur = cx.cursor()
    row_path = cur.execute("SELECT path FROM display_history_frames WHERE frame_id=? AND evidence_id IS NULL", (fx["frame_id"],)).fetchone()
    if row_path is None:
        fx = [f for f in fa_s if not f.get("verification_checkpoint_evidence_id")][5]; row_path = cur.execute("SELECT path FROM display_history_frames WHERE frame_id=?", (fx["frame_id"],)).fetchone()
    os.remove(os.path.join(proot, row_path[0])); cur.execute("UPDATE display_history_frames SET image_available=0 WHERE frame_id=?", (fx["frame_id"],)); cx.commit(); cx.close()
    st, r = near(iso(fx["utc_ms"]))
    check("G: when retention removed the JPEG the metadata is returned and the image is reported as expired", st == 200 and r["found"] and r["evidence_state"] == "frame_found_image_expired" and r["frame"]["image"]["available"] is False and r["frame"]["image"]["state"] == "expired" and r["frame"]["image"]["url"] is None and r["frame"]["session_id"] == sid_a and r["frame"]["similarity"]["available"], r.get("frame"))
    sc, body = fetch("/api/pop/history/%d/image" % fx["frame_id"]); check("G: the image endpoint answers 410 for the expired JPEG", sc == 410, sc)

    chrome = start(["/usr/bin/google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu", f"--remote-debugging-port={CDP_PORT}", f"--user-data-dir={OUT}/profile",
                    "--window-size=1440,2400", "about:blank"], "chrome.log")
    time.sleep(2.5)
    tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{CDP_PORT}/json"))
    ws_url = [t for t in tabs if t["type"] == "page"][0]["webSocketDebuggerUrl"]
    async with websockets.connect(ws_url, max_size=None) as ws:
        n = 0; pend = {}

        async def reader():
            async for m in ws:
                dd = json.loads(m)
                if "id" in dd and dd["id"] in pend: pend[dd["id"]].set_result(dd)
                elif dd.get("method") == "Runtime.exceptionThrown": logs.append("EXC " + json.dumps(dd["params"]["exceptionDetails"])[:300])
                elif dd.get("method") == "Runtime.consoleAPICalled" and dd["params"]["type"] == "error":
                    logs.append("CONSOLE " + json.dumps([a.get("value", a.get("description")) for a in dd["params"]["args"]])[:300])
        asyncio.create_task(reader())

        async def cmd(method, **p):
            nonlocal n; n += 1; f = asyncio.get_event_loop().create_future(); pend[n] = f
            await ws.send(json.dumps({"id": n, "method": method, "params": p})); r = await f; return r.get("result", r)

        async def ev(js):
            r = await cmd("Runtime.evaluate", expression=js, awaitPromise=True, returnByValue=True)
            if "exceptionDetails" in r: logs.append("EVALERR " + json.dumps(r["exceptionDetails"])[:200] + " :: " + js[:160])
            return r.get("result", {}).get("value")

        async def shot(name):
            r = await cmd("Page.captureScreenshot", format="png", captureBeyondViewport=True); open(f"{OUT}/{name}.png", "wb").write(base64.b64decode(r["data"]))

        async def wait_for(js, timeout=15):
            t0 = time.time()
            while time.time() - t0 < timeout:
                if await ev(js): return True
                await asyncio.sleep(0.25)
            return False

        async def upload(data):
            b64 = base64.b64encode(data).decode()
            return await ev("""(()=>{const bin=atob(%s);const u=new Uint8Array(bin.length);for(let i=0;i<bin.length;i++)u[i]=bin.charCodeAt(i);
              const dt=new DataTransfer();dt.items.add(new File([u],'ref.jpg',{type:'image/jpeg'}));
              const inp=document.querySelector('#view-search input[type=file][aria-label*="display search"]');
              inp.files=dt.files;inp.dispatchEvent(new Event('change'));return true})()""" % json.dumps(b64))

        await cmd("Page.enable"); await cmd("Runtime.enable"); await cmd("Emulation.setDeviceMetricsOverride", width=1440, height=2400, deviceScaleFactor=1, mobile=False)
        await cmd("Page.navigate", url=BASE + "?t=%d#search?source=display" % int(time.time() * 1000))
        check("UI: the Display History panel states recording is enabled at 1 FPS during registered sessions", await wait_for(
            "(()=>{const t=(document.querySelector('[data-role=history-panel]')||{}).innerText||'';return /Recording enabled/.test(t)&&/1\\.00 FPS target/.test(t)&&/registered Proof-of-Play session/.test(t)})()", 20), await ev("(document.querySelector('[data-role=history-panel]')||{}).innerText"))
        pt = await ev("document.querySelector('[data-role=history-panel]').innerText.replace(/\\n+/g,' | ')")
        REPORT["ui_history_panel"] = pt
        check("UI: the panel shows searchable frame counts, dropped/failed, storage, embedding status and the latest capture time",
              all(s in pt for s in ("Searchable display frames", "checkpoints", "history", "Dropped / failed", "Storage", "quota", "Embedding", "pending", "Latest captured display frame")), pt)
        check("UI: the role filter and the session filter are offered", await ev("!!document.querySelector('#view-search select[aria-label=\"Frame role\"]') && !!document.querySelector('#view-search input[aria-label=\"Playback session id\"]')") is True)
        await upload(ref12)
        await wait_for("[...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Search display evidence') && ![...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Search display evidence').disabled", 10)
        await ev("([...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Search display evidence').click(), 1)")
        check("UI: results show cards", await wait_for("document.querySelectorAll('#view-search .ds-hit').length>=3", 20))
        await ev("(async()=>{for(const c of document.querySelectorAll('#view-search .ds-hit')){c.scrollIntoView();await new Promise(r=>setTimeout(r,120));}window.scrollTo(0,0);return 1})()")
        card = await ev("(()=>{const c=document.querySelector('#view-search .ds-hit');return c?c.innerText.replace(/\\n+/g,' | '):''})()")
        REPORT["ui_first_card"] = card
        check("UI: the top card is a 'History sample' with similarity, capture time, offset in the play, advertisement and session link", all(s.lower() in card.lower() for s in ("History sample", "similarity", "Captured", "Within the play", "Meet BrightSign", "Open playback session")), card)
        check("UI: images decode (reference and stored frame)", await wait_for("[...document.querySelectorAll('#view-search .ds-hit')].slice(0,3).every(c=>[...c.querySelectorAll('img')].length===2&&[...c.querySelectorAll('img')].every(i=>i.complete&&i.naturalWidth>0))", 15))
        await shot("1_display_search_history")
        # click-to-enlarge on a result card, then "Search with this image" makes the stored frame the reference
        await ev("(document.querySelector('#view-search .ds-hit .vm-thumb:nth-child(1)'), document.querySelectorAll('#view-search .ds-hit')[0].querySelectorAll('img[data-zoom]')[1].click(), 1)")
        check("UI: clicking a stored frame opens the large viewer with its details and a link to the original JPEG", await wait_for(
            "(()=>{const b=document.querySelector('.lb-bg');if(!b)return false;const i=b.querySelector('.lb-stage img');return !!i&&i.complete&&i.naturalWidth===640&&/History sample|Verification checkpoint/.test(b.innerText)&&/session /.test(b.innerText)&&!!b.querySelector('a[href*=\"/api/pop/\"]')})()", 10), await ev("(document.querySelector('.lb-bg')||{}).innerText"))
        await shot("1b_viewer")
        await ev("document.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowRight',bubbles:true})), 1")
        check("UI: the arrow keys step to the next image (counter changes)", await wait_for("/3 \\/ \\d+/.test(document.querySelector('.lb-bg').innerText)", 5), await ev("(document.querySelector('.lb-bg')||{}).innerText"))
        await ev("(document.querySelector('[data-role=lb-search]').click(), 1)")
        check("UI: 'Search with this image' closes the viewer and loads the stored frame as the search reference", await wait_for(
            "!document.querySelector('.lb-bg') && /stored-frame\\.jpg/.test(document.getElementById('view-search').innerText)", 10), await ev("document.getElementById('view-search').innerText.slice(0,400)"))
        await ev("([...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Search display evidence').click(), 1)")
        check("UI: searching with that stored frame finds itself first (similarity about 1.0)", await wait_for(
            "(()=>{const t=document.querySelector('#view-search .ds-hit .vm-score');return !!t&&parseFloat(t.textContent.replace('similarity ',''))>0.999})()", 20), await ev("(document.querySelector('#view-search .ds-hit .vm-score')||{}).textContent"))
        await ev("(document.querySelectorAll('#view-search .ds-hit')[2].querySelector('[data-role=search-with-frame]').click(), 1)")
        check("UI: each result card also has 'Search with this frame' which replaces the reference", await wait_for("/stored-frame\\.jpg/.test(document.querySelector('#view-search [data-role]') ? document.getElementById('view-search').innerText : '')", 10))
        await ev("document.querySelector('#view-search select[aria-label=\"Frame role\"]').value='verification_checkpoint'; document.querySelector('#view-search select[aria-label=\"Frame role\"]').dispatchEvent(new Event('change')); ([...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Search display evidence').click(), 1)")
        check("UI: the checkpoint-only filter shows only 'Verification checkpoint' cards", await wait_for("(()=>{const r=[...document.querySelectorAll('#view-search .ds-hit [data-role=frame-role]')].map(x=>x.textContent);return r.length>0&&r.every(x=>x==='Verification checkpoint')})()", 20))
        await ev("document.querySelector('#view-search select[aria-label=\"Frame role\"]').value='any'; ([...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Search display evidence').click(), 1)")
        await upload(jpeg_bytes(display_picture("A", 40)))
        await wait_for("![...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Search display evidence').disabled", 5)
        await ev("([...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Search display evidence').click(), 1)")
        check("UI: a reference from a moment that was never stored shows 'No matching stored evidence' and 'does not establish that the content was never displayed'",
              await wait_for("(()=>{const t=(document.querySelector('#view-search .ds-nomatch')||{}).innerText||'';return /No matching stored evidence/.test(t)&&/does not establish that the content was never displayed/.test(t)})()", 20), await ev("(document.querySelector('#view-search .ds-nomatch')||{}).innerText"))
        await shot("2_no_match")
        # session detail with the history strip
        await cmd("Page.navigate", url=BASE + "?t=%d#pop" % int(time.time() * 1000))
        await wait_for("document.body.innerText.indexOf('Proof of Play')>=0", 15)
        await ev(f"(window.VMUI && window.VMUI.pop && window.VMUI.pop.openDetail ? (window.VMUI.pop.openDetail({json.dumps(sid_a)}), 1) : 0)")
        opened = await wait_for("!!document.querySelector('[data-role=history-section]')", 6)
        if not opened:
            await cmd("Page.navigate", url=BASE + "?t=%d#pop?session=%s" % (int(time.time() * 1000), sid_a))
            opened = await wait_for("!!document.querySelector('[data-role=history-section]')", 20)
        check("UI: the session detail separates 'Verification checkpoints (used for the verdict)' from 'Display history' frames", opened and await ev("(()=>{const t=document.body.innerText;return /verification checkpoints/i.test(t)&&/display history: additional searchable frames/i.test(t)&&/never changes the result/i.test(t)})()") is True)
        check("UI: the history strip lists the session's frames as thumbnails with their offsets", await wait_for("document.querySelectorAll('[data-role=history-section] figure').length>=%d" % (len(fa) - 1), 15), await ev("document.querySelectorAll('[data-role=history-section] figure').length"))
        await ev("(document.querySelector('[data-role=history-section] figure img').click(), 1)")
        check("UI: a history thumbnail in the session window opens the large viewer", await wait_for("!!document.querySelector('.lb-bg .lb-stage img') && /Display history \\+/.test(document.querySelector('.lb-bg').innerText)", 10))
        await ev("document.querySelector('.lb-bg .vm-btn.small').click(), 1")
        await ev("(async()=>{for(const f of document.querySelectorAll('[data-role=history-section] figure')){f.scrollIntoView();await new Promise(r=>setTimeout(r,40));}return 1})()")
        await asyncio.sleep(1.0)
        await shot("3_session_history")
        await ev("document.querySelectorAll('.vm-modal-head button').forEach(b=>b.click()), 1")
        await cmd("Page.navigate", url=BASE + "?t=%d#pop?session=%s" % (int(time.time() * 1000), sid_d))
        check("UI: a session with a gap shows the 'Partial history' warning", await wait_for("/Partial history/.test((document.body||{}).innerText||'')", 20), await ev("((document.body||{}).innerText||'').slice(0,300)"))
        await shot("4_partial_history")

        # ----- Test G (UI): Search by time -----------------------------------------------------------------------------------------------------
        await ev("document.querySelectorAll('.vm-modal-head button').forEach(b=>b.click()), 1")
        await cmd("Page.navigate", url=BASE + "?t=%d#search?source=display" % int(time.time() * 1000))
        check("UI/G: the Search by time panel is on the BrightSign Display search page with date, time, timezone, window and tolerance", await wait_for(
            "(()=>{const p=document.querySelector('[data-role=time-search]');return !!p&&!!p.querySelector('[data-role=ts-date]')&&!!p.querySelector('[data-role=ts-time]')&&!!p.querySelector('[data-role=ts-tz]')&&!!p.querySelector('[data-role=ts-window]')&&!!p.querySelector('[data-role=ts-tol]')&&/Europe\\/London/.test(p.innerText)})()", 20),
            await ev("((document.querySelector('[data-role=time-search]')||{}).innerText||'').slice(0,300)"))
        JS_SET = """(args)=>{const q=s=>document.querySelector('[data-role=time-search] '+s);
          const set=(el,v)=>{el.value=v;el.dispatchEvent(new Event('input',{bubbles:true}));el.dispatchEvent(new Event('change',{bubbles:true}));};
          set(q('[data-role=ts-tz]'),args.tz); if(args.other!==undefined) set(q('[data-role=ts-tz-other]'),args.other);
          set(q('[data-role=ts-date]'),args.date); set(q('[data-role=ts-time]'),args.time); if(args.win) set(q('[data-role=ts-window]'),args.win); if(args.tol) set(q('[data-role=ts-tol]'),args.tol);
          return true}"""
        async def set_time(tz, ms, other=None, win=None, tol=None):
            z = "UTC" if tz == "utc" else (other if tz == "other" else "Europe/London")
            d = datetime.datetime.fromtimestamp(ms // 1000, zoneinfo.ZoneInfo(z))
            a = {"tz": tz, "date": d.strftime("%Y-%m-%d"), "time": d.strftime("%H:%M:%S")}
            if other is not None: a["other"] = other
            if win: a["win"] = str(win)
            if tol: a["tol"] = str(tol)
            await ev("(%s)(%s)" % (JS_SET, json.dumps(a)))
        async def run_ts():
            await ev("(document.querySelector('[data-role=ts-run]').click(), 1)")
            return await wait_for("!!document.querySelector('[data-role=nearest-frame],[data-role=no-evidence]')&&!document.querySelector('[data-role=time-search] .vm-spin:not([hidden])')", 25)
        # (1) device timezone (Europe/London, BST/GMT aware): the instant is resolved correctly and the nearest frame is shown with everything
        t_sec = (fk["utc_ms"] // 1000) * 1000
        await set_time("device", t_sec, win=10, tol=2000)
        resolved = await ev("document.querySelector('[data-role=resolved-utc]').textContent")
        check("UI/G: a London wall-clock time is converted to the right UTC instant", resolved == iso(t_sec).replace(".000Z", ".000Z"), (resolved, iso(t_sec)))
        expect_near = near(iso(t_sec), 2000)[1]["frame"]
        check("UI/G: the lookup runs and shows the nearest stored display frame", await run_ts(), await ev("(document.querySelector('[data-role=time-search]').innerText||'').slice(0,400)"))
        await ev("(async()=>{for(const f of document.querySelectorAll('[data-role=time-search] figure, [data-role=nearest-frame] img')){f.scrollIntoView();await new Promise(r=>setTimeout(r,40));}return 1})()")
        card = await ev("document.querySelector('[data-role=nearest-frame]').innerText.replace(/\\n+/g,' | ')")
        REPORT["ui_time_search_nearest_card"] = card
        check("UI/G: the card shows the frame id, UTC and zone times, difference, uncertainty, offset, source, size and similarity", all(x in card for x in (("history frame #%d" % expect_near["frame_id"]), "Recorded (UTC)", "Recorded (selected zone)", "Time difference", "Timestamp quality", "Within the play", "Weston/VOP2", "640 × 360", "Similarity")), card)
        check("UI/G: the stored JPEG is displayed", await wait_for("(()=>{const i=document.querySelector('[data-role=nearest-frame] img');return !!i&&i.complete&&i.naturalWidth===640&&i.naturalHeight===360})()", 15))
        check("UI/G: player-reported playback and visual verification are two separate, labelled blocks", await ev("(()=>{const r=document.querySelector('[data-role=nearest-frame] [data-role=player-reported]'),v=document.querySelector('[data-role=nearest-frame] [data-role=visual-verification]');return !!r&&!!v&&r!==v&&/Reported by the player/.test(r.innerText)&&/Seen on the display/.test(v.innerText)&&r.querySelector('[data-role=expected-ad]').textContent==='ad-a'&&r.querySelector('[data-role=reported-ad]').textContent==='ad-a'&&/Playback corroborated/.test(v.querySelector('[data-role=verify-status]').textContent)&&/match the expected advertisement/.test(v.querySelector('[data-role=visual-verdict]').textContent)})()"))
        await shot("5_time_search_nearest")
        check("UI/G: clicking the stored frame opens the large viewer", (await ev("(document.querySelector('[data-role=nearest-frame] img[data-zoom]').click(),1)")) and await wait_for("!!document.querySelector('.lb-bg .lb-stage img')", 8))
        await ev("(document.dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true})),1)"); await ev("(document.querySelector('.lb-bg')&&document.querySelector('.lb-bg').remove(),1)")
        check("UI/G: the session window of the result lists the stored frames of that session in time order", await ev("(()=>{const f=[...document.querySelectorAll('[data-session=\"%s\"] [data-role=session-frames] figure')];return f.length>=9&&f.some(x=>x.classList.contains('ts-near'))})()" % sid_a))
        # (2) open the Proof-of-Play session from the result
        check("UI/G: 'Open Proof-of-Play session' links to the session", await ev("document.querySelector('[data-role=nearest-frame] a[data-session-link]').getAttribute('href')") == "#pop?session=" + sid_a)
        await ev("(document.querySelector('[data-role=nearest-frame] a[data-session-link]').click(),1)")
        check("UI/G: the link opens that Proof-of-Play session with its checkpoints and history", await wait_for("/Proof of Play . %s/i.test((document.body||{}).innerText||'') && /Verification checkpoints/i.test((document.body||{}).innerText||'')" % sid_a, 20), await ev("((document.body||{}).innerText||'').slice(0,200)"))
        await ev("document.querySelectorAll('.vm-modal-head button').forEach(b=>b.click()), 1")
        # (3) wrong advertisement: reported ad-b, contradicted by the frames
        await cmd("Page.navigate", url=BASE + "?t=%d#search?source=display" % int(time.time() * 1000))
        await wait_for("!!document.querySelector('[data-role=ts-run]')", 15)
        await set_time("utc", (fb_s[8]["utc_ms"] // 1000) * 1000, win=10, tol=2000)
        await run_ts()
        check("UI/G: a wrong-advertisement session shows expected ad-a, reported ad-b, a Mismatch verdict and that the frames contradict the schedule", await wait_for("(()=>{const n=document.querySelector('[data-role=nearest-frame]');if(!n)return false;return n.querySelector('[data-role=expected-ad]').textContent==='ad-a'&&n.querySelector('[data-role=reported-ad]').textContent==='ad-b'&&/Mismatch/.test(n.querySelector('[data-role=verify-status]').textContent)&&/different content than expected/.test(n.querySelector('[data-role=visual-verdict]').textContent)&&/different advertisement than scheduled/.test(n.innerText)})()", 15), await ev("(document.querySelector('[data-role=time-search]').innerText||'').slice(0,500)"))
        await shot("6_time_search_mismatch")
        # (4) no retained evidence
        await set_time("utc", first - 7 * 86400000, win=30, tol=2000)
        await run_ts()
        check("UI/G: with no retained evidence the page says so and never claims nothing was displayed", await wait_for("(()=>{const n=document.querySelector('[data-role=no-evidence]');return !!n&&/No retained display evidence/i.test(n.innerText)&&/does not establish that nothing was displayed/i.test(n.innerText)&&/earlier than the oldest retained frame/i.test(n.innerText)&&!document.querySelector('[data-role=nearest-frame]')})()", 15), await ev("(document.querySelector('[data-role=time-search]').innerText||'').slice(0,600)"))
        await shot("7_time_search_none")
        # (5) a gap between frames, with the session the player reported
        await set_time("utc", ((fk["utc_ms"] // 1000) * 1000) + 500, win=10, tol=100)
        await run_ts()
        # (6) expired JPEG
        # the time input has whole-second precision: use the second (floor or ceil) whose nearest stored frame is the expired one
        t_exp = next((t for t in ((fx["utc_ms"] // 1000) * 1000, (fx["utc_ms"] // 1000 + 1) * 1000) if near(iso(t), 2000)[1].get("frame", {}).get("frame_id") == fx["frame_id"]), None)
        check("UI/G: a whole-second time exists whose nearest frame is the expired one (test precondition)", t_exp is not None)
        await set_time("device", t_exp or (fx["utc_ms"] // 1000) * 1000, win=10, tol=2000)
        await run_ts()
        check("UI/G: an expired JPEG shows the metadata and says the image expired", await wait_for("(()=>{const n=document.querySelector('[data-role=nearest-frame]');return !!n&&!!n.querySelector('[data-role=image-expired]')&&/image expired/.test(n.innerText)&&/Recorded \\(UTC\\)/.test(n.innerText)&&/Playback corroborated/.test(n.innerText)})()", 15), await ev("(document.querySelector('[data-role=time-search]').innerText||'').slice(0,500)"))
        await shot("8_time_search_expired")
        # (7) timezone arithmetic: DST in another zone, ambiguous and non-existent local times
        z = lambda a: ev("(()=>{const r=VMUI.timeSearch.wallToUtc(%s);return JSON.stringify({u:new Date(r.utc).toISOString(),o:r.other?new Date(r.other).toISOString():null,a:!!r.ambiguous,g:!!r.gap})})()" % a)
        r1 = json.loads(await z("2026,1,15,12,0,0,'America/New_York'")); r2 = json.loads(await z("2026,7,15,12,0,0,'America/New_York'"))
        r3 = json.loads(await z("2026,10,25,1,30,0,'Europe/London'")); r4 = json.loads(await z("2026,3,29,1,30,0,'Europe/London'")); r5 = json.loads(await z("2026,11,1,1,30,0,'America/New_York'"))
        r6 = json.loads(await z("2026,6,1,12,0,0,'Europe/London'")); r7 = json.loads(await z("2026,12,1,12,0,0,'Europe/London'")); r8 = json.loads(await z("2026,6,1,12,0,0,'UTC'"))
        check("UI/G: New York winter (EST) and summer (EDT) times convert to UTC with the right offset", r1["u"] == "2026-01-15T17:00:00.000Z" and r2["u"] == "2026-07-15T16:00:00.000Z", (r1, r2))
        check("UI/G: London summer (BST) and winter (GMT) times convert to UTC with the right offset", r6["u"] == "2026-06-01T11:00:00.000Z" and r7["u"] == "2026-12-01T12:00:00.000Z" and r8["u"] == "2026-06-01T12:00:00.000Z", (r6, r7, r8))
        check("UI/G: a local time that happens twice (clocks go back) is flagged ambiguous with both instants", r3["a"] and {r3["u"], r3["o"]} == {"2026-10-25T00:30:00.000Z", "2026-10-25T01:30:00.000Z"} and r5["a"] and {r5["u"], r5["o"]} == {"2026-11-01T05:30:00.000Z", "2026-11-01T06:30:00.000Z"}, (r3, r5))
        check("UI/G: a local time that does not exist (clocks go forward) is flagged", r4["g"] and not r4["a"], r4)
        await ev("(%s)(%s)" % (JS_SET, json.dumps({"tz": "other", "other": "Europe/London", "date": "2026-10-25", "time": "01:30:00", "win": "10", "tol": "2000"})))
        check("UI/G: the form shows the ambiguity banner and offers the first or second occurrence", await wait_for("!!document.querySelector('[data-role=tz-ambiguous]')&&!!document.querySelector('[data-role=ts-pick]')&&!document.querySelector('[data-role=ts-pick]').closest('.vm-row').hidden", 8))
        await ev("(%s)(%s)" % (JS_SET, json.dumps({"tz": "other", "other": "Mars/Olympus", "date": "2026-10-25", "time": "01:30:00"})))
        check("UI/G: an unknown timezone name is rejected with a message and the search is disabled", await wait_for("(()=>{const p=document.querySelector('[data-role=time-search]');return /not a timezone name/.test(p.innerText)&&p.querySelector('[data-role=ts-run]').disabled})()", 8))
    check("UI: no console errors or exceptions", not [l for l in logs if "favicon" not in l], logs)

    # ===== Test F: storage pressure ===================================================================================================================
    stop(api); stop(dash); time.sleep(1.5)
    root2, proot2 = os.path.join(tmp, "store2"), os.path.join(tmp, "popstore2"); os.makedirs(root2)
    cfg2 = os.path.join(tmp, "cfg2.json")
    json.dump(config(root2, proot2, {"enabled": True, "phase_ms": 0, "min_free_bytes": 10 ** 15}, "b2.sock"), open(cfg2, "w"))
    api2 = start([API_BIN, "--config", cfg2, "--port", str(API_PORT), "--provider", "mock:64", "--status-file", os.path.join(tmp, "st2.json")], "api2.log"); time.sleep(2.0)
    http("POST", "/api/pop/media", {"media_id": "ad-c", "display_name": "Short spot", "uri": "ad-c.mp4", "duration_s": 30, "slot_s": SLOT_C, "width": 1920, "height": 1080, "sha256": "0" * 64})
    d = os.path.join(proot2, "pop", "refs_incoming", "ad-c.display_letterbox"); os.makedirs(d, exist_ok=True)
    for i in range(0, 14): display_picture("C", i * 2).save(os.path.join(d, f"{i * 2000}.jpg"), quality=90)
    http("POST", "/api/pop/media/ad-c/references/build?profile=display_letterbox")
    p0, t0m = proc_snapshot(), time.time()
    sf, bad = await asyncio.to_thread(run_session, "ad-c", None, None, (2, 7, 12))
    REPORT["F_host_vm_api_server_checkpoints_only_14s_session"] = delta(p0, proc_snapshot(), time.time() - t0m)
    hs = http("GET", "/api/pop/history/status")[1]
    check("F: with free space below the limit no history is stored (recording pauses and says so)", sf is not None and hs["store"]["rows"] == 0 and hs["counters"]["paused_low_space"] > 0 and "free space" in hs["last_error"], (hs["counters"], hs["last_error"]))
    check("F: the three verification checkpoints are unaffected by the storage pressure", sf is not None and sf["verification"]["frames_ok"] == 3 and len(sf["evidence"]) == 3, sf and sf.get("verification"))
    check("F: no history JPEG was written", not os.path.exists(os.path.join(proot2, "pop", "history")) or not any(f.endswith(".jpg") for _, _, fs in os.walk(os.path.join(proot2, "pop", "history")) for f in fs))
    stop(api2)
    player.stop_flag = True
    json.dump(REPORT, open(f"{OUT}/report.json", "w"), indent=1)
    print(json.dumps(REPORT, indent=1)[:2500])
    print(f"RESULT {sum(results)}/{len(results)} passed")


try:
    asyncio.run(main())
except Exception:
    traceback.print_exc(); results.append(False); print("FAIL exception in test")
finally:
    for p in procs: stop(p)
    print("logs in", tmp)
    sys.exit(0 if results and all(results) else 1)
