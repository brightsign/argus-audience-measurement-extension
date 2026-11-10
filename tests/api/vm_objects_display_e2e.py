#!/usr/bin/env python3
"""Host end-to-end test: D4 object search/indexing for the BrightSign display (Weston/VOP2 frames of Proof of Play) next to the camera.

  vm_objects_display_e2e.py VM_API_SERVER VM_INGEST_DIR VM_POP_FIXTURE WORKDIR

Real vm_api_server (mock detector + mock embedder, no RKNN), a synthetic camera store and a synthetic Proof-of-Play store written through the real
PopStore API (vm_pop_fixture). Display frames are 640x360 with letterbox bars. Camera ids, Proof-of-Play history ids and checkpoint ids all start at 1
in these stores on purpose: the test proves they are never mixed. Synthetic pictures only.
"""
import io, json, os, shutil, signal, subprocess, sys, time, urllib.error, urllib.request
from PIL import Image, ImageDraw

SERVER, INGEST, FIXTURE, WORK = sys.argv[1:5]
PORT = 20461
BASE = f"http://127.0.0.1:{PORT}"
shutil.rmtree(WORK, ignore_errors=True); os.makedirs(WORK)
CAM, POP = os.path.join(WORK, "cam"), os.path.join(WORK, "pop")
EVENTS, JOBS = os.path.join(WORK, "events.jsonl"), os.path.join(WORK, "jobs")
fails, procs = [], []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(extra)[:400]) if not cond and extra != "" else ""), flush=True)
    if not cond: fails.append(name)


def call(method, path, body=None, raw=None, timeout=30):
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(BASE + path, data=data, method=method, headers={"Content-Type": "application/json", "Origin": BASE} if data else {})
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            b = r.read(); ct = r.headers.get("Content-Type", "")
            return r.status, (json.loads(b) if "json" in ct else b), (time.time() - t0) * 1000
    except urllib.error.HTTPError as e:
        b = e.read(); ct = e.headers.get("Content-Type", "")
        return e.code, (json.loads(b) if "json" in ct else b), (time.time() - t0) * 1000


def write_cfg(path, display=True, extra_auto=None):
    auto = {"enabled": True, "live_scan": False, "backfill_pause_ms": 60, "max_backfill_frames": 60, "min_confidence": 0.30, "display": {"enabled": display, "live_scan": False}}
    auto.update(extra_auto or {})
    json.dump({"visual_memory": {"enabled": True, "vector_search_enabled": True, "storage_root": CAM, "stored_width": 640, "stored_height": 360,
                                 "embedding": {"enabled": True, "backend": "rknn", "model_path": "x"},
                                 "objects": {"enabled": True, "auto": auto},
                                 "pop": {"enabled": True, "storage_root": POP, "events_path": EVENTS, "jobs_dir": JOBS, "poll_ms": 100, "capture_backend": "resilience_grpc",
                                         "history": {"enabled": True}}}}, open(path, "w"))


def start(cfg, name):
    p = subprocess.Popen([SERVER, "--config", cfg, "--port", str(PORT), "--provider", "mock:64", "--detector", "mock", "--no-reexec", "--status-file", os.path.join(WORK, "st.json")],
                         stdout=open(os.path.join(WORK, name + ".log"), "w"), stderr=subprocess.STDOUT)
    procs.append(p)
    for _ in range(80):
        try:
            if call("GET", "/api/vm/status")[0] == 200: return p
        except Exception: pass
        time.sleep(0.2)
    raise SystemExit("server did not start")


def stop(p):
    p.send_signal(signal.SIGTERM)
    try: p.wait(10)
    except Exception: p.kill()


def wait_backfill(src, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        st, s, _ = call("GET", f"/api/vm/objects/auto/status?source={src}")
        if st == 200 and s.get("backfill", {}).get("state") in ("done", "cancelled", "error"): return s
        time.sleep(0.25)
    return None


# ---------------------------------------------------------------- synthetic data
RED, GREEN, BLUE, YELLOW = (220, 30, 30), (30, 200, 40), (30, 40, 220), (230, 220, 30)
CLS = {RED: "bottle", GREEN: "cup", BLUE: "person", YELLOW: "laptop"}


def display_frame(path, col, x, y, w, h, bg=128):
    im = Image.new("RGB", (640, 360), (0, 0, 0)); d = ImageDraw.Draw(im)
    d.rectangle([80, 0, 559, 359], fill=(bg, bg, bg))                       # 4:3 content with black pillar bars (letterbox)
    d.rectangle([x, y, x + w - 1, y + h - 1], fill=col); im.save(path, quality=92)


os.makedirs(os.path.join(WORK, "src")); os.makedirs(os.path.join(WORK, "dsp")); os.makedirs(JOBS); open(EVENTS, "w").close()
cam_files = []
for i in range(8):                                                           # camera frames: 480x360, red bottles / green cups
    col, (w, h) = ((RED, (40, 100)) if i % 2 == 0 else (GREEN, (50, 50)))
    im = Image.new("RGB", (480, 360), (128, 128, 128)); ImageDraw.Draw(im).rectangle([30 + i * 30, 40, 30 + i * 30 + w - 1, 40 + h - 1], fill=col)
    p = os.path.join(WORK, "src", f"c{i}.jpg"); im.save(p, quality=92); cam_files.append(p)
subprocess.run([INGEST, "--root", CAM, "--model", "none", "--span-minutes", "30"] + cam_files, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

now = int(time.time() * 1000)
T_A, T_B = now - 3600000, now - 3000000
truth = []                                                                    # (session, kind, id key, class, box)
args = [FIXTURE, "--root", POP, "--session", f"sa:ad-a:ad-a:{T_A}", "--session", f"sb:ad-b:ad-a:{T_B}"]
spec = [("sa", 0, RED, 100, 100, 40, 100), ("sa", 1, GREEN, 200, 120, 56, 56), ("sa", 2, BLUE, 300, 100, 60, 120), ("sa", 3, RED, 84, 80, 50, 120),
        ("sa", 4, YELLOW, 300, 100, 80, 50), ("sa", 5, RED, 500, 150, 40, 100), ("sb", 0, RED, 200, 100, 40, 100), ("sb", 1, GREEN, 300, 150, 56, 56), ("sb", 2, RED, 400, 90, 36, 96)]
for sid, k, col, x, y, w, h in spec:
    f = os.path.join(WORK, "dsp", f"{sid}_{k}.jpg"); display_frame(f, col, x, y, w, h)
    t = (T_A if sid == "sa" else T_B) + 500 + k * 1000
    args += ["--hist", f"{sid}:{k}:{t}:{f}:{'ad-a' if sid == 'sa' else 'ad-b'}"]
    truth.append({"session": sid, "k": k, "cls": CLS[col], "box": (x, y, w, h), "t": t})
N_HIST, N_FRAMES = len(spec), len(spec) + 1                                   # 9 history frames + 1 Weston checkpoint (the screenshot checkpoint is excluded)
N_BOTTLE = sum(1 for x in spec if x[2] == RED) + 1                              # + the red checkpoint
N_CUP, N_LAPTOP = sum(1 for x in spec if x[2] == GREEN), sum(1 for x in spec if x[2] == YELLOW)
N_DOBJ = N_BOTTLE + N_CUP + N_LAPTOP                                          # blue = person is never indexed
fck = os.path.join(WORK, "dsp", "sa_ck.jpg"); display_frame(fck, RED, 250, 150, 40, 100)
fbs = os.path.join(WORK, "dsp", "sb_bs.jpg"); display_frame(fbs, RED, 100, 100, 40, 100)
args += ["--ckpt", f"sa:1:{T_A + 6000}:{fck}", "--ckpt", f"sb:1:{T_B + 6000}:{fbs}:brightscript_screenshot"]    # the screenshot checkpoint is not Weston output
subprocess.run(args, check=True, stdout=subprocess.DEVNULL)

cfg = os.path.join(WORK, "cfg.json"); write_cfg(cfg)
srv = start(cfg, "api")

# ---------------------------------------------------------------- status / display frame listing / images
st, s, _ = call("GET", "/api/vm/status")
d = s["objects"].get("display", {})
check("status: a display block exists, nothing indexed yet, display auto enabled", d.get("available") is True and d["regions"] == 0 and d["auto_enabled"] is True and s["objects"]["regions"] == 0, d)
st, a, _ = call("GET", "/api/vm/objects/auto/status?source=display")
check("auto status (display): enabled+running, 10 Weston frames stored (the screenshot checkpoint is excluded), none processed",
      st == 200 and a["enabled"] and a["running"] and a["frames"]["stored"] == N_FRAMES and a["progress"]["frames_processed"] == 0 and "display objects only" in a["scope"]["message"], (a.get("frames"), a.get("scope")))
st, fl, _ = call("GET", "/api/vm/display-frames?limit=100")
frames = fl.get("frames", [])
check("display-frames: newest first, 10 Weston frames (9 history + 1 checkpoint), kinds history/checkpoint, ids start at 1 like the camera's",
      st == 200 and fl["total"] == N_FRAMES and [f["capture_utc_ms"] for f in frames] == sorted((f["capture_utc_ms"] for f in frames), reverse=True)
      and {f["kind"] for f in frames} == {"history", "checkpoint"} and min(f["id"] for f in frames if f["kind"] == "history") == 1, (st, fl.get("total")))
hist = {f["id"]: f for f in frames if f["kind"] == "history"}
f1 = hist[1]
check("a display frame carries session, expected/reported ad, offset, uncertainty and a session link",
      f1["session_id"] == "sa" and f1["expected_media_id"] == "ad-a" and f1["reported_media_id"] == "ad-a" and f1["playback_offset_ms"] == 520 and f1["time_uncertainty_ms"] == 252 and f1["session_link"] == "#pop?session=sa", f1)
st, b, _ = call("GET", f1["image_url"])
im = Image.open(io.BytesIO(b)); check("the frame image is the stored 640x360 Weston JPEG", st == 200 and im.size == (640, 360), (st, im.size if st == 200 else ""))
st, b, _ = call("GET", "/api/vm/display-frames/history/1/crop?x=100&y=100&w=40&h=100&max=120")
check("the crop preview of a display frame is a JPEG of the box", st == 200 and Image.open(io.BytesIO(b)).size == (40, 100), st)
st, r, _ = call("GET", "/api/vm/display-frames/history/1/crop?x=900&y=0&w=50&h=50"); check("crop outside the frame -> 400", st == 400, (st, r))
st, r, _ = call("GET", "/api/vm/display-frames/bogus/1/image"); check("bad kind -> 400", st == 400, (st, r))
st, r, _ = call("GET", "/api/vm/display-frames/history/9999/image"); check("unknown frame -> 404", st == 404, (st, r))
st, sl, _ = call("GET", "/api/vm/display-frames?session_id=sb&limit=100"); check("session filter (the screenshot checkpoint of sb is not Weston output)", st == 200 and sl["total"] == 3 and all(f["session_id"] == "sb" for f in sl["frames"]), sl.get("total"))

# ---------------------------------------------------------------- Test A: camera regression (unchanged behaviour)
st, r, _ = call("POST", "/api/vm/objects/auto/backfill", {"limit": 20})                       # source defaults to camera
check("camera backfill (default source) accepted", st == 202 and r["source"] == "camera", (st, r))
fin = wait_backfill("camera")
check("camera: 8 frames processed, bottles + cups indexed", fin and fin["backfill"]["state"] == "done" and fin["progress"]["frames_processed"] == 8 and fin["objects"]["auto"] == 8, fin and (fin["backfill"], fin["objects"]))
st, cl, _ = call("GET", "/api/vm/objects?limit=100")
check("object list defaults to camera objects only (nothing from the display)", st == 200 and cl["total"] == 8 and all(o["source_type"] == "camera" for o in cl["objects"]), cl.get("total"))
cam_bottle = [o for o in cl["objects"] if o["class_name"] == "bottle"][0]
st, r, _ = call("POST", "/api/vm/objects/search", {"region_id": cam_bottle["region_id"], "top_k": 20})
check("camera search: default scope = camera, 3 other bottles first then cups, camera scope text", st == 200 and r["scope"]["source_type"] == "camera" and len(r["results"]) == 7
      and [x["class_name"] for x in r["results"][:3]] == ["bottle"] * 3 and all(x["source_type"] == "camera" for x in r["results"]), (st, [x["class_name"] for x in r.get("results", [])]))

# ---------------------------------------------------------------- Test B: display object indexing (bounded backfill of retained Weston frames)
st, r, _ = call("POST", "/api/vm/objects/auto/backfill", {"source": "display", "limit": 50})
check("display backfill accepted", st == 202 and r["source"] == "display", (st, r))
fin = wait_backfill("display")
check("display: all 10 Weston frames processed and tracked per source", fin and fin["backfill"]["state"] == "done" and fin["backfill"]["source"] == "display" and fin["progress"]["frames_processed"] == N_FRAMES, fin and (fin["backfill"], fin["progress"]))
check("display: bottles, cups and the laptop per the allowlist; the blue 'person' is never indexed",
      fin["objects"]["auto"] == N_DOBJ and {c["class_name"]: c["count"] for c in fin["by_class"]} == {"bottle": N_BOTTLE, "cup": N_CUP, "laptop": N_LAPTOP}, (fin["objects"], fin["by_class"]))
st, dl, _ = call("GET", "/api/vm/objects?source_type=display&limit=100")
objs = dl["objects"]
check("list source_type=display: only display objects with their Proof-of-Play facts", st == 200 and dl["total"] == N_DOBJ and all(o["source_type"] == "display" and o["pop_session_id"] in ("sa", "sb") for o in objs), dl.get("total"))
ok_box = True
for o in objs:
    if o["source_kind"] != "history": continue
    t = [x for x in truth if x["t"] == o["capture_utc_ms"]]
    if not t or tuple(o["bbox"][k] for k in "xywh") != t[0]["box"] or o["class_name"] != t[0]["cls"] or o["image_width"] != 640 or o["image_height"] != 360: ok_box = False; print("   mismatch", o, t)
check("every display object has the right class, box (in the stored 640x360 frame, incl. next to the letterbox bar) and capture timestamp", ok_box)
o_sb = [o for o in objs if o["pop_session_id"] == "sb"][0]
check("session sb: expected ad-b but the player reported ad-a (both kept, with offset/uncertainty/provenance/link)",
      o_sb["expected_media_id"] == "ad-b" and o_sb["reported_media_id"] == "ad-a" and o_sb["playback_offset_ms"] > 0 and o_sb["time_uncertainty_ms"] == 252 and "weston_vop2" in o_sb["provenance"] and o_sb["session_link"] == "#pop?session=sb", o_sb)
check("crops retained, detector provenance present", all(o["crop_state"] == "retained" and o["detector"] == "mock_detector" and o["confidence"] > 0.5 and o["index_origin"] == "auto" for o in objs))
st, b, _ = call("GET", "/api/vm/objects/%d/image" % objs[0]["region_id"]); check("display object crop image served (own crop JPEG)", st == 200 and b[:2] == b"\xff\xd8", st)

# ---------------------------------------------------------------- Test C: display object search (select from a display frame, search indexed display objects)
disp_bottle = [o for o in objs if o["class_name"] == "bottle" and o["pop_session_id"] == "sa" and o["source_kind"] == "history"][0]
st, r, _ = call("POST", "/api/vm/objects/search", {"region_id": disp_bottle["region_id"], "top_k": 20})
res = r["results"]
check("search from a display object: scope defaults to display, indexed display objects only, scope text says so",
      st == 200 and r["scope"]["source_type"] == "display" and "display objects only" in r["scope"]["message"] and all(x["source_type"] == "display" for x in res) and len(res) == N_DOBJ - 1, (st, r.get("scope")))
check("similar display bottles first (the other %d bottles, then the rest), with timestamps/sessions/offsets/availability" % (N_BOTTLE - 1),
      [x["class_name"] for x in res[:N_BOTTLE - 1]] == ["bottle"] * (N_BOTTLE - 1) and all(x["pop_session_id"] and x["capture_utc_ms"] > 0 and x["crop_available"] and x["image_exists"] for x in res), [x["class_name"] for x in res])
st, r, _ = call("POST", "/api/vm/objects/search", {"frame_source_type": "display", "kind": "history", "frame_id": 1, "x": 100, "y": 100, "w": 40, "h": 100, "top_k": 5})
check("search with a box selected on a stored DISPLAY frame: display scope, the identical stored object is the top hit",
      st == 200 and r["scope"]["source_type"] == "display" and r["query"]["frame_source_type"] == "display" and r["results"][0]["similarity"] > 0.999 and all(x["source_type"] == "display" for x in r["results"]), (st, r.get("results", [{}])[0]))
check("results carry a link to the Proof-of-Play session and its frame image", r["results"][0]["session_link"].startswith("#pop?session=") and r["results"][0]["frame_image_url"].startswith("/api/vm/display-frames/history/"), r["results"][0])

# ---------------------------------------------------------------- Test D: playback correlation
top = res[0]
st, sess, _ = call("GET", "/api/pop/sessions/" + disp_bottle["pop_session_id"])
check("the result's session opens in Proof of Play with the same expected / reported advertisement", st == 200 and sess["expected_media_id"] == disp_bottle["expected_media_id"] and sess["played_media_id"] == disp_bottle["reported_media_id"], (st, str(sess)[:200]))
st, hf, _ = call("GET", "/api/pop/history/frames?limit=100&session_id=" + disp_bottle["pop_session_id"])
mine = [f for f in hf["frames"] if f["frame_id"] == disp_bottle["frame_id"]]
check("the PoP history frame has the same capture time and playback offset as the object", st == 200 and mine and mine[0]["utc_ms"] == disp_bottle["capture_utc_ms"] and mine[0]["actual_offset_ms"] == disp_bottle["playback_offset_ms"], (mine[:1], disp_bottle["capture_utc_ms"]))
iso = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(disp_bottle["capture_utc_ms"] // 1000)) + ".%03dZ" % (disp_bottle["capture_utc_ms"] % 1000)
st, tf, _ = call("GET", "/api/display/history/nearest?time=%s&max_diff_ms=500" % iso)
check("the time-search API finds the same display frame at the object's capture time", st == 200 and tf["found"] and tf["frame"]["frame_id"] == disp_bottle["frame_id"] and tf["frame"]["session_id"] == "sa", (st, str(tf)[:200]))

# ---------------------------------------------------------------- Test E: source isolation and id collisions
cam_ids = {o["frame_id"] for o in cl["objects"]}; disp_hist_ids = {o["frame_id"] for o in objs if o["source_kind"] == "history"}
check("camera frame ids and Proof-of-Play ids really do overlap in this data set", len(cam_ids & disp_hist_ids) > 0, (sorted(cam_ids), sorted(disp_hist_ids)))
st, l1, _ = call("GET", "/api/vm/objects?source_type=camera&frame_id=1&limit=100"); st, l2, _ = call("GET", "/api/vm/objects?source_type=display&kind=history&frame_id=1&limit=100")
check("the same number selects different objects per source", l1["total"] >= 1 and l2["total"] >= 1 and {o["region_id"] for o in l1["objects"]}.isdisjoint({o["region_id"] for o in l2["objects"]}), (l1["total"], l2["total"]))
st, ra, _ = call("GET", "/api/vm/objects?source_type=all&limit=100"); check("source_type=all lists both, explicitly", st == 200 and ra["total"] == 8 + N_DOBJ, ra.get("total"))
st, r, _ = call("GET", "/api/vm/objects?source_type=bogus"); check("bad source_type -> 400", st == 400, (st, r))
st, r, _ = call("POST", "/api/vm/objects/search", {"region_id": disp_bottle["region_id"], "top_k": 30, "source_type": "camera"})
check("explicit camera scope with a display query returns camera objects only (the vector spaces are the same, the sources are not mixed)", st == 200 and all(x["source_type"] == "camera" for x in r["results"]) and len(r["results"]) == 8, (st, len(r.get("results", []))))
st, r, _ = call("POST", "/api/vm/objects/search", {"region_id": disp_bottle["region_id"], "top_k": 30, "source_type": "all"})
check("only an explicit 'all' mixes them", st == 200 and {x["source_type"] for x in r["results"]} == {"camera", "display"} and len(r["results"]) == 8 + N_DOBJ - 1, (st, len(r.get("results", []))))
st, sc, _ = call("GET", "/api/vm/objects/auto/status?source=camera"); st2, sd, _ = call("GET", "/api/vm/objects/auto/status?source=display")
check("progress is kept per source (8 camera frames, 10 display frames)", sc["progress"]["frames_processed"] == 8 and sd["progress"]["frames_processed"] == N_FRAMES and sc["objects"]["total"] == 8 and sd["objects"]["total"] == N_DOBJ, (sc["progress"], sd["progress"]))

# ---------------------------------------------------------------- manual indexing on a display frame
st, r, _ = call("POST", "/api/vm/objects", {"source_type": "display", "kind": "history", "frame_id": 2, "x": 10, "y": 10, "w": 100, "h": 100, "label": "bars"})
check("manual object on a display frame: 201, source display, stored in 640x360 frame coordinates, crop retained, session facts",
      st == 201 and r["region"]["source_type"] == "display" and r["region"]["index_origin"] == "manual" and r["region"]["crop_state"] == "retained" and r["region"]["pop_session_id"] == "sa"
      and (r["region"]["bbox"]["x"], r["region"]["bbox"]["w"], r["region"]["image_width"]) == (10, 100, 640), (st, str(r)[:300]))
st, r2, _ = call("POST", "/api/vm/objects", {"source_type": "display", "kind": "history", "frame_id": 2, "x": 200, "y": 120, "w": 56, "h": 56, "label": "my cup"})
check("a manual box equal to an automatic detection: no second object (duplicate); it stays automatic, and the person's label wins", st == 200 and r2["status"] == "duplicate" and r2["region"]["index_origin"] == "auto" and r2["region"]["label"] == "my cup", (st, str(r2)[:300]))
for name, body in [("bad kind", {"source_type": "display", "kind": "nope", "frame_id": 2, "x": 200, "y": 120, "w": 56, "h": 56}), ("unknown frame", {"source_type": "display", "kind": "history", "frame_id": 9999, "x": 1, "y": 1, "w": 50, "h": 50}),
                   ("bad source", {"source_type": "hdmi", "frame_id": 2, "x": 1, "y": 1, "w": 50, "h": 50})]:
    st, r, _ = call("POST", "/api/vm/objects", body); check("manual index rejected: " + name, st in (400, 404), (st, r))

# ---------------------------------------------------------------- Test F: expired parent frames
dirs = os.path.join(POP, "pop", "history"); removed = 0
for dp, _, fn in os.walk(dirs):
    for f in fn:
        if f.endswith(".jpg"): os.remove(os.path.join(dp, f)); removed += 1
check("display history JPEGs removed (simulated retention)", removed >= 9, removed)
st, r, _ = call("GET", "/api/vm/display-frames/history/1/image")
check("the expired display frame answers: JPEG missing (the files are gone), no image served", st in (404, 410), (st, r))
st, b, _ = call("GET", "/api/vm/objects/%d/image" % disp_bottle["region_id"])
check("the object's own crop is still served after its parent frame is gone", st == 200 and b[:2] == b"\xff\xd8", st)
st, det, _ = call("GET", "/api/vm/objects/%d" % disp_bottle["region_id"])
check("object detail reports the parent frame accurately (flag still set, file missing) and keeps session/timestamp", st == 200 and det["image_exists"] is False and det["pop_session_id"] == "sa" and det["capture_utc_ms"] == disp_bottle["capture_utc_ms"], det)
st, r, _ = call("POST", "/api/vm/objects/search", {"region_id": disp_bottle["region_id"], "top_k": 20, "require_image": True})
check("require_image keeps display objects whose crop is retained (history parents are gone; the checkpoint parent, kept elsewhere, is still reported present)", st == 200 and len(r["results"]) == N_DOBJ and all(x["crop_available"] for x in r["results"])
      and all(not x["image_exists"] for x in r["results"] if x["source_kind"] == "history") and any(x["image_exists"] for x in r["results"] if x["source_kind"] == "checkpoint"), (st, len(r.get("results", []))))

# ---------------------------------------------------------------- restart: progress per source persists, nothing is detected twice
stop(srv); srv = start(cfg, "api2")
st, r, _ = call("POST", "/api/vm/objects/auto/backfill", {"source": "display", "limit": 50}); fin2 = wait_backfill("display")
check("after a restart a display backfill finds nothing new (expired frames are not listed, processed ones are skipped)", st == 202 and fin2["backfill"]["total"] == 0 and fin2["objects"]["total"] == N_DOBJ + 1, fin2 and (fin2["backfill"], fin2["objects"]))
st, r, _ = call("POST", "/api/vm/objects/auto/backfill", {"source": "bogus"}); check("bad backfill source -> 400", st == 400, (st, r))
stop(srv)

# ---------------------------------------------------------------- display indexing disabled: camera unaffected, display backfill refused, manual still possible
write_cfg(cfg, display=False); srv = start(cfg, "api3")
st, r, _ = call("POST", "/api/vm/objects/auto/backfill", {"source": "display", "limit": 5}); check("display auto indexing OFF: backfill refused (503) with the reason", st == 503 and "display" in r["message"], (st, r))
st, a, _ = call("GET", "/api/vm/objects/auto/status?source=display"); check("display status says disabled", st == 200 and a["enabled"] is False and a["running"] is False, a.get("enabled"))
st, a, _ = call("GET", "/api/vm/objects/auto/status?source=camera"); check("camera automatic indexing unaffected", st == 200 and a["enabled"] and a["running"], a.get("enabled"))
st, ol, _ = call("GET", "/api/vm/objects?source_type=display&limit=100"); check("existing display objects remain searchable", st == 200 and ol["total"] == N_DOBJ + 1, ol.get("total"))
# manual indexing of a display crop must not depend on the detector at all: display auto indexing is OFF here, so no display worker/detector run is involved
before = ol["total"]
st, fl2, _ = call("GET", "/api/vm/display-frames?limit=100")
live = [f for f in fl2["frames"] if f["kind"] == "checkpoint"]                       # Test F removed every history JPEG; the Weston checkpoint JPEG (kept elsewhere) is still there
check("a retained display frame (the Weston checkpoint) is still available for the manual test", st == 200 and len(live) >= 1, (st, fl2.get("total")))
st, r, _ = call("POST", "/api/vm/objects", {"source_type": "display", "kind": "checkpoint", "frame_id": live[0]["id"], "x": 20, "y": 20, "w": 90, "h": 90, "label": "manual, no detector"})
check("manual display object with display auto indexing OFF: 201, source display, origin manual, crop retained, session facts",
      st == 201 and r["region"]["source_type"] == "display" and r["region"]["index_origin"] == "manual" and r["region"]["crop_state"] == "retained" and r["region"]["pop_session_id"] == "sa", (st, str(r)[:300]))
rid = r["region"]["region_id"] if st == 201 else 0
st, r, _ = call("POST", "/api/vm/objects/search", {"region_id": rid, "top_k": 5})
check("a manually indexed display crop is searchable: display scope, a result list with the other display objects", st == 200 and r["scope"]["source_type"] == "display" and len(r["results"]) >= 1 and all(x["source_type"] == "display" for x in r["results"]), (st, str(r)[:200]))
st, ol2, _ = call("GET", "/api/vm/objects?source_type=display&limit=100"); check("exactly one more display object, and no automatic object was added", st == 200 and ol2["total"] == before + 1 and sum(1 for o in ol2["objects"] if o["index_origin"] == "auto") == sum(1 for o in ol["objects"] if o["index_origin"] == "auto"), ol2.get("total"))
stop(srv)

print("RESULT %d failed" % len(fails) if fails else "RESULT all passed")
sys.exit(1 if fails else 0)
