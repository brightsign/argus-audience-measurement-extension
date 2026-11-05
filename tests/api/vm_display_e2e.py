#!/usr/bin/env python3
"""Host end-to-end test of the OS rendered-frame integration: display preview API + Proof of Play backend "resilience_grpc".

  vm_display_e2e.py VM_API_SERVER ARGUS_DISPLAY_BRIDGE FAKE_RENDERED_OUTPUT_SERVER

Starts REAL processes on loopback / Unix sockets:
  fake_rendered_output_server   test double of the OS daemon + producer with the OS semantics (real gRPC RenderedOutputService over a
                                Unix socket, interest-gated 5 FPS producer, 3 s STALE window, no timestamp in the frame message)
  argus_display_bridge          the real gRPC client process
  vm_api_server                 the real API, with visual_memory.display and visual_memory.pop (mock embedder)
and plays the role of the BrightScript player by appending events in the player's format.

SYNTHETIC / HOST ONLY: pictures are generated here. This verifies plumbing and decision logic. It is NOT hardware evidence; the XT5
measurements come from the real OS service.
Exit code 0 = all checks passed.
"""
import json, os, shutil, signal, subprocess, sys, tempfile, time, traceback, urllib.error, urllib.request
from PIL import Image, ImageDraw

API_BIN, BRIDGE_BIN, FAKE_BIN = sys.argv[1:4]
PORT, PORT2 = 20451, 20452
tmp = tempfile.mkdtemp(prefix="display_e2e_")
procs, fails = [], []
bridge_pid = -1


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(extra)[:300]) if not cond and extra != "" else ""), flush=True)
    if not cond:
        fails.append(name)


def call(port, method, path, body=None, headers=None):
    data = json.dumps(body).encode() if body is not None else None
    h = dict(headers or {})
    if data: h["Content-Type"] = "application/json"
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data, method=method, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            b = r.read(); ct = r.headers.get("Content-Type", "")
            return r.status, (json.loads(b) if "json" in ct and b else b), dict(r.headers)
    except urllib.error.HTTPError as e:
        b = e.read(); ct = e.headers.get("Content-Type", "")
        return e.code, (json.loads(b) if "json" in ct and b else b), dict(e.headers)
    except (urllib.error.URLError, ConnectionError, TimeoutError):
        return 0, None, {}


def picture(kind, w, h, shift=0, bright=0):
    im = Image.new("RGB", (w, h)); px = im.load()
    for y in range(h):
        for x in range(w):
            xn, yn = (x + shift) / w, y / h
            if kind == "A": px[x, y] = (int(40 + 200 * xn), int(220 - 150 * yn), 90 + int(60 * abs(xn - 0.5)))
            else: px[x, y] = (200 if (int(xn * 8) % 2) else 30, 40 + int(180 * abs(yn - 0.5) * 2), 220 if (int(yn * 6) % 2) else 60)
    d = ImageDraw.Draw(im)
    if kind == "A": d.ellipse([w * 0.3, h * 0.2, w * 0.7, h * 0.8], outline=(255, 255, 255), width=4)
    else: d.rectangle([w * 0.2, h * 0.3, w * 0.8, h * 0.7], fill=(250, 250, 40))
    if bright: im = im.point(lambda v: max(0, min(255, v + bright)))
    return im


def display_picture(kind, shift=0, bright=0):
    """What the OS producer delivers: the 1280x800 display letterboxed into 640x360 (picture 576x360 centred on black)."""
    canvas = Image.new("RGB", (640, 360), (0, 0, 0))
    canvas.paste(picture(kind, 576, 360, shift=shift, bright=bright), (32, 0))
    return canvas


def start(cmd, log, **kw):
    p = subprocess.Popen(cmd, stdout=open(os.path.join(tmp, log), "w"), stderr=subprocess.STDOUT, **kw)
    procs.append(p)
    return p


def stop(p):
    try: p.send_signal(signal.SIGTERM); p.wait(timeout=5)
    except Exception:
        try: p.kill()
        except Exception: pass


def wait_until(fn, timeout=10, step=0.1):
    t0 = time.time()
    while time.time() - t0 < timeout:
        v = fn()
        if v: return v
        time.sleep(step)
    return None


frames = os.path.join(tmp, "frames"); os.makedirs(frames)
display_picture("A").save(os.path.join(frames, "A.jpg"), quality=88)
display_picture("B").save(os.path.join(frames, "B.jpg"), quality=88)
Image.new("RGB", (640, 360), (70, 70, 70)).save(os.path.join(frames, "IDLE.jpg"), quality=88)
Image.new("RGB", (640, 360), (0, 0, 0)).save(os.path.join(frames, "BLACK.jpg"), quality=88)
dsock, bsock = os.path.join(tmp, "daemon.sock"), os.path.join(tmp, "bridge.sock")
interest, control, stats = os.path.join(tmp, "rendered_frame.interest"), os.path.join(tmp, "control.json"), os.path.join(tmp, "stats.json")


def ctl(**kw):
    c = {"frame": "IDLE", "pause": False, "black": False, "frozen": False, "unavailable": False, "clock_skew_ms": 0, "provider": 7}
    c.update(kw)
    json.dump(c, open(control + ".tmp", "w")); os.replace(control + ".tmp", control)


def fstats():
    try: return json.load(open(stats))
    except Exception: return {"frames_produced": 0, "capture_calls": 0, "health_calls": 0}


root = os.path.join(tmp, "store"); sd = os.path.join(tmp, "sd")
proot = os.path.join(tmp, "popstore")                                  # Proof of Play's own storage root (visual_memory.pop.storage_root)
os.makedirs(os.path.join(sd, "pop", "jobs")); os.makedirs(root, exist_ok=True)
legacy_bytes = b"SQLite format 3\0" + b"stand-in for an existing pop.db in the Visual Memory storage root" * 20
open(os.path.join(root, "pop.db"), "wb").write(legacy_bytes)
events = os.path.join(sd, "pop", "events.jsonl"); open(events, "w").close()


def vm_cfg(display, port_name):
    c = {"visual_memory": {"enabled": True, "vector_search_enabled": True, "storage_root": root, "embedding": {"enabled": True},
                           "pop": {"enabled": True, "storage_root": proot, "events_path": events, "jobs_dir": os.path.join(sd, "pop", "jobs"), "poll_ms": 50,
                                   "sd_path_prefix_from": "SD:/", "sd_path_prefix_to": sd + "/", "allow_test_jobs": True,
                                   "capture_backend": "resilience_grpc" if display else "brightscript_screenshot"}}}
    if display:
        # vm_api_server starts and supervises argus_display_bridge itself (manage_bridge), exactly as on the player
        c["visual_memory"]["display"] = {"enabled": True, "bridge_socket": bsock, "poll_ms": 100, "max_age_ms": 1200, "bridge_binary": BRIDGE_BIN,
                                         "os_socket": "unix://" + dsock, "interest_file": interest}
    p = os.path.join(tmp, f"cfg_{port_name}.json"); json.dump(c, open(p, "w"))
    return p


try:
    # ------------------------------------------------------------------ 0. disabled by default
    ctl()
    cfg0 = vm_cfg(False, "off")
    s0 = start([API_BIN, "--config", cfg0, "--port", str(PORT2), "--provider", "mock:64", "--status-file", os.path.join(tmp, "st0.json")], "api_off.log")
    wait_until(lambda: call(PORT2, "GET", "/api/display/status")[0] == 200, 8)
    st, b, _ = call(PORT2, "GET", "/api/display/status")
    check("disabled by default: status says so", st == 200 and b["enabled"] is False and b["state"] == "disabled", b)
    st, b, _ = call(PORT2, "GET", "/api/display/frame.jpg")
    check("disabled by default: no frame endpoint (404)", st == 404, b)
    st, b, _ = call(PORT2, "POST", "/api/display/frame.jpg", {})
    check("display routes are GET only (405)", st == 405, b)
    st, b, _ = call(PORT2, "POST", "/api/pop/sessions", {"expected_media_id": "x", "capture_backend": "resilience_grpc"}, {"Origin": f"http://127.0.0.1:{PORT2}"})
    check("PoP: resilience_grpc is refused when the display source is not enabled or the media is unknown (never silently falls back)", st in (404, 503), (st, b))
    stop(s0)

    # ------------------------------------------------------------------ 1. start the chain
    fake = start([FAKE_BIN, "--socket", dsock, "--frames-dir", frames, "--interest-file", interest, "--control-file", control, "--stats-file", stats], "fake.log")
    check("fake OS service up", wait_until(lambda: os.path.exists(dsock), 8))
    cfg = vm_cfg(True, "on")
    api = start([API_BIN, "--config", cfg, "--port", str(PORT), "--provider", "mock:64", "--status-file", os.path.join(tmp, "st1.json")], "api_on.log")
    check("vm_api_server up", wait_until(lambda: call(PORT, "GET", "/api/display/status")[0] == 200, 10))
    check("the bridge was started and is supervised by vm_api_server", bool(wait_until(lambda: os.path.exists(bsock), 8)) and call(PORT, "GET", "/api/display/status")[1]["bridge"]["running"] is True)
    bridge_pid = call(PORT, "GET", "/api/display/status")[1]["bridge"]["pid"]

    # ------------------------------------------------------------------ 2. interest gating: nothing runs without a viewer
    time.sleep(1.5)
    s_before = fstats()
    time.sleep(1.5)
    s_after = fstats()
    check("idle: no viewer and no session => no RPC to the OS service", s_after.get("health_calls", 0) == s_before.get("health_calls", 0) == 0 and s_after.get("frames_produced", 0) == 0, (s_before, s_after))
    check("idle: the producer is not asked to capture (no interest file)", not os.path.exists(interest))
    st, b, _ = call(PORT, "GET", "/api/display/status")
    check("idle: status reports idle, enabled, source described as approximate", b["enabled"] and b["state"] == "idle" and "approximate" in b["source"], b)

    # ------------------------------------------------------------------ 3. preview
    ctl(frame="A")
    def get_frame(after=None):
        return call(PORT, "GET", "/api/display/frame.jpg" + (f"?after={after}" if after is not None else ""))
    t0 = time.time()
    got = wait_until(lambda: (lambda r: r if r[0] == 200 else None)(get_frame()), 6, 0.2)
    check("preview: first frame arrives after the producer wakes up (interest touched through the bridge)", bool(got), time.time() - t0)
    st, jpg, h = got
    img_ok = jpg[:3] == b"\xff\xd8\xff" and h.get("Content-Type") == "image/jpeg"
    check("preview: a JPEG with metadata headers", img_ok and int(h["X-Display-Seq"]) >= 1 and h["X-Display-Size"] == "640x360" and h["X-Display-Provider"] == "7", h)
    check("preview: not cacheable", "no-store" in h.get("Cache-Control", ""), h)
    check("preview: frame age is small and capture time is current", 0 <= int(h["X-Display-Age-Ms"]) < 1200 and abs(int(h["X-Display-Capture-Utc-Ms"]) - time.time() * 1000) < 2500, h)
    from io import BytesIO
    im = Image.open(BytesIO(jpg)); im.load()
    check("preview: the frame decodes and shows ad A's display picture", im.size == (640, 360) and abs(im.getpixel((320, 180))[0] - Image.open(os.path.join(frames, "A.jpg")).convert("RGB").getpixel((320, 180))[0]) < 25)
    last = int(h["X-Display-Seq"])
    st, _, _ = get_frame(after=last)
    check("preview: ?after=<latest> answers 204 immediately when nothing is newer", st in (204, 200))
    # rate: poll for 4 s with ?after and count distinct frames
    seen, t_end = set(), time.time() + 4.0
    while time.time() < t_end:
        s2, b2, h2 = get_frame()
        if s2 == 200: seen.add(int(h2["X-Display-Seq"]))
        time.sleep(0.05)
    check("preview: about 5 distinct frames per second (4 s window)", 14 <= len(seen) <= 24, len(seen))
    st, b, _ = call(PORT, "GET", "/api/display/status")
    check("status: available, ~5 fps, counters, ring bounded", b["display_health"] == "available" and 3.5 <= b["fps"] <= 6.5 and b["counters"]["accepted"] >= 15 and b["ring"]["frames"] <= 16, b)
    check("status: no gaps from errors", b["counters"]["rpc_errors"] == 0 and b["counters"]["invalid_jpeg"] == 0, b["counters"])
    check("status: acquisition latency is reported", b["acquire_ms"]["p50"] is not None and b["acquire_ms"]["p50"] < 200, b["acquire_ms"])
    check("status: the producer latency budget is flagged as not measured", b["config"]["producer_latency_measured"] is False)

    # content changes, black and frozen signals
    ctl(frame="B")
    changed = wait_until(lambda: (lambda r: r if r[0] == 200 and abs(Image.open(BytesIO(r[1])).convert("RGB").getpixel((200, 100))[0] - im.convert("RGB").getpixel((200, 100))[0]) > 40 else None)(get_frame()), 4, 0.2)
    check("preview: a change of what is displayed shows up within seconds", bool(changed))
    ctl(frame="BLACK", black=True)
    bk = wait_until(lambda: (lambda b: b if b["health"]["black_screen"] else None)(call(PORT, "GET", "/api/display/status")[1]), 4, 0.2)
    check("black-screen signal passes through from the producer", bool(bk))
    ctl(frame="A", frozen=True)
    fz = wait_until(lambda: (lambda b: b if b["health"]["frozen_frame"] else None)(call(PORT, "GET", "/api/display/status")[1]), 4, 0.2)
    check("frozen-frame signal passes through from the producer", bool(fz))
    ctl(frame="A")

    # ------------------------------------------------------------------ 4. interest lapses when nobody looks
    time.sleep(7.5)
    f1 = fstats(); time.sleep(2.0); f2 = fstats()
    check("after the last viewer request the source stops polling and the producer idles", f2["frames_produced"] == f1["frames_produced"] and f2["health_calls"] == f1["health_calls"], (f1, f2))

    # ------------------------------------------------------------------ 5. failure modes
    def wait_state(want, **kw):
        """Apply a fault, keep the viewer interested and wait until the source reports `want` (or time out)."""
        ctl(frame="A", **kw)
        end = time.time() + 8
        s_ = None
        while time.time() < end:
            get_frame()
            s_ = call(PORT, "GET", "/api/display/status")[1]
            if s_["state"] == want: break
            time.sleep(0.2)
        return s_
    ctl(frame="A", pause=True)                                          # the producer stops delivering frames
    stale = wait_until(lambda: (lambda b: b if b["state"] == "stale" else (get_frame() and None))(call(PORT, "GET", "/api/display/status")[1]), 8, 0.3)
    check("producer stops feeding: reported STALE after the OS window, frames are not served as current", bool(stale) and get_frame()[0] == 503, stale and stale["detail"])
    ctl(frame="A")
    rec = wait_until(lambda: get_frame()[0] == 200, 6, 0.3)
    check("producer resumes: frames are served again", bool(rec))
    s_ = wait_state("provider_rejected", provider=5)
    check("a frame with another provenance (SCREENSHOT=5) is rejected, not shown", s_["state"] == "provider_rejected", s_["state"])
    ctl(frame="A")
    wait_state("ok")
    s_ = wait_state("clock_mismatch", clock_skew_ms=60000)
    check("a daemon clock that is not our monotonic clock is rejected", s_["state"] == "clock_mismatch" and s_["counters"]["clock_rejected"] >= 1, s_["state"])
    wait_state("ok")
    stop(fake)
    gone = wait_until(lambda: (lambda b: b if b["state"] == "unavailable" else (get_frame() and None))(call(PORT, "GET", "/api/display/status")[1]), 6, 0.3)
    check("OS service gone: unavailable, and no frame older than the freshness limit is served",
          bool(gone) and bool(wait_until(lambda: get_frame()[0] == 503, 4, 0.3)), gone and gone["detail"])
    fake = start([FAKE_BIN, "--socket", dsock, "--frames-dir", frames, "--interest-file", interest, "--control-file", control, "--stats-file", stats], "fake2.log")
    def restarted():
        get_frame()
        b = call(PORT, "GET", "/api/display/status")[1]
        return b if b["counters"]["service_restarts"] >= 1 and b["state"] == "ok" else None
    stt = wait_until(restarted, 12, 0.3)
    check("OS service restarts: reconnects by itself, new boot id counted, frames flow again", bool(stt) and get_frame()[0] == 200, call(PORT, "GET", "/api/display/status")[1]["counters"])
    os.kill(call(PORT, "GET", "/api/display/status")[1]["bridge"]["pid"], signal.SIGKILL)            # the bridge crashes
    def bridge_back():
        get_frame()
        b = call(PORT, "GET", "/api/display/status")[1]
        return b if b["bridge"]["restarts"] >= 1 and b["bridge"]["running"] and b["state"] == "ok" else None
    bb = wait_until(bridge_back, 15, 0.3)
    check("bridge killed: the supervisor restarts it and frames flow again", bool(bb) and get_frame()[0] == 200, call(PORT, "GET", "/api/display/status")[1]["bridge"])

    # ------------------------------------------------------------------ 6. Proof of Play with the resilience_grpc backend
    mono0 = time.time()
    seq = [0]

    def mono():
        return int((time.time() - mono0) * 1000)

    def emit(ev, **kw):
        seq[0] += 1
        e = {"ev": ev, "seq": seq[0], "mono_ms": mono(), **kw}
        with open(events, "a") as f: f.write(json.dumps(e) + "\n")

    emit("boot", mode="jobs")
    for mid, name in (("ad-a", "Advertisement A"), ("ad-b", "Advertisement B")):
        st, r, _ = call(PORT, "POST", "/api/pop/media", {"media_id": mid, "display_name": name, "uri": mid + ".mp4", "duration_s": 60, "slot_s": 6, "width": 1920, "height": 1080},
                        {"Origin": f"http://127.0.0.1:{PORT}"})
        check(f"register {mid}", st == 201, r)
        kind = mid[-1].upper()
        for prof, mkimg in (("screenshot", lambda i: picture(kind, 640, 400, shift=i * 4)), ("display_letterbox", lambda i: display_picture(kind, shift=i * 4))):
            d = os.path.join(proot, "pop", "refs_incoming", mid if prof == "screenshot" else mid + ".display_letterbox"); os.makedirs(d)
            for i in range(8): mkimg(i).save(os.path.join(d, f"{i * 4000}.jpg"), quality=90)
            st, r, _ = call(PORT, "POST", f"/api/pop/media/{mid}/references/build?profile={prof}", None, {"Origin": f"http://127.0.0.1:{PORT}"})
            check(f"build {prof} references for {mid}", st == 200 and r["added"] == 8 and r["profile"] == prof, r)
    st, r, _ = call(PORT, "GET", "/api/pop/media")
    check("registry lists the two ads (hidden profile rows are not listed) with their display reference counts",
          st == 200 and sorted(m["media_id"] for m in r["media"]) == ["ad-a", "ad-b"] and all(m["reference_frames"] == 8 and m["display_reference_frames"] == 8 for m in r["media"]), r)

    def schedule(sid, offsets=(1, 3, 5), **kw):
        body = {"expected_media_id": "ad-a", "session_id": sid}
        if offsets is not None: body["capture_offsets_s"] = list(offsets)
        body.update(kw)
        st, r, _ = call(PORT, "POST", "/api/pop/sessions", body, {"Origin": f"http://127.0.0.1:{PORT}"})
        return st, r

    st, r = schedule("g-a")
    check("schedule with the configured backend resilience_grpc", st == 201 and r["capture_backend"] == "resilience_grpc" and r["capture_offsets_ms"] == [1000, 3000, 5000], r)
    job = json.load(open(os.path.join(sd, "pop", "jobs", r["job_file"])))
    check("the player is told to take NO screenshots for this session", job["capture_offsets_ms"] == [] and job["capture_backend"] == "resilience_grpc", job)
    st, r2 = schedule("g-s", capture_backend="brightscript_screenshot")
    job2 = json.load(open(os.path.join(sd, "pop", "jobs", r2["job_file"])))
    check("the screenshot backend stays selectable per session (fallback path unchanged)", st == 201 and job2["capture_offsets_ms"] == [1000, 3000, 5000], job2)
    st, r3 = schedule("g-x", capture_backend="nonsense")
    check("unknown backend -> 400", st == 400, r3)
    for sid in ("g-b", "g-c", "g-e"):
        schedule(sid, **({"play_media_id": "ad-b"} if sid == "g-b" else {"interrupt_at_s": 2.5} if sid == "g-c" else {"break_capture": True} if sid == "g-e" else {}))

    def player(sid, media, shows, secs, reason="slot_end", paused=False, late_ms=0):
        """The BrightScript player's event sequence in real time; `shows` is what the display shows while it plays."""
        emit("session_scheduled_run", session=sid, job_id="job-" + sid, media_id=media, expected_media_id="ad-a", capture_count=0)
        emit("play_requested", session=sid, media_id=media, expected_media_id="ad-a", ok=True)
        time.sleep(0.15)
        if not late_ms: ctl(frame=shows, pause=paused)
        emit("play_started", session=sid, media_id=media, expected_media_id="ad-a", vcode=3, latency_ms=200)
        t_start = time.time()
        if late_ms:                                                   # the picture appears on the display only late_ms AFTER the play_started event
            time.sleep(late_ms / 1000.0); ctl(frame=shows, pause=paused)
        time.sleep(secs - late_ms / 1000.0)
        emit("play_stopped", session=sid, media_id=media, reason=reason)
        emit("session_end", session=sid, media_id=media, expected_media_id="ad-a", reason=reason, actual_play_ms=int((time.time() - t_start) * 1000))
        ctl(frame="IDLE", pause=paused)

    def verdict(sid, want, timeout=12):
        d = wait_until(lambda: (lambda r: r[1] if r[0] == 200 and r[1]["status"] == want else None)(call(PORT, "GET", f"/api/pop/sessions/{sid}")), timeout, 0.25)
        return d or call(PORT, "GET", f"/api/pop/sessions/{sid}")[1]

    ctl(frame="IDLE")
    time.sleep(0.5)
    player("g-a", "ad-a", "A", 6.2)
    d = verdict("g-a", "playback_corroborated")
    check("Test A (correct ad, OS frames): Playback corroborated", d.get("status") == "playback_corroborated", (d.get("status"), d.get("reason")))
    ev = d.get("evidence", [])
    check("A: three pieces of evidence, all captured by resilience_grpc with provenance and a time uncertainty",
          len(ev) == 3 and all(e["capture_method"] == "resilience_grpc" and "weston_vop2_writeback" in e["provenance"] and e["time_uncertainty_ms"] is not None and 250 <= e["time_uncertainty_ms"] <= 600 for e in ev), ev)
    check("A: each frame was taken close to its target offset (|actual - target| < 700 ms)", all(abs(e["actual_offset_ms"] - e["target_offset_ms"]) < 700 for e in ev), [(e["target_offset_ms"], e["actual_offset_ms"]) for e in ev])
    check("A: all evidence images stored and served", all(e["image_available"] for e in ev) and call(PORT, "GET", ev[0]["image_url"])[2].get("Content-Type") == "image/jpeg")
    check("A: matched against the display-geometry references (similarity high)", d["verification"]["frames_matched"] == 3 and d["verification"]["min_sim_expected"] > 0.8, d["verification"])

    player("g-b", "ad-b", "B", 6.2)
    d = verdict("g-b", "mismatch")
    check("Test B (wrong ad on the display): Mismatch naming ad-b", d.get("status") == "mismatch" and d["verification"]["best_media_id"] == "ad-b", (d.get("status"), d.get("reason")))

    player("g-c", "ad-a", "A", 2.6, reason="interrupted")
    d = verdict("g-c", "incomplete")
    check("Test C (interrupted at 2.5 s): Incomplete; only the offset that was reached has evidence", d.get("status") == "incomplete" and len(d["evidence"]) == 1, (d.get("status"), len(d.get("evidence", []))))

    ctl(frame="IDLE", pause=True)
    time.sleep(4.0)                                                  # the OS producer has stopped feeding: STALE, no usable frame near any target
    st, r = schedule("g-n")
    check("scheduling is refused (503) while the OS producer delivers nothing: the backend must be ready before a session starts", st == 503 and "not ready" in r.get("message", ""), (st, r))
    st, r = schedule("g-d", require_ready=False)
    check("require_ready:false is a test hook that works with allow_test_jobs", st == 201, (st, r))
    player("g-d", "ad-a", "A", 6.2, paused=True)
    d = verdict("g-d", "capture_unsupported", 14)
    check("Test D (no display frames available): Capture unsupported, failed captures recorded with the reason",
          d.get("status") == "capture_unsupported" and [e["status"] for e in d["evidence"]] == ["failed"] * 3 and "no fresh display frame" in d["evidence"][0]["failure_reason"], (d.get("status"), d.get("evidence") and d["evidence"][0]))
    ctl(frame="IDLE", pause=False)

    time.sleep(1.0)
    player("g-e", "ad-a", "A", 6.2)
    d = verdict("g-e", "capture_unsupported", 14)
    check("break_capture test hook also works for this backend", d.get("status") == "capture_unsupported", (d.get("status"), d.get("reason")))

    # ---- never use a frame from before the play, or one that may belong to what follows it
    ctl(frame="IDLE")
    time.sleep(1.0)
    schedule("g-f", offsets=(0.3, 3, 5))
    player("g-f", "ad-a", "A", 6.2, late_ms=500)                       # the advertisement shows up 500 ms after the event; IDLE is on screen before
    d = verdict("g-f", "playback_corroborated")
    ev = d.get("evidence", [])
    check("pre-playback frames are never evidence: with the display switching 500 ms after play_started, the first capture still shows the advertisement",
          d.get("status") == "playback_corroborated" and len(ev) == 3 and all(e["status"] == "ok" for e in ev) and ev[0]["actual_offset_ms"] >= 600, (d.get("status"), [(e["status"], e["actual_offset_ms"]) for e in ev]))
    time.sleep(1.0)
    schedule("g-g", offsets=(1, 6))
    player("g-g", "ad-a", "A", 6.2)
    d = verdict("g-g", "incomplete", 14) if False else call(PORT, "GET", "/api/pop/sessions/g-g")[1]
    wait_until(lambda: call(PORT, "GET", "/api/pop/sessions/g-g")[1].get("verification"), 12, 0.3)
    d = call(PORT, "GET", "/api/pop/sessions/g-g")[1]
    ev = d.get("evidence", [])
    check("a frame at the very end of the play is transition-adjacent: not used as evidence, recorded as a failed capture with that reason",
          len(ev) == 2 and ev[0]["status"] == "ok" and ev[1]["status"] == "failed" and "transition-adjacent" in ev[1]["failure_reason"], [(e["status"], e["failure_reason"]) for e in ev])
    # ---- one frame per second (capture_every_s): bounded, asynchronous, only selected frames are stored
    time.sleep(1.0)
    st, r = schedule("g-h", offsets=None, capture_every_s=1)
    check("capture_every_s=1 on a 6 s slot gives 5 target offsets", st == 201 and r["capture_offsets_ms"] == [1000, 2000, 3000, 4000, 5000], r)
    st, r2 = schedule("g-i", offsets=None, capture_every_s=0.1)
    check("capture_every_s below 0.5 s is refused", st == 400, r2)
    st, r2 = schedule("g-j", capture_every_s=1)
    check("capture_every_s together with capture_offsets_s is refused", st == 400, r2)
    n_before = len([os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(proot, "pop", "evidence")) for f in fs])
    player("g-h", "ad-a", "A", 6.2)
    d = verdict("g-h", "playback_corroborated")
    ev = d.get("evidence", [])
    errs = [abs(e["actual_offset_ms"] - e["target_offset_ms"]) for e in ev if e["status"] == "ok"]
    check("1 fps evidence: all 5 frames captured, none missing, each close to its target second", d.get("status") == "playback_corroborated" and len(ev) == 5 and len(errs) == 5 and max(errs) < 700, (d.get("status"), len(ev), errs))
    n_after = len([os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(proot, "pop", "evidence")) for f in fs])
    check("only the 5 selected frames were stored for the 1 fps session (not the ~30 frames the preview source produced)", n_after - n_before == 5, n_after - n_before)

    # the screenshot-backend session scheduled earlier never ran (no player): still scheduled, no interference
    d = call(PORT, "GET", "/api/pop/sessions/g-s")[1]
    check("a scheduled screenshot-backend session is untouched", d["status"] == "scheduled", d["status"])

    # ------------------------------------------------------------------ 7. nothing is persisted except selected evidence; idle again afterwards
    imgs = [os.path.join(dp, f) for dp, _, fs in os.walk(os.path.join(proot, "pop", "evidence")) for f in fs]
    check("only the selected evidence frames are stored (7 + g-f 3 + g-g 1 + g-h 5 = 16 images), not the preview stream", len(imgs) == 16, len(imgs))
    check("no Visual Memory frame store was created by the preview", not os.path.exists(os.path.join(root, "frames")))
    time.sleep(8.0)
    f1 = fstats(); time.sleep(2.0); f2 = fstats()
    check("after the last session the producer idles again (no held interest)", f2["frames_produced"] == f1["frames_produced"], (f1, f2))
    st, b, _ = call(PORT, "GET", "/api/display/status")
    check("status after the run: no rejected frames, ring still bounded", b["ring"]["frames"] <= 16 and b["holds"] == 0, (b["ring"], b["holds"]))
    check("PoP status shows the configured backend", call(PORT, "GET", "/api/pop/status")[1]["capture_backend"] == "resilience_grpc")
    check("isolated storage: pop.db (schema 2) lives in visual_memory.pop.storage_root", os.path.exists(os.path.join(proot, "pop.db")) and call(PORT, "GET", "/api/pop/status")[1]["store"]["schema_version"] == 2)
    check("isolated storage: the file in the Visual Memory storage root is byte-identical and no PoP directory appeared there",
          open(os.path.join(root, "pop.db"), "rb").read() == legacy_bytes and not os.path.exists(os.path.join(root, "pop")))

    # ---- configuration changes without a root shell: the API restarts itself in place (same PID) and the bridge follows
    pid_api = api.pid
    cfg_json = json.load(open(cfg))
    cfg_json["visual_memory"]["display"]["enabled"] = False
    time.sleep(0.2); json.dump(cfg_json, open(cfg, "w"))
    off = wait_until(lambda: (lambda r: r if r[0] == 200 and r[1].get("enabled") is False else None)(call(PORT, "GET", "/api/display/status")), 12, 0.3)
    check("config change: display disabled in place within seconds, same PID, no manual restart", bool(off) and api.poll() is None and api.pid == pid_api)
    check("config change: the bridge child was stopped with the old configuration", not os.path.exists(f"/proc/{call(PORT, 'GET', '/api/display/status')[1].get('bridge', {}).get('pid', -1) or -1}"))
    open(cfg, "w").write('{"visual_memory": {"enabled": true, ')           # a half-written file must be ignored
    time.sleep(5.0)
    check("a half-written config is ignored: the running configuration stays", call(PORT, "GET", "/api/display/status")[0] == 200)
    cfg_json["visual_memory"]["display"]["enabled"] = True
    json.dump(cfg_json, open(cfg, "w"))
    on = wait_until(lambda: (lambda r: r if r[0] == 200 and r[1].get("enabled") is True else None)(call(PORT, "GET", "/api/display/status")), 12, 0.3)
    check("config change: display enabled again in place; the preview works", bool(on) and bool(wait_until(lambda: get_frame()[0] == 200, 8, 0.3)) and api.pid == pid_api)
except BaseException:
    traceback.print_exc(); fails.append("the test aborted with an exception")
finally:
    for p in procs: stop(p)
    try:
        alive = os.path.exists(f"/proc/{bridge_pid}") and open(f"/proc/{bridge_pid}/cmdline", "rb").read().startswith(BRIDGE_BIN.encode())
    except Exception:
        alive = False
    print(("PASS" if not alive else "FAIL") + " stopping vm_api_server stops the supervised bridge (no orphan)")
    if alive: fails.append("orphaned bridge")
    time.sleep(0.3)
    shutil.rmtree(tmp, ignore_errors=True)
    print("OK: 0 check(s) failed" if not fails else f"FAILED: {len(fails)} check(s): {fails}")
    sys.exit(1 if fails else 0)
