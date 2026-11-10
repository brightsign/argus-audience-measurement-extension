#!/usr/bin/env python3
"""Host self-test of the HARDWARE validation script (tests/api/vm_objects_display_hw.py): proves the script's plumbing before it touches a player.

  vm_objects_display_hw_selftest.py <vm_api_server> <vm_ingest_dir> <vm_pop_fixture> <workdir> <product_frames_dir> <manifest.json>

Builds a host store: 30 one-per-second display frames in the OS's 640x360 letterboxed geometry become a fake Proof-of-Play session "sp" of advertisement ad-c, and
six synthetic camera frames give the camera source objects whose ids overlap with the display ids.
The server uses the MOCK detector and embedder, which only find flat saturated colour blocks: so each stored frame is a NEUTRAL grey 640x360 letterboxed frame (not the video content)
with a pure red block (= "bottle", scene 1) or a pure green block (= "cup", scene 2) painted at the ground-truth box of the video (scene 3 gets none). The mock detector cannot separate two
objects in one frame, so the two-objects-per-frame detection rates of the `auto` stage are allowed to fail here; they are validated offline (`evaluate` stage, tested on the host
float-YOLOX results and on a deliberately sabotaged result) and, for the real detector, on the player. That makes every stage, including the ground-truth evaluation, runnable on the host.
The REAL detector is exercised only on the player; this proves the script, not the detector.
"""
import importlib.util, json, os, shutil, signal, subprocess, sys, time, urllib.request
from PIL import Image, ImageDraw

SERVER, INGEST, FIXTURE, WORK, FRAMES, MAN = sys.argv[1:7]
HW = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vm_objects_display_hw.py")
PORT = 20477; BASE = "http://127.0.0.1:%d" % PORT
shutil.rmtree(WORK, ignore_errors=True); os.makedirs(WORK)
CAM, POP = os.path.join(WORK, "cam"), os.path.join(WORK, "pop")
EVENTS, JOBS = os.path.join(WORK, "events.jsonl"), os.path.join(WORK, "jobs")
os.makedirs(JOBS); open(EVENTS, "w").close()
spec = importlib.util.spec_from_file_location("hwmod", HW); hwmod = importlib.util.module_from_spec(spec); spec.loader.exec_module(hwmod)
man = json.load(open(MAN)); os.makedirs(os.path.join(WORK, "dsp")); os.makedirs(os.path.join(WORK, "camsrc"))
RED, GREEN = (220, 30, 30), (30, 200, 40)
dsp = {}
for k in range(30):
    im = Image.new("RGB", (640, 360), (0, 0, 0)); d = ImageDraw.Draw(im); d.rectangle([32, 0, 607, 359], fill=(128, 128, 128)); t = k + 0.52; sc = hwmod.scene_of(t)   # neutral letterboxed frame
    if sc < 2 and min(abs(t - 10), abs(t - 20)) >= hwmod.BOUNDARY_GUARD_S:
        for lab, col in ((("water bottle", RED),) if sc == 0 else (("coffee cup", GREEN),)):      # one object per scene: the mock detector merges all coloured pixels of a frame into ONE box
            g = hwmod.gt_box(man, sc, lab, t); d.rectangle([int(g[0]), int(g[1]), int(g[0] + g[2]) - 1, int(g[1] + g[3]) - 1], fill=col)
    dsp[k] = os.path.join(WORK, "dsp", "%d.jpg" % k); im.save(dsp[k], quality=92)
cam_files = []
for i in range(6):                                                           # camera frames: 480x360, red bottles / green cups (ids overlap with the display ids)
    col, (w, h) = ((RED, (40, 100)) if i % 2 == 0 else (GREEN, (50, 50)))
    im = Image.new("RGB", (480, 360), (128, 128, 128)); ImageDraw.Draw(im).rectangle([30 + i * 30, 40, 30 + i * 30 + w - 1, 40 + h - 1], fill=col)
    p = os.path.join(WORK, "camsrc", "c%d.jpg" % i); im.save(p, quality=92); cam_files.append(p)
subprocess.run([INGEST, "--root", CAM, "--model", "none", "--span-minutes", "30"] + cam_files, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
T = int(time.time() * 1000) - 3600000
args = [FIXTURE, "--root", POP, "--session", "sp:ad-c:ad-c:%d" % T]
for k in range(30): args += ["--hist", "sp:%d:%d:%s:ad-c" % (k, T + 500 + k * 1000, dsp[k])]
args += ["--ckpt", "sp:1:%d:%s" % (T + 3500, dsp[3])]
subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
cfg = os.path.join(WORK, "cfg.json")


def write_cfg(display_auto):
    json.dump({"visual_memory": {"enabled": True, "vector_search_enabled": True, "storage_root": CAM, "stored_width": 640, "stored_height": 360,
                                 "embedding": {"enabled": True, "backend": "rknn", "model_path": "x"},
                                 "objects": {"enabled": True, "auto": {"enabled": True, "live_scan": False, "backfill_pause_ms": 20, "max_backfill_frames": 60, "min_confidence": 0.30, "display": {"enabled": display_auto, "live_scan": False}}},
                                 "pop": {"enabled": True, "storage_root": POP, "events_path": EVENTS, "jobs_dir": JOBS, "poll_ms": 100, "capture_backend": "resilience_grpc", "history": {"enabled": True}}}}, open(cfg, "w"))


def start():
    p = subprocess.Popen([SERVER, "--config", cfg, "--port", str(PORT), "--provider", "mock:64", "--detector", "mock", "--no-reexec", "--status-file", os.path.join(WORK, "st.json")],
                         stdout=open(os.path.join(WORK, "srv.log"), "a"), stderr=subprocess.STDOUT)
    for _ in range(80):
        try:
            if urllib.request.urlopen(BASE + "/api/vm/status", timeout=2).status == 200: return p
        except Exception: time.sleep(0.2)
    p.kill(); raise SystemExit("server did not start")


def stop(p):
    p.send_signal(signal.SIGTERM)
    try: p.wait(10)
    except Exception: p.kill()


def hw(*a):
    r = subprocess.run([sys.executable, HW, "--api", BASE] + list(a), capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


results, bad = [], []


def run(name, rc_expected, *a, allowed_fail_substr=()):
    rc, out = hw(*a)
    fl = [l for l in out.splitlines() if l.startswith("FAIL")]
    unexpected = [l for l in fl if not any(s in l for s in allowed_fail_substr)]
    ok = (rc == 0 and not fl) if not allowed_fail_substr else (not unexpected)
    print(("PASS " if ok else "FAIL ") + "hw stage %s (rc=%d, %d FAIL lines, %d unexpected)" % (name, rc, len(fl), len(unexpected)))
    for l in unexpected: print("    " + l[:300])
    if not ok: bad.append(name); print(out[-1500:])
    return out


write_cfg(False); srv = start()
BL = os.path.join(WORK, "baseline.json")
run("baseline", 0, "baseline", "--out", BL)
# camera objects must exist for baseline/compare to be meaningful
urllib.request.urlopen(urllib.request.Request(BASE + "/api/vm/objects/auto/backfill", data=json.dumps({"limit": 20}).encode(), headers={"Content-Type": "application/json", "Origin": BASE}, method="POST"))
time.sleep(3)
run("baseline (with camera objects)", 0, "baseline", "--out", BL)
run("manual (display auto indexing OFF: no detector involved)", 0, "manual", "--session", "sp", "--manifest", MAN)
stop(srv); write_cfg(True); srv = start()
run("auto (backfill, provenance, session facts; the two-objects-per-frame truth rates need the real detector and are allowed to fail here)", 1, "auto", "--session", "sp", "--manifest", MAN,
    "--out", os.path.join(WORK, "auto_report.json"), allowed_fail_substr=("found in >=", "centre inside the ground-truth"))
run("search", 0, "search", "--session", "sp")
run("isolation", 0, "isolation")
out = run("expired", 0, "expired"); print("   expired stage note:", [l for l in out.splitlines() if l.startswith("NOTE")][:1])
run("compare (camera regression vs baseline)", 0, "compare", "--baseline", BL)
stop(srv)
print("RESULT %s" % ("FAIL: " + ", ".join(bad) if bad else "all stages pass"))
sys.exit(1 if bad else 0)
