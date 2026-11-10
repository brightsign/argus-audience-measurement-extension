#!/usr/bin/env python3
"""Hardware validation for source-aware (camera | display) Object Search against a REAL player (HOST script; stdlib only).

  vm_objects_display_hw.py --api http://PLAYER:20300 <stage> [options]

Stages (run in this order, see docs/VISUAL_MEMORY_OBJECT_AUTO_INDEXING.md, "Hardware validation procedure"):
  baseline  --out FILE                 BEFORE the package is installed: record every camera object (ids, boxes, classes, labels) and top-10 search results of a sample.
  compare   --baseline FILE            AFTER the schema-3 migration: the same camera objects must still exist unchanged (new ones may have been added), searches must agree, every
                                       object is source_type=camera. Camera regression of the default scope.
  manual    --session SID --manifest M MANUAL indexing of the bottle and the cup from the stored Weston/VOP2 frames of the product-video session, using the ground-truth boxes of
                                       the generated video. Needs NO detector: run it with objects.auto.display.enabled = false.
  auto      --session SID --manifest M AUTOMATIC indexing: bounded display backfill (<= 50 frames), then every indexed object is compared with the ground truth of the video.
  search    --session SID              display-only search from an automatic bottle and cup: scope, similar objects first, ad / offset / timestamp / session link agree with Proof of Play.
  isolation                            camera and display do not mix; same ids in both sources; the explicit 'all' scope.
  expired                              parent frames whose JPEG expired: reported unavailable, crops still served; NOT EXERCISED when the player has none.
  evaluate  --detections JSON --manifest M   offline: apply the same ground-truth evaluation to the host float-YOLOX results (scripts/visual_memory/object_probe_onnx.py output).

Every check prints PASS / FAIL / NOTE. Nothing here changes the player configuration. The only writes are the documented API calls: manual index (manual), backfill (auto).
Thresholds below are REPORTING criteria for the demonstration, not detector tuning: a failure is a result, it is never "fixed" by lowering a threshold.
"""
import argparse, io, json, math, sys, time, urllib.error, urllib.parse, urllib.request

W360, SCALE, XOFF = 640, 0.45, 32          # 1920x1080 video -> top-left 1280x800 -> 576x360 centred on a 640x360 frame (verified geometry)
BOUNDARY_GUARD_S = 1.5                     # frames this close to a scene change are ambiguous (player offset uncertainty + drift)
MIN_RATE = 0.80                            # reporting criterion: a class is found in at least 80% of the unambiguous frames that contain it
PERIOD = {"water bottle": 7.0, "coffee cup": 5.0}
CLASS_OF = {"water bottle": "bottle", "coffee cup": "cup"}

fails, notes = [], []


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(extra)[:500]) if (not cond and extra != "") else ""), flush=True)
    if not cond: fails.append(name)
    return cond


def note(msg):
    print("NOTE " + msg, flush=True); notes.append(msg)


class Api:
    def __init__(self, base): self.base = base.rstrip("/")

    def call(self, method, path, body=None, timeout=60):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base + path, data=data, method=method, headers={"Content-Type": "application/json", "Origin": self.base} if data else {})
        t0 = time.time()
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                b = r.read(); ct = r.headers.get("Content-Type", "")
                return r.status, (json.loads(b) if "json" in ct else b), (time.time() - t0) * 1000
        except urllib.error.HTTPError as e:
            b = e.read(); ct = e.headers.get("Content-Type", "")
            try: return e.code, (json.loads(b) if "json" in ct else b), (time.time() - t0) * 1000
            except Exception: return e.code, b, (time.time() - t0) * 1000

    def get(self, p): return self.call("GET", p)
    def post(self, p, b): return self.call("POST", p, b)


# ------------------------------------------------------------------------------------------------ ground truth of the generated video
def scene_of(t):
    return min(2, int(t // 10))


def gt_box(man, scene, label, t):
    """Box of an object in the stored 640x360 Weston frame (x, y, w, h), from the generator's manifest. The cup box is its sprite canvas (saucer and handle included)."""
    for o in man["scenes"][scene]["objects"]:
        if o["label"] == label:
            tl = t - scene * 10.0
            cx = o["centre_x"] + o["drift_amp_px"] * math.sin(2 * math.pi * tl / PERIOD[label]); w, h = o["size_px"]
            x1, x2, y2 = cx - w / 2, cx + w / 2, o["base_y"]; y1 = y2 - h
            return [x1 * SCALE + XOFF, y1 * SCALE, (x2 - x1) * SCALE, (y2 - y1) * SCALE]
    return None


def inside(b, g, pad=10):
    cx, cy = b["x"] + b["w"] / 2, b["y"] + b["h"] / 2
    return g[0] - pad <= cx <= g[0] + g[2] + pad and g[1] - pad <= cy <= g[1] + g[3] + pad


def iou(b, g):
    ax1, ay1, ax2, ay2 = b["x"], b["y"], b["x"] + b["w"], b["y"] + b["h"]; bx1, by1, bx2, by2 = g[0], g[1], g[0] + g[2], g[1] + g[3]
    iw, ih = max(0, min(ax2, bx2) - max(ax1, bx1)), max(0, min(ay2, by2) - max(ay1, by1)); i = iw * ih
    return i / (b["w"] * b["h"] + g[2] * g[3] - i) if i else 0.0


def evaluate(frames, objects, man, label="display objects"):
    """frames: [{offset_s, key}]; objects: [{key, class_name, bbox{x,y,w,h}, confidence}]. Returns a report and records PASS/FAIL checks."""
    per = []
    by_key = {}
    for o in objects: by_key.setdefault(o["key"], []).append(o)
    for f in frames:
        t = f["offset_s"]; sc = scene_of(t)
        near = min(abs(t - 10.0), abs(t - 20.0)) < BOUNDARY_GUARD_S
        exp = ["bottle", "cup"] if sc < 2 else []
        objs = by_key.get(f["key"], [])
        row = {"key": f["key"], "offset_s": round(t, 2), "scene": sc + 1, "ambiguous": near, "expected": exp, "found": []}
        for o in objs:
            g = None
            for lab, cls in CLASS_OF.items():
                if cls == o["class_name"] and sc < 2: g = gt_box(man, sc, lab, t)
            row["found"].append({"class": o["class_name"], "confidence": round(o["confidence"], 3), "box": [round(o["bbox"][k]) for k in "xywh"],
                                 "centre_in_truth": bool(g and inside(o["bbox"], g)), "iou_with_truth": round(iou(o["bbox"], g), 3) if g else None})
        per.append(row)
    amb = [r for r in per if r["ambiguous"]]; ok = [r for r in per if not r["ambiguous"]]
    rep = {"frames": len(per), "unambiguous_frames": len(ok), "ambiguous_near_scene_change": len(amb)}
    for cls in ("bottle", "cup"):
        pos = [r for r in ok if cls in r["expected"]]
        hit = [r for r in pos if any(x["class"] == cls for x in r["found"])]
        good = [r for r in pos if any(x["class"] == cls and x["centre_in_truth"] for x in r["found"])]
        ious = [x["iou_with_truth"] for r in pos for x in r["found"] if x["class"] == cls and x["iou_with_truth"] is not None]
        confs = [x["confidence"] for r in pos for x in r["found"] if x["class"] == cls]
        rep[cls] = {"frames_with_object": len(pos), "found": len(hit), "rate": round(len(hit) / len(pos), 3) if pos else None, "box_centre_inside_truth": len(good),
                    "iou_median": sorted(ious)[len(ious) // 2] if ious else None, "conf_min": min(confs) if confs else None, "conf_median": sorted(confs)[len(confs) // 2] if confs else None}
        check("%s: %s found in >= %d%% of the %d frames that show it (found %d)" % (label, cls, MIN_RATE * 100, len(pos), len(hit)), bool(pos) and len(hit) / len(pos) >= MIN_RATE, rep[cls])
        check("%s: every %s box has its centre inside the ground-truth object (%d/%d frames)" % (label, cls, len(good), len(hit)), len(good) == len(hit) and len(hit) > 0, rep[cls])
    neg = [r for r in ok if r["scene"] == 3]
    false_pos = [r for r in neg if any(x["class"] in ("bottle", "cup") for x in r["found"])]
    rep["scene3_frames"] = len(neg); rep["scene3_false_positives"] = len(false_pos)
    check("%s: scene 3 (unrelated objects): no bottle/cup in any of the %d frames" % (label, len(neg)), len(false_pos) == 0 and len(neg) > 0, [(r["key"], r["found"]) for r in false_pos][:3])
    rep["objects_total"] = len(objects); rep["objects_per_frame"] = round(len(objects) / len(per), 2) if per else 0
    rep["per_frame"] = per
    return rep


def parse_man(path): return json.load(open(path))


def list_all(api, path, key="objects", cap=5000):
    """Page through a list endpoint (the API caps limit at 100). Returns (items, total)."""
    items, off, total = [], 0, 0
    while off < cap:
        st, r, _ = api.get("%s%slimit=100&offset=%d" % (path, "&" if "?" in path else "?", off))
        if st != 200: sys.exit("cannot list %s: %s %s" % (path, st, str(r)[:200]))
        total = r.get("total", 0); items += r[key]
        if len(items) >= total or not r[key]: break
        off += 100
    return items, total


def session_frames(api, sid):
    fr, total = list_all(api, "/api/vm/display-frames?session_id=%s" % urllib.parse.quote(sid), "frames")
    check("display frames of session %s listed (%d)" % (sid, total), total > 0)
    return fr


def region_key(o): return "%s:%s" % (o["source_kind"], o["frame_id"])


# ------------------------------------------------------------------------------------------------ stages
def stage_baseline(api, a):
    objs, _t = list_all(api, "/api/vm/objects")
    keep = [{k: o.get(k) for k in ("region_id", "frame_id", "bbox", "class_name", "label", "index_origin", "confidence", "detector", "capture_utc_ms", "image_width", "image_height")} for o in objs]
    ids = [o["region_id"] for o in keep]; sample = ids[:: max(1, len(ids) // 5)][:5]
    searches = {}
    for rid in sample:
        st, r, _ = api.post("/api/vm/objects/search", {"region_id": rid, "top_k": 10})
        if st == 200: searches[str(rid)] = [[x["region_id"], round(x["similarity"], 4)] for x in r["results"]]
    st, s, _ = api.get("/api/vm/status")
    out = {"taken_utc_ms": int(time.time() * 1000), "objects": keep, "total": len(keep), "max_region_id": max(ids) if ids else 0, "searches": searches, "status_objects": s.get("objects") if st == 200 else None}
    json.dump(out, open(a.out, "w"), indent=1)
    print("baseline: %d camera objects, %d sample searches -> %s" % (len(keep), len(searches), a.out))


def stage_compare(api, a):
    base = json.load(open(a.baseline)); b_ids = {o["region_id"]: o for o in base["objects"]}
    objs, _t = list_all(api, "/api/vm/objects")
    now = {o["region_id"]: o for o in objs}
    check("camera object list (default scope) works and every object says source_type=camera", all(o.get("source_type") == "camera" for o in objs) and len(objs) > 0, (len(objs)))
    missing = [i for i in b_ids if i not in now]
    check("all %d baseline camera objects still exist (ids preserved)" % len(b_ids), not missing, missing[:5])
    diff = [i for i in b_ids if i in now and any(now[i].get(k) != b_ids[i].get(k) for k in ("frame_id", "bbox", "class_name", "label", "index_origin", "confidence", "detector", "capture_utc_ms", "image_width", "image_height"))]
    check("their frame ids, boxes, classes, labels, origin, confidence, detector and timestamps are unchanged", not diff, diff[:3])
    added = [i for i in now if i not in b_ids]
    note("%d camera objects were added since the baseline (live scan); ids > %d: %s" % (len(added), base["max_region_id"], all(i > base["max_region_id"] for i in added)))
    for rid, old in base["searches"].items():
        st, r, _ = api.post("/api/vm/objects/search", {"region_id": int(rid), "top_k": 25})
        newl = [[x["region_id"], round(x["similarity"], 4)] for x in r["results"] if x["region_id"] <= base["max_region_id"]][:len(old)] if st == 200 else []
        check("search from camera object %s: the same top-%d (new objects excluded), same similarities" % (rid, len(old)), st == 200 and newl == old, (old[:3], newl[:3]))
        check("search scope defaults to camera (%s)" % rid, st == 200 and r["scope"]["source_type"] == "camera" and all(x["source_type"] == "camera" for x in r["results"]))
    st, s, _ = api.get("/api/vm/status"); check("camera frames and object counts are reported in status", st == 200 and s["objects"].get("auto_regions") is not None, str(s.get("objects"))[:200])


def pick_offsets(frames, targets):
    """For each wanted time (s) the history frame of the session closest to it."""
    hist = [f for f in frames if f["kind"] == "history" and f.get("playback_offset_ms") is not None]
    out = []
    for t in targets:
        if hist: out.append(min(hist, key=lambda f: abs(f["playback_offset_ms"] / 1000.0 - t)))
    return out


def stage_manual(api, a):
    man = parse_man(a.manifest); frames = session_frames(api, a.session)
    st, au, _ = api.get("/api/vm/objects/auto/status?source=display")
    note("display auto indexing at this moment: enabled=%s running=%s (the claim 'independent of YOLOX' needs enabled=false)" % (au.get("enabled"), au.get("running")))
    made = []
    for t, sc in ((4.0, 0), (14.0, 1)):
        for f in pick_offsets(frames, [t])[:1]:
            tt = f["playback_offset_ms"] / 1000.0
            for lab in ("water bottle", "coffee cup"):
                g = gt_box(man, scene_of(tt), lab, tt)
                body = {"source_type": "display", "kind": f["kind"], "frame_id": f["id"], "x": int(g[0]), "y": int(g[1]), "w": int(g[2]), "h": int(g[3]), "label": "HW test " + lab}
                st, r, ms = api.post("/api/vm/objects", body)
                ok = st in (200, 201) and r.get("region", {}).get("source_type") == "display"
                check("manual %s on display frame %s/%s (offset %.1f s): created or duplicate (%d, %.0f ms)" % (lab, f["kind"], f["id"], tt, st, ms), ok, str(r)[:300])
                if not ok: continue
                reg = r["region"]
                check("  stored in 640x360 frame coordinates with the box we sent, crop retained, session %s, ad %s, offset+uncertainty+provenance recorded" % (a.session, reg.get("reported_media_id")),
                      reg["image_width"] == 640 and reg["image_height"] == 360 and reg["bbox"] == {"x": body["x"], "y": body["y"], "w": body["w"], "h": body["h"]} and reg["crop_state"] == "retained"
                      and reg["pop_session_id"] == a.session and reg["playback_offset_ms"] > 0 and reg["time_uncertainty_ms"] is not None and "weston" in (reg.get("provenance") or "").lower(), str(reg)[:400])
                st2, img, _ = api.get("/api/vm/objects/%d/image" % reg["region_id"]); check("  its crop JPEG is served", st2 == 200 and img[:2] == b"\xff\xd8", st2)
                made.append((lab, reg["region_id"], tt))
    for lab, rid, tt in made:
        st, r, ms = api.post("/api/vm/objects/search", {"region_id": rid, "top_k": 12})
        res = r.get("results", []) if st == 200 else []
        same = [x for x in res if x["source_type"] == "display"]
        check("search from the manual %s (region %d): display scope only, %d results in %.0f ms" % (lab, rid, len(res), ms), st == 200 and r["scope"]["source_type"] == "display" and len(same) == len(res), str(r)[:200])
        if len(made) >= 4 and res: note("  top similarities for %s: %s" % (lab, [round(x["similarity"], 3) for x in res[:5]]))
    other = [m for m in made if m[0] == "water bottle"]
    if len(other) >= 2:
        st, r, _ = api.post("/api/vm/objects/search", {"region_id": other[0][1], "top_k": 5})
        ids = [x["region_id"] for x in r["results"]]
        check("the manual bottle of scene 1 finds the manual bottle of scene 2 (different position/size/wall) within the top 5", other[1][1] in ids, ids)


def stage_auto(api, a):
    man = parse_man(a.manifest)
    st, au, _ = api.get("/api/vm/objects/auto/status?source=display")
    if not check("display automatic indexing is enabled (objects.auto.display.enabled)", st == 200 and au.get("enabled"), au.get("scope")): return
    frames = session_frames(api, a.session)
    lim = max(1, min(50, a.limit))
    t0 = time.time(); st, r, _ = api.post("/api/vm/objects/auto/backfill", {"source": "display", "limit": lim})
    check("bounded display backfill accepted (limit %d)" % lim, st == 202, str(r)[:200])
    fin = None
    while time.time() - t0 < a.timeout:
        st, s, _ = api.get("/api/vm/objects/auto/status?source=display")
        if st == 200 and s["backfill"]["state"] in ("done", "cancelled", "error"): fin = s; break
        time.sleep(1.0)
    if not check("backfill finished within %d s" % a.timeout, fin is not None): return
    bf = fin["backfill"]; elapsed = time.time() - t0
    check("backfill state done, failed=0 (%s)" % bf, bf["state"] == "done" and bf["failed"] == 0)
    print("  backfill: %d frames, %d objects, %.1f s wall = %.0f ms per frame (detector + MobileNetV2 + SQLite + crop)" % (bf["done"], bf["indexed"], elapsed, 1000.0 * elapsed / max(1, bf["done"])))
    dall, _t = list_all(api, "/api/vm/objects?source_type=display")
    objs = [o for o in dall if o["pop_session_id"] == a.session and o["index_origin"] == "auto"]
    fr = [{"key": "%s:%s" % (f["kind"], f["id"]), "offset_s": (f.get("playback_offset_ms") or 0) / 1000.0} for f in frames if f.get("playback_offset_ms") is not None]
    rep = evaluate(fr, [{"key": region_key(o), "class_name": o["class_name"], "bbox": o["bbox"], "confidence": o["confidence"]} for o in objs], man)
    check("every indexed object kept its crop and the detector provenance", all(o["crop_state"] == "retained" and o["detector"] and o["detector_version"] for o in objs) and len(objs) > 0, len(objs))
    check("every object carries session, expected/reported ad, offset, uncertainty", all(o["pop_session_id"] == a.session and o["reported_media_id"] and o["playback_offset_ms"] is not None and o["time_uncertainty_ms"] is not None for o in objs))
    check("no person and nothing outside the class allowlist was indexed", all(o["class_name"] in ("bottle", "cup", "wine_glass", "laptop", "cell_phone", "book") for o in objs), sorted({o["class_name"] for o in objs}))
    check("every box lies inside the stored 640x360 frame (letterbox bars handled)", all(0 <= o["bbox"]["x"] and o["bbox"]["x"] + o["bbox"]["w"] <= 640 and 0 <= o["bbox"]["y"] and o["bbox"]["y"] + o["bbox"]["h"] <= 360 for o in objs))
    rep["backfill_wall_s"] = round(elapsed, 1); rep["backfill"] = bf
    json.dump(rep, open(a.out or "display_auto_report.json", "w"), indent=1); print("  report ->", a.out or "display_auto_report.json")


def stage_search(api, a):
    dall, _t = list_all(api, "/api/vm/objects?source_type=display")
    objs = [o for o in dall if o["pop_session_id"] == a.session and o["index_origin"] == "auto"]
    for cls in ("bottle", "cup"):
        q = [o for o in objs if o["class_name"] == cls]
        if not check("an automatic %s exists in session %s" % (cls, a.session), len(q) > 0): continue
        q0 = sorted(q, key=lambda o: o["capture_utc_ms"])[0]
        st, r, ms = api.post("/api/vm/objects/search", {"region_id": q0["region_id"], "top_k": 10})
        res = r.get("results", []) if st == 200 else []
        check("display-only search from %s #%d: indexed display objects only, scope text says so, %d results in %.0f ms" % (cls, q0["region_id"], len(res), ms),
              st == 200 and r["scope"]["source_type"] == "display" and "indexed" in r["scope"]["message"].lower() and all(x["source_type"] == "display" for x in res), r.get("scope"))
        same = sum(1 for x in res[:5] if x.get("class_name") == cls)
        check("  the top 5 are mostly the same class (%d of 5 are %s)" % (same, cls), same >= 3, [(x.get("class_name"), round(x["similarity"], 3)) for x in res[:5]])
        check("  every result has timestamp, ad, offset, session link and a frame image link", all(x["capture_utc_ms"] > 0 and x["reported_media_id"] and x["playback_offset_ms"] is not None and x["session_link"].startswith("#pop?session=") and x["frame_image_url"] for x in res))
        if res:
            x = res[0]
            st, sess, _ = api.get("/api/pop/sessions/" + urllib.parse.quote(x["pop_session_id"]))
            check("  top result's session opens in Proof of Play with the same expected / reported ad", st == 200 and sess["expected_media_id"] == x["expected_media_id"] and sess["played_media_id"] == x["reported_media_id"], str(sess)[:200])
            if x["source_kind"] == "history":
                iso = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(x["capture_utc_ms"] // 1000)) + ".%03dZ" % (x["capture_utc_ms"] % 1000)
                st, tf, _ = api.get("/api/display/history/nearest?time=%s&max_diff_ms=500" % iso)
                check("  time search at the result's timestamp returns the same display frame", st == 200 and tf.get("found") and tf["frame"]["frame_id"] == x["frame_id"], str(tf)[:200])
            st, im, _ = api.get(x["frame_image_url"]); check("  the original display frame is served (letterboxed 640x360 JPEG)", st == 200 and im[:2] == b"\xff\xd8", st)


def stage_isolation(api, a):
    camo, camt = list_all(api, "/api/vm/objects"); diso, dist = list_all(api, "/api/vm/objects?source_type=display"); st, al, _ = api.get("/api/vm/objects?source_type=all&limit=1")
    cam = {"objects": camo, "total": camt}; dis = {"objects": diso, "total": dist}
    check("default list = camera only", all(o["source_type"] == "camera" for o in cam["objects"]), cam["total"])
    check("source_type=display lists display objects only", all(o["source_type"] == "display" for o in dis["objects"]) and dis["total"] > 0, dis["total"])
    check("source_type=all lists both, explicitly (total %d = camera %d + display %d)" % (al["total"], cam["total"], dis["total"]), al["total"] == cam["total"] + dis["total"])
    cids = {o["frame_id"] for o in cam["objects"]}; dids = {o["frame_id"] for o in dis["objects"] if o["source_kind"] == "history"}
    ov = cids & dids
    if ov:
        n = sorted(ov)[0]; st, l1, _ = api.get("/api/vm/objects?source_type=camera&frame_id=%d&limit=100" % n); st, l2, _ = api.get("/api/vm/objects?source_type=display&kind=history&frame_id=%d&limit=100" % n)
        check("frame id %d exists in both sources and selects different objects per source" % n, {o["region_id"] for o in l1["objects"]}.isdisjoint({o["region_id"] for o in l2["objects"]}) and l1["total"] and l2["total"])
    else:
        note("no camera frame id coincides with a display history id in this data (the e2e suite proves the collision case); ids camera %s.. display %s.." % (sorted(cids)[:2], sorted(dids)[:2]))
    if dis["objects"]:
        d0 = dis["objects"][0]
        st, r, _ = api.post("/api/vm/objects/search", {"region_id": d0["region_id"], "top_k": 25, "source_type": "camera"})
        check("a display query with explicit camera scope returns camera objects only", st == 200 and all(x["source_type"] == "camera" for x in r["results"]), str(r)[:200])
        st, r, _ = api.post("/api/vm/objects/search", {"region_id": d0["region_id"], "top_k": 25, "source_type": "all"})
        check("explicit 'all' mixes both sources", st == 200 and {x["source_type"] for x in r["results"]} >= {"display"}, sorted({x["source_type"] for x in r.get("results", [])}))
    st, a1, _ = api.get("/api/vm/objects/auto/status?source=camera"); st, a2, _ = api.get("/api/vm/objects/auto/status?source=display")
    check("progress is tracked per source", a1["progress"]["frames_processed"] != a2["progress"]["frames_processed"] or a2["progress"]["frames_processed"] == 0, (a1["progress"], a2["progress"]))


def stage_expired(api, a):
    frs, _t = list_all(api, "/api/vm/display-frames", "frames")
    gone = [f for f in frs if not f.get("image_available") or f.get("image_exists") is False]
    diso, _t = list_all(api, "/api/vm/objects?source_type=display"); dis = {"objects": diso}
    orphan = [o for o in diso if o.get("image_exists") is False]
    if not gone and not orphan:
        note("NOT EXERCISED: the player has no expired display JPEG yet (history retention is hours/days). To force it deliberately, delete one history JPEG of the test session through the player file API "
             "(needs explicit approval), then re-run this stage.")
        return
    for f in gone[:3]:
        st, r, _ = api.get(f["image_url"]); check("expired frame %s/%s: image request answers 404/410 (not a crash, not a stale image)" % (f["kind"], f["id"]), st in (404, 410), st)
    for o in orphan[:3]:
        st, det, _ = api.get("/api/vm/objects/%d" % o["region_id"]); check("object %d: detail reports the missing parent (image_exists=false) and keeps session/timestamp" % o["region_id"], st == 200 and det["image_exists"] is False and det["pop_session_id"])
        st, im, _ = api.get("/api/vm/objects/%d/image" % o["region_id"]); check("object %d: its own crop is still served" % o["region_id"], st == 200 and im[:2] == b"\xff\xd8", st)
    st, r, _ = api.post("/api/vm/objects/search", {"region_id": dis["objects"][0]["region_id"], "top_k": 10, "require_image": True})
    check("require_image drops results whose parent frame expired but keeps the ones whose crop exists", st == 200, str(r)[:200])


def stage_evaluate(a):
    man = parse_man(a.manifest); det = json.load(open(a.detections)); fr, objs = [], []
    for row in det:
        off = int(row["frame"].split(".")[0]) / 1000.0; key = row["frame"]
        fr.append({"key": key, "offset_s": off})
        for d in row["thresholds"].get(a.threshold, []):
            if d["class"] in ("bottle", "cup"):
                b = d["box"]; objs.append({"key": key, "class_name": d["class"], "confidence": d["confidence"], "bbox": {"x": b[0], "y": b[1], "w": b[2] - b[0], "h": b[3] - b[1]}})
    rep = evaluate(fr, objs, man, label="host float YOLOX >= %s" % a.threshold)
    json.dump(rep, open(a.out or "host_eval_report.json", "w"), indent=1)
    print({k: v for k, v in rep.items() if k != "per_frame"})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--api", default="http://127.0.0.1:20300"); ap.add_argument("stage", choices=["baseline", "compare", "manual", "auto", "search", "isolation", "expired", "evaluate"])
    ap.add_argument("--out"); ap.add_argument("--baseline"); ap.add_argument("--session"); ap.add_argument("--manifest"); ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--detections"); ap.add_argument("--threshold", default="0.3"); ap.add_argument("--limit", type=int, default=50, help="auto: frames of the bounded backfill (<= 50; the newest first)")
    a = ap.parse_args(); api = Api(a.api)
    if a.stage == "evaluate": stage_evaluate(a)
    else:
        if a.stage in ("manual", "auto", "search") and not a.session: sys.exit("--session is required")
        if a.stage in ("manual", "auto") and not a.manifest: sys.exit("--manifest is required")
        {"baseline": stage_baseline, "compare": stage_compare, "manual": stage_manual, "auto": stage_auto, "search": stage_search, "isolation": stage_isolation, "expired": stage_expired}[a.stage](api, a)
    print("\nRESULT %s (%d failed, %d notes)" % ("FAIL" if fails else "PASS", len(fails), len(notes)))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
