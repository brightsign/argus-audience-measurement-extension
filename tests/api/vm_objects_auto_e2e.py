#!/usr/bin/env python3
"""Host end-to-end test of D4 automatic object indexing against a real vm_api_server (mock detector + mock embedder).

  vm_objects_auto_e2e.py VM_API_SERVER VM_INGEST_DIR WORKDIR

Builds a synthetic store (coloured shapes on varying backgrounds, 480x360 like the player's stored frames), starts the
server with objects.auto enabled (live scan OFF, bounded backfill only), and checks provenance, class filters, crops that
outlive the full-frame JPEGs, bounded/concurrent/cancelled backfill, restart persistence, no blocking of API requests,
the v1 -> v2 migration through the server, and that auto indexing is inert when not enabled. No RKNN, no player.
"""
import json, os, shutil, signal, sqlite3, subprocess, sys, time, urllib.error, urllib.request
from PIL import Image, ImageDraw

SERVER, INGEST, WORK = sys.argv[1], sys.argv[2], sys.argv[3]
PORT = 20431
BASE = f"http://127.0.0.1:{PORT}"
shutil.rmtree(WORK, ignore_errors=True)
os.makedirs(WORK)
ROOT = os.path.join(WORK, "store")
fails = []
procs = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(extra)[:300]) if not cond and extra != "" else ""), flush=True)
    if not cond:
        fails.append(name)


def call(method, path, body=None, raw=None, timeout=20):
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


def write_cfg(path, auto_enabled=True, pause=150, extra_auto=None):
    auto = {"enabled": auto_enabled, "live_scan": False, "backfill_pause_ms": pause, "max_backfill_frames": 40, "min_confidence": 0.30}
    auto.update(extra_auto or {})
    json.dump({"visual_memory": {"enabled": True, "vector_search_enabled": True, "storage_root": ROOT, "stored_width": 640, "stored_height": 360,
                                 "embedding": {"enabled": True, "backend": "rknn", "model_path": "model/none.rknn"},
                                 "objects": {"enabled": True, "max_regions": 10000, "retention_days": 30, "auto": auto}}}, open(path, "w"))


def start(cfg, name="api"):
    p = subprocess.Popen([SERVER, "--config", cfg, "--port", str(PORT), "--provider", "mock:64", "--detector", "mock", "--no-reexec",
                          "--status-file", os.path.join(WORK, "st.json")], stdout=open(os.path.join(WORK, name + ".log"), "w"), stderr=subprocess.STDOUT)
    procs.append(p)
    for _ in range(60):
        try:
            if call("GET", "/api/vm/status")[0] == 200:
                return p
        except Exception:
            pass
        time.sleep(0.2)
    raise SystemExit("server did not start")


def stop(p):
    p.send_signal(signal.SIGTERM)
    try:
        p.wait(10)
    except Exception:
        p.kill()


def wait_backfill(timeout=40):
    t0 = time.time()
    while time.time() - t0 < timeout:
        st, s, _ = call("GET", "/api/vm/objects/auto/status")
        if st == 200 and s.get("backfill", {}).get("state") in ("done", "cancelled", "error"):
            return s
        time.sleep(0.25)
    return None


# ---------------------------------------------------------------- synthetic store: frame i has ONE shape
BG = [(128, 128, 128), (60, 70, 80), (200, 190, 180), (90, 120, 90)]
SHAPES = [("bottle", (220, 30, 30), (40, 100)), ("cup", (30, 200, 40), (50, 50)), ("person", (30, 40, 220), (60, 120)), ("laptop", (230, 220, 30), (80, 50)), ("bottle", (220, 30, 30), (36, 96))]
N = 20
truth = []
os.makedirs(os.path.join(WORK, "src"))
files = []
for i in range(N):
    kind, col, (w, h) = SHAPES[i % 5]
    x, y = 30 + (i * 37) % 300, 20 + (i * 23) % 200
    im = Image.new("RGB", (480, 360), BG[(i // 5) % 4])
    ImageDraw.Draw(im).rectangle([x, y, x + w - 1, y + h - 1], fill=col)
    p = os.path.join(WORK, "src", f"f{i:02d}.jpg")
    im.save(p, quality=92)
    files.append(p)
    truth.append({"kind": kind, "x": x, "y": y, "w": w, "h": h})
subprocess.run([INGEST, "--root", ROOT, "--model", "none", "--span-minutes", "30"] + files, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

# a v1 objects.db from an earlier D4 install (three manual regions) that the server must migrate without losing anything
V1 = ("CREATE TABLE object_models(model_id INTEGER PRIMARY KEY,model_name TEXT NOT NULL,model_version TEXT NOT NULL,model_sha256 TEXT NOT NULL,preprocessing TEXT NOT NULL,dimension INTEGER NOT NULL,dtype TEXT NOT NULL,metric TEXT NOT NULL,UNIQUE(model_name,model_version,model_sha256,preprocessing,dimension,dtype,metric));"
      "CREATE TABLE object_regions(region_id INTEGER PRIMARY KEY AUTOINCREMENT,frame_id INTEGER NOT NULL,source_id TEXT NOT NULL,capture_utc_ms INTEGER NOT NULL,bbox_x INTEGER NOT NULL,bbox_y INTEGER NOT NULL,bbox_width INTEGER NOT NULL,bbox_height INTEGER NOT NULL,image_width INTEGER NOT NULL,image_height INTEGER NOT NULL,object_label TEXT NOT NULL DEFAULT '',model_id INTEGER NOT NULL REFERENCES object_models(model_id),embedding_status INTEGER NOT NULL DEFAULT 1,crop_path TEXT,created_at INTEGER NOT NULL,frame_sha256 TEXT NOT NULL DEFAULT '',UNIQUE(frame_id,bbox_x,bbox_y,bbox_width,bbox_height,model_id));"
      "CREATE TABLE object_vectors(region_id INTEGER PRIMARY KEY REFERENCES object_regions(region_id) ON DELETE CASCADE,embedding BLOB NOT NULL);"
      "CREATE INDEX ix_obj_time ON object_regions(capture_utc_ms);CREATE INDEX ix_obj_src ON object_regions(source_id,capture_utc_ms);CREATE INDEX ix_obj_label ON object_regions(object_label);CREATE INDEX ix_obj_frame ON object_regions(frame_id);PRAGMA user_version=1;")
db = sqlite3.connect(os.path.join(ROOT, "objects.db"))
db.executescript(V1)
db.execute("INSERT INTO object_models VALUES(1,'old_model','1','x','pp1',4,'f32','cosine')")
for i in (1, 2, 3):
    db.execute("INSERT INTO object_regions(frame_id,source_id,capture_utc_ms,bbox_x,bbox_y,bbox_width,bbox_height,image_width,image_height,object_label,model_id,created_at) VALUES(?,?,?,?,?,?,?,?,?,?,1,?)",
               (20460 + i, "/dev/video1", 1793900000000 + i, 10, 20, 30, 40, 640, 360, f"old{i}", 1793900100000 + i))
    db.execute("INSERT INTO object_vectors VALUES(?,?)", (i, b"\x00\x00\x80\x3f" * 4))
db.commit(); db.close()
v1_bytes = open(os.path.join(ROOT, "objects.db"), "rb").read()

cfg = os.path.join(WORK, "cfg.json")
write_cfg(cfg)
srv = start(cfg)

# ---------------------------------------------------------------- migration, defaults
check("v1 objects.db was migrated (verbatim v1 backup kept, plus the v2 state before the v3 rebuild)", os.path.exists(os.path.join(ROOT, "objects.db.pre-v3")) and os.path.exists(os.path.join(ROOT, "objects.db.pre-v2")) and open(os.path.join(ROOT, "objects.db.pre-v2"), "rb").read() == v1_bytes)
st, s, _ = call("GET", "/api/vm/status")
ob = s["objects"]
check("status: 3 old regions kept as manual, schema 3 (v1 -> v2 -> v3)", ob["schema_version"] == 3 and ob["regions"] == 3 and ob["manual_regions"] == 3 and ob["auto_regions"] == 0, ob)
st, a, _ = call("GET", "/api/vm/objects/auto/status")
check("auto status: enabled/running, person excluded, allowlist verified against COCO ids",
      st == 200 and a["enabled"] and a["running"] and a["class_ids"] == [39, 41, 40, 63, 67, 73] and a["classes"] == ["bottle", "cup", "wine_glass", "laptop", "cell_phone", "book"] and 0 not in a["class_ids"], a)
check("auto status: scope says it only covers indexed/processed objects", "not searched" in a["scope"]["message"] and a["scope"]["coverage"] == "indexed_subset", a["scope"])
check("auto status: frames known", a["frames"]["stored"] == N and a["frames"]["with_retained_jpeg"] == N, a["frames"])
st, ol, _ = call("GET", "/api/vm/objects?limit=10")
check("old manual objects readable after migration (no crop, origin manual)", st == 200 and ol["total"] == 3 and all(o["index_origin"] == "manual" and o["crop_state"] == "none" for o in ol["objects"]), ol)
st, r, _ = call("GET", "/api/vm/objects/1/image")
check("old object without crop and without frame: 410 with an accurate reason", st == 410 and "no crop image was stored" in r["message"], (st, r))

# ---------------------------------------------------------------- bounded backfill
st, r, _ = call("POST", "/api/vm/objects/auto/backfill", {"limit": 999})
check("backfill limit above the configured cap is refused", st == 400, (st, r))
st, r, _ = call("POST", "/api/vm/objects/auto/backfill", {"frame_ids": [0]})
check("backfill bad frame id is refused", st == 400, (st, r))
st, r, _ = call("POST", "/api/vm/objects/auto/backfill", raw=b"nope")
check("backfill non-JSON body refused", st == 400, (st, r))
st, r, _ = call("POST", "/api/vm/objects/auto/backfill", {"limit": N})
check("backfill accepted (202), bounded", st == 202 and r["status"] == "queued" and r["limit"] == N, (st, r))
st2, r2, _ = call("POST", "/api/vm/objects/auto/backfill", {"limit": 5})
check("a second backfill while one runs -> 409", st2 == 409, (st2, r2))
lat = []
for _ in range(12):                                                 # the API stays responsive while the worker is busy
    lat.append(call("GET", "/api/vm/status")[2]); lat.append(call("GET", "/api/vm/objects?limit=5")[2]); time.sleep(0.1)
lat_sorted = sorted(lat)
check("API requests are not queued behind the worker (median < 50 ms; the worst case is one SQLite commit wait, < 2.5 s)", lat_sorted[len(lat) // 2] < 50 and max(lat) < 2500, [round(x) for x in lat])
fin = wait_backfill()
check("backfill finished", fin is not None and fin["backfill"]["state"] == "done" and fin["backfill"]["total"] == N and fin["backfill"]["done"] == N, fin and fin["backfill"])
check("worker: no errors, queue bounded", fin["worker"]["detector_errors"] == 0 and fin["worker"]["provider_errors"] == 0 and fin["worker"]["queue_capacity"] == 8 and fin["worker"]["last_error"] == "", fin["worker"])
# red bottles (kinds 0 and 4 -> 8 frames), green cups (4), yellow laptops (4); blue "person" is never indexed
check("progress: all frames processed, objects only from allowed classes",
      fin["progress"]["frames_processed"] == N and fin["objects"]["auto"] == 16 and fin["progress"]["frames_with_objects"] == 16, (fin["progress"], fin["objects"]))
by = {c["class_name"]: c["count"] for c in fin["by_class"]}
check("per-class counts", by == {"bottle": 8, "cup": 4, "laptop": 4}, by)

st, al, _ = call("GET", "/api/vm/objects?origin=auto&limit=100")
check("list origin=auto", st == 200 and al["total"] == 16, al["total"])
st, bl, _ = call("GET", "/api/vm/objects?class=bottle&limit=100")
check("list class=bottle", bl["total"] == 8 and all(o["class_name"] == "bottle" for o in bl["objects"]), bl["total"])
st, r, _ = call("GET", "/api/vm/objects?origin=weird")
check("origin filter validated", st == 400, (st, r))
o0 = bl["objects"][0]
check("provenance fields: class, confidence, detector, version, crop", o0["index_origin"] == "auto" and o0["class_id"] == 39 and 0.8 < o0["confidence"] <= 1 and o0["detector"] == "mock_detector"
      and o0["detector_version"] and o0["crop_state"] == "retained" and o0["crop_bytes"] > 0 and o0["crop_width"] > 0, o0)

# the stored box and timestamp are those of the right frame
st, fr, _ = call("GET", "/api/vm/frames?limit=100")
by_id = {f["id"]: f for f in fr["frames"]}
order = sorted(fr["frames"], key=lambda f: f["capture_utc_ms"])         # ingest order == capture order
ok_box = True
for o in al["objects"]:
    idx = order.index(by_id[o["frame_id"]])
    t = truth[idx]
    bx = o["bbox"]
    if o["capture_utc_ms"] != by_id[o["frame_id"]]["capture_utc_ms"] or abs(bx["x"] - t["x"]) > 1 or abs(bx["y"] - t["y"]) > 1 or abs(bx["w"] - t["w"]) > 1 or abs(bx["h"] - t["h"]) > 1 \
            or {"bottle": "bottle", "cup": "cup", "laptop": "laptop"}[t["kind"]] != o["class_name"]:
        ok_box = False; print("   mismatch", o, t)
check("every automatic object has the right class, box and capture timestamp", ok_box)

# ---------------------------------------------------------------- crops, expiry and search
st, b, _ = call("GET", "/api/vm/objects/%d/image" % o0["region_id"])
check("object crop image is its own JPEG", st == 200 and b[:2] == b"\xff\xd8", st)
im = Image.open(__import__("io").BytesIO(b)); check("crop dimensions match the detection", abs(im.size[0] - o0["crop_width"]) <= 1 and im.size[1] == o0["crop_height"], (im.size, o0["crop_width"], o0["crop_height"]))
# similar bottles rank above unrelated cups / laptops
st, r, _ = call("POST", "/api/vm/objects/search", {"region_id": o0["region_id"], "top_k": 10, "origin": "auto"})
res = r["results"]
check("search scope is reported and distinguishes the subset", st == 200 and r["scope"]["auto_objects"] == 16 and "not searched" in r["scope"]["message"], r.get("scope"))
check("similar bottles are retrieved first (7 bottles precede any cup/laptop)", [x["class_name"] for x in res[:7]] == ["bottle"] * 7 and all(x["class_name"] != "bottle" for x in res[7:]), [x["class_name"] for x in res])
check("results are sorted and carry crop/frame availability + timestamps", [x["similarity"] for x in res] == sorted((x["similarity"] for x in res), reverse=True)
      and all(x["crop_available"] and x["image_exists"] and x["capture_utc_ms"] > 0 for x in res), res[0])
st, r, _ = call("POST", "/api/vm/objects/search", {"region_id": o0["region_id"], "top_k": 10, "class": "cup"})
check("class filter in search", st == 200 and len(r["results"]) == 4 and all(x["class_name"] == "cup" for x in r["results"]), r)
st, r, _ = call("POST", "/api/vm/objects/search", {"region_id": o0["region_id"], "top_k": 10, "origin": "bad"})
check("origin validated in search", st == 400, r)

# a user's manual region on top of an automatic one keeps its label
f_for = o0["frame_id"]
st, r, _ = call("POST", "/api/vm/objects", {"frame_id": f_for, "x": o0["bbox"]["x"], "y": o0["bbox"]["y"], "w": o0["bbox"]["w"], "h": o0["bbox"]["h"], "label": "mine"})
check("manual re-index of an automatic region returns the existing one", st == 200 and r["status"] == "duplicate" and r["region"]["index_origin"] == "auto", r)
st, r, _ = call("POST", "/api/vm/objects", {"frame_id": order[1]["id"], "x": 10, "y": 10, "w": 100, "h": 100, "label": "manual-one"})
check("a manual object gets a crop too", st == 201 and r["region"]["crop_state"] == "retained" and r["region"]["index_origin"] == "manual", r)

# all full-frame JPEGs expire; object crops stay inspectable
frames_dir = os.path.join(ROOT, "frames")
removed = 0
for dp, dn, fn in os.walk(frames_dir):
    for f in fn:
        if f.endswith(".jpg"): os.remove(os.path.join(dp, f)); removed += 1
check("frame JPEGs removed for the expiry test", removed >= N, removed)
st, b, _ = call("GET", "/api/vm/frames/%d/image" % f_for)
check("full-frame image is gone", st in (404, 410), st)
st, b, _ = call("GET", "/api/vm/objects/%d/image" % o0["region_id"])
check("...but the object's crop JPEG is still served", st == 200 and b[:2] == b"\xff\xd8", st)
st, r, _ = call("POST", "/api/vm/objects/search", {"region_id": o0["region_id"], "top_k": 20, "require_image": True, "origin": "auto"})
check("require_image keeps objects whose crop is retained", st == 200 and len(r["results"]) == 15 and all(not x["image_exists"] and x["crop_available"] for x in r["results"]), (st, len(r.get("results", []))))
st, r, _ = call("GET", "/api/vm/objects/1/image")
check("expired old object (no crop, no frame) says so", st == 410 and "expired" in r["message"], r)

# ---------------------------------------------------------------- restart persistence, no duplicate work
before = call("GET", "/api/vm/objects/auto/status")[1]["progress"]["frames_processed"]
stop(srv)
srv = start(cfg, "api2")
a2 = call("GET", "/api/vm/objects/auto/status")[1]
check("progress persisted across a restart", a2["progress"]["frames_processed"] == before and a2["objects"]["auto"] == 16, a2["progress"])
st, r, _ = call("POST", "/api/vm/objects/auto/backfill", {"limit": 30})
fin2 = wait_backfill()
check("a range backfill after the restart finds nothing new to do", st == 202 and fin2["backfill"]["total"] == 0 and fin2["objects"]["auto"] == 16, fin2 and fin2["backfill"])
st, r, _ = call("POST", "/api/vm/objects/auto/backfill/cancel")
check("cancel with nothing running is harmless", st == 200, (st, r))

# ---------------------------------------------------------------- cancel a running backfill
st, r, _ = call("POST", "/api/vm/objects/auto/backfill", {"frame_ids": list(range(1, 11)), "retry_failed": True})
time.sleep(0.4)
call("POST", "/api/vm/objects/auto/backfill/cancel")
fin3 = wait_backfill()
check("a running backfill can be cancelled or completes cleanly", fin3 and fin3["backfill"]["state"] in ("cancelled", "done"), fin3 and fin3["backfill"])
stop(srv)

# ---------------------------------------------------------------- disabled by default: inert
write_cfg(cfg, auto_enabled=False)
srv = start(cfg, "api3")
st, a, _ = call("GET", "/api/vm/objects/auto/status")
check("auto disabled: not running, no worker block", st == 200 and a["enabled"] is False and a["running"] is False and "worker" not in a, a)
st, r, _ = call("POST", "/api/vm/objects/auto/backfill", {"limit": 3})
check("auto disabled: backfill refused (503)", st == 503, (st, r))
st, ol, _ = call("GET", "/api/vm/objects?limit=5")
check("auto disabled: existing objects (manual and automatic) are still searchable", st == 200 and ol["total"] >= 16, ol["total"])
stop(srv)

print("RESULT %d failed" % len(fails) if fails else "RESULT all passed")
sys.exit(1 if fails else 0)
