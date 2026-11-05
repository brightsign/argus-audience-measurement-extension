#!/usr/bin/env python3
"""Provision the Proof of Play demonstration: register advertisements and build their reference frames (HOST tool; ffmpeg, ffprobe).

  provision_demo.py prepare --out BUNDLE --ad ad-a="Meet BrightSign"=/path/meet-brightsign.mp4 --ad ad-b="Snowy River"=/path/snowy_river.mp4 [--slot 30] [--interval 1]
  provision_demo.py apply   --api http://PLAYER:20300 --bundle BUNDLE [--store-root DIR]

prepare  (offline, no network) generates, from the ORIGINAL video files only, two reference sets per advertisement:
           <id>/                       "screenshot" profile  (640x400 full frame; the D5 baseline, used by the brightscript_screenshot backend)
           <id>.display_letterbox/     Weston/VOP2 profile   (640x360, picture 576x360 centred; geometry verified on real XT5 frames)
         plus <id>.manifest.json (media SHA-256, ffprobe data, exact filters, embedding model version) and register.json (the media rows).
         Captured evidence images are never used as references.
apply    registers every advertisement through the Proof of Play API (POST /api/pop/media) and builds the references.
         The reference JPEGs must be in <storage_root>/pop/refs_incoming/ of the ISOLATED test store: with --store-root they are copied there
         (a host store or a mounted card); without it the script only verifies that the references exist after you copied the bundle's
         refs_incoming/ yourself (for example through the DWS file API; credentials are never handled here).
         It then prints the registry as the API reports it and fails if a reference count differs from the bundle.
Nothing here touches the schema-1 pop.db: it talks to whatever store the API serves, which for the demonstration is the isolated schema-2 store.
"""
import argparse, hashlib, json, os, shutil, subprocess, sys, urllib.error, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def probe(p):
    j = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                                            "stream=codec_name,width,height,r_frame_rate:format=duration", "-of", "json", p]))
    s = j["streams"][0]; n, d = s["r_frame_rate"].split("/")
    return {"codec": s["codec_name"], "width": s["width"], "height": s["height"], "fps": round(float(n) / float(d), 3), "duration_s": round(float(j["format"]["duration"]), 3)}


def count_jpgs(d):
    return len([f for f in os.listdir(d) if f.endswith(".jpg")]) if os.path.isdir(d) else 0


def cmd_prepare(a):
    os.makedirs(a.out, exist_ok=True)
    refs = os.path.join(a.out, "refs_incoming"); os.makedirs(refs, exist_ok=True)
    reg, summary = [], []
    for spec in a.ad:
        try:
            mid, name, path = spec.split("=", 2)
        except ValueError:
            sys.exit("--ad must be ID=NAME=PATH")
        if not os.path.isfile(path):
            sys.exit("not a file: " + path)
        pr = probe(path)
        # the two reference profiles, both from the original video
        subprocess.run(["bash", os.path.join(HERE, "make_reference_frames.sh"), path, os.path.join(refs, mid), str(a.interval), "0", "screenshot"], check=True, stdout=subprocess.DEVNULL)
        subprocess.run([sys.executable, os.path.join(HERE, "pop_reference_tools.py"), "make-refs", "--video", path, "--media-id", mid, "--out", refs, "--interval", str(a.interval)],
                       check=True, stdout=subprocess.DEVNULL)
        row = {"media_id": mid, "display_name": name, "kind": "file", "uri": os.path.basename(path), "sha256": sha256(path), "codec": pr["codec"],
               "width": pr["width"], "height": pr["height"], "fps": pr["fps"], "duration_s": pr["duration_s"], "slot_s": a.slot}
        reg.append(row)
        summary.append({"media_id": mid, "screenshot_refs": count_jpgs(os.path.join(refs, mid)), "display_refs": count_jpgs(os.path.join(refs, mid + ".display_letterbox")),
                        "sha256": row["sha256"], "duration_s": pr["duration_s"], "source_file": os.path.basename(path)})
    json.dump(reg, open(os.path.join(a.out, "register.json"), "w"), indent=1)
    json.dump(summary, open(os.path.join(a.out, "bundle_summary.json"), "w"), indent=1)
    print(json.dumps(summary, indent=1))
    print("Bundle ready: %s\n  copy %s/ to <storage_root>/pop/refs_incoming/ of the isolated store, then run: provision_demo.py apply --api ... --bundle %s" % (a.out, refs, a.out))


def call(api, method, path, body=None):
    req = urllib.request.Request(api.rstrip("/") + path, method=method, data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json", "Origin": api.rstrip("/")})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read())
        except Exception:
            return e.code, {}


def cmd_apply(a):
    reg = json.load(open(os.path.join(a.bundle, "register.json")))
    summ = {s["media_id"]: s for s in json.load(open(os.path.join(a.bundle, "bundle_summary.json")))}
    st, status = call(a.api, "GET", "/api/pop/status")
    if st != 200 or not status.get("enabled"):
        sys.exit("Proof of Play is not enabled on %s (visual_memory.pop.enabled)" % a.api)
    schema = (status.get("store") or {}).get("schema_version")
    print("store schema version:", schema, "| capture_backend:", status.get("capture_backend"), "| allow_test_jobs:", status.get("allow_test_jobs"))
    if a.store_root:
        dst = os.path.join(a.store_root, "pop", "refs_incoming"); os.makedirs(dst, exist_ok=True)
        for mid in summ:
            for suffix in ("", ".display_letterbox"):
                src = os.path.join(a.bundle, "refs_incoming", mid + suffix)
                if os.path.isdir(src):
                    shutil.copytree(src, os.path.join(dst, mid + suffix), dirs_exist_ok=True)
    ok = True
    for row in reg:
        st, r = call(a.api, "POST", "/api/pop/media", row)
        if st not in (200, 201):
            print("register %s: HTTP %s %s" % (row["media_id"], st, r)); ok = False; continue
        for profile, mid in (("screenshot", row["media_id"]), ("display_letterbox", row["media_id"])):
            st, r = call(a.api, "POST", "/api/pop/media/%s/references/build?replace=1&profile=%s" % (mid, profile))
            print("build %-18s %-10s -> HTTP %s added %s failed %s" % (profile, mid, st, r.get("added"), r.get("failed")))
            if st != 200 or r.get("failed"):
                ok = False
    st, m = call(a.api, "GET", "/api/pop/media")
    print("\nRegistry as reported by the API:")
    for x in m.get("media", []):
        want = summ.get(x["media_id"], {})
        good = x["reference_frames"] == want.get("screenshot_refs") and x.get("display_reference_frames") == want.get("display_refs")
        ok = ok and (good or x["media_id"] not in summ)
        print("  %-8s %-18s duration %-7s sha256 %s…  screenshot refs %s  display refs %s  %s" % (x["media_id"], x["display_name"], x["duration_s"], (x["sha256"] or "")[:12],
              x["reference_frames"], x.get("display_reference_frames"), "OK" if good else "COUNT MISMATCH vs bundle"))
    sys.exit(0 if ok else 1)


ap = argparse.ArgumentParser()
sp = ap.add_subparsers(dest="cmd", required=True)
p = sp.add_parser("prepare"); p.add_argument("--out", required=True); p.add_argument("--ad", action="append", required=True); p.add_argument("--slot", type=float, default=30.0)
p.add_argument("--interval", type=float, default=1.0)
q = sp.add_parser("apply"); q.add_argument("--api", required=True); q.add_argument("--bundle", required=True); q.add_argument("--store-root")
a = ap.parse_args()
{"prepare": cmd_prepare, "apply": cmd_apply}[a.cmd](a)
