#!/usr/bin/env python3
"""Host test of scripts/pop/provision_demo.py (prepare + apply) against a real vm_api_server with an isolated schema-2 Proof of Play store.

  pop_provision_e2e.py VM_API_SERVER

Synthetic videos (ffmpeg test sources). Checks: both reference profiles are generated from the ORIGINAL videos with the right geometry, the
manifests record hashes / filters / model, apply registers both advertisements and builds references whose counts equal the bundle, the registry
the API reports matches, a schema-1 stand-in pop.db in the Visual Memory root is byte-identical afterwards, and a second apply is idempotent.
"""
import hashlib, json, os, shutil, signal, subprocess, sys, tempfile, time, traceback, urllib.request
from PIL import Image

API_BIN = sys.argv[1]
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(os.path.dirname(HERE))
PORT = 20497
tmp = tempfile.mkdtemp(prefix="popprov_"); fails = []; procs = []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(extra)[:300]) if not cond and extra != "" else ""), flush=True)
    if not cond: fails.append(name)


def sh(sha_path):
    return hashlib.sha256(open(sha_path, "rb").read()).hexdigest()


try:
    vids = {}
    for mid, src in (("ad-a", "testsrc2=size=1920x1080:rate=25"), ("ad-b", "mandelbrot=size=1280x720:rate=25")):
        vids[mid] = os.path.join(tmp, mid + ".mp4")
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", src, "-t", "6", "-pix_fmt", "yuv420p", "-c:v", "libx264", vids[mid]], check=True)
    bundle = os.path.join(tmp, "bundle")
    r = subprocess.run([sys.executable, os.path.join(ROOT, "scripts/pop/provision_demo.py"), "prepare", "--out", bundle, "--slot", "30",
                        "--ad", f"ad-a=Ad A={vids['ad-a']}", "--ad", f"ad-b=Ad B={vids['ad-b']}"], capture_output=True, text=True)
    check("prepare succeeds", r.returncode == 0, r.stderr[-300:])
    summ = {s["media_id"]: s for s in json.load(open(os.path.join(bundle, "bundle_summary.json")))}
    check("both profiles generated for both ads (6 s at 1 s spacing = 6 frames each)", all(s["screenshot_refs"] == 6 and s["display_refs"] == 6 for s in summ.values()), summ)
    check("media hashes in the bundle are the SHA-256 of the original files", all(summ[m]["sha256"] == sh(vids[m]) for m in vids))
    d = Image.open(os.path.join(bundle, "refs_incoming", "ad-a.display_letterbox", "0.jpg")); s_ = Image.open(os.path.join(bundle, "refs_incoming", "ad-a", "0.jpg"))
    check("display-profile reference is 640x360 with black side bars; the screenshot profile is 640x400", d.size == (640, 360) and s_.size == (640, 400) and
          max(d.convert("L").getpixel((x, 180)) for x in range(0, 30)) < 8 and max(d.convert("L").getpixel((x, 180)) for x in range(610, 640)) < 8, (d.size, s_.size))
    man = json.load(open(os.path.join(bundle, "refs_incoming", "ad-a.manifest.json")))
    check("manifest records hash, exact filter, model version and the original-video source", man["video_sha256"] == sh(vids["ad-a"]) and "crop=1280:800:0:0" in man["ffmpeg_filter"] and man["embedding_model"] and "ORIGINAL" in man["source_of_images"], man)

    root, proot = os.path.join(tmp, "store"), os.path.join(tmp, "popstore"); os.makedirs(root)
    legacy = os.path.join(root, "pop.db"); open(legacy, "wb").write(b"SQLite format 3\0" + b"existing schema-1 stand-in" * 40); h0 = sh(legacy)
    cfg = os.path.join(tmp, "cfg.json")
    json.dump({"visual_memory": {"enabled": True, "vector_search_enabled": True, "storage_root": root, "embedding": {"enabled": True},
                                 "pop": {"enabled": True, "storage_root": proot, "events_path": os.path.join(tmp, "ev.jsonl"), "jobs_dir": os.path.join(tmp, "jobs")}}}, open(cfg, "w"))
    open(os.path.join(tmp, "ev.jsonl"), "w").close(); os.makedirs(os.path.join(tmp, "jobs"))
    p = subprocess.Popen([API_BIN, "--config", cfg, "--port", str(PORT), "--provider", "mock:64", "--status-file", os.path.join(tmp, "st.json")], stdout=open(os.path.join(tmp, "api.log"), "w"), stderr=subprocess.STDOUT)
    procs.append(p); time.sleep(2.0)
    api = f"http://127.0.0.1:{PORT}"
    r = subprocess.run([sys.executable, os.path.join(ROOT, "scripts/pop/provision_demo.py"), "apply", "--api", api, "--bundle", bundle, "--store-root", proot], capture_output=True, text=True)
    check("apply succeeds and reports OK counts for both ads", r.returncode == 0 and r.stdout.count("OK") >= 2 and "COUNT MISMATCH" not in r.stdout, r.stdout[-700:] + r.stderr[-200:])
    media = json.load(urllib.request.urlopen(api + "/api/pop/media"))["media"]
    check("the registry the API reports lists Ad A and Ad B with 6 + 6 reference frames and the original hashes",
          sorted(m["media_id"] for m in media) == ["ad-a", "ad-b"] and all(m["reference_frames"] == 6 and m["display_reference_frames"] == 6 and m["sha256"] == sh(vids[m["media_id"]]) for m in media), media)
    check("durations come from the original files", all(abs(m["duration_s"] - 6.0) < 0.2 for m in media), [m["duration_s"] for m in media])
    st = json.load(urllib.request.urlopen(api + "/api/pop/status"))
    check("the demonstration store is a separate schema-2 database", st["store"]["schema_version"] == 2 and os.path.exists(os.path.join(proot, "pop", "pop.db")) or os.path.exists(os.path.join(proot, "pop.db")), st["store"])
    check("the schema-1 pop.db in the Visual Memory root is byte-identical", sh(legacy) == h0)
    r2 = subprocess.run([sys.executable, os.path.join(ROOT, "scripts/pop/provision_demo.py"), "apply", "--api", api, "--bundle", bundle, "--store-root", proot], capture_output=True, text=True)
    check("a second apply is idempotent", r2.returncode == 0, r2.stdout[-500:])
except BaseException:
    traceback.print_exc(); fails.append('test aborted by an exception')
finally:
    for p in procs:
        try: p.send_signal(signal.SIGTERM); p.wait(timeout=5)
        except Exception: p.kill()
    shutil.rmtree(tmp, ignore_errors=True)
    print("FAILED: %s" % fails if fails else "OK: 0 check(s) failed")
    sys.exit(1 if fails else 0)
