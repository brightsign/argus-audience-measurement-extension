#!/usr/bin/env python3
"""End-to-end check of the D4 object endpoints against a running vm_api_server (objects.enabled, embedding on).

  vm_objects_e2e.py http://127.0.0.1:20411

Needs a store with >= 4 frames. It adds object regions to that store's objects.db (use a scratch store).
Exit code 0 = all checks passed.
"""
import json, sys, urllib.request, urllib.error

BASE = sys.argv[1].rstrip("/") if len(sys.argv) > 1 else "http://127.0.0.1:20411"
fails = []


def call(method, path, body=None, raw=None):
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(BASE + path, data=data, method=method,
                                 headers={"Content-Type": "application/json"} if data else {})
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            b = r.read(); ct = r.headers.get("Content-Type", "")
            return r.status, (json.loads(b) if "json" in ct else b), ct
    except urllib.error.HTTPError as e:
        b = e.read(); ct = e.headers.get("Content-Type", "")
        return e.code, (json.loads(b) if "json" in ct else b), ct


def check(name, cond, extra=""):
    print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(extra)) if extra and not cond else ""))
    if not cond:
        fails.append(name)


st, fr, _ = call("GET", "/api/vm/frames?limit=8")
ids = [f["id"] for f in fr["frames"]]
check("store has frames", len(ids) >= 4, ids)
f1, f2, f3, f4 = ids[:4]

st, s, _ = call("GET", "/api/vm/status")
check("status.objects enabled", s["objects"]["enabled"] and s["objects"]["available"], s.get("objects"))
st, r, _ = call("POST", "/api/vm/objects/search", {"frame_id": f1, "x": 40, "y": 40, "w": 160, "h": 120})
check("search on empty index -> ok + empty_index", st == 200 and r["results"] == [] and r["status"].get("empty_index") is True, r)

box = {"frame_id": f1, "x": 40, "y": 40, "w": 160, "h": 120}
st, r, _ = call("POST", "/api/vm/objects", dict(box, label="cup"))
check("index region -> 201", st == 201 and r["status"] == "indexed" and r["region"]["bbox"] == {"x": 40, "y": 40, "w": 160, "h": 120}, r)
rid = r["region"]["region_id"]
st, r, _ = call("POST", "/api/vm/objects", dict(box, label="cup"))
check("duplicate -> 200 duplicate, same id", st == 200 and r["status"] == "duplicate" and r["region"]["region_id"] == rid, r)
for name, b in [("out of bounds", dict(box, x=600)), ("too small", dict(box, w=4, h=4)), ("negative", dict(box, x=-5)),
                ("missing field", {"frame_id": f1, "x": 1}), ("unknown frame", dict(box, frame_id=999999)),
                ("long label", dict(box, label="x" * 100)), ("float coord", dict(box, x=1.5)),
                ("huge", dict(box, w=10 ** 12))]:
    st, r, _ = call("POST", "/api/vm/objects", b)
    check("reject: " + name, st in (400, 404) and "error" in r, (st, r))
st, r, _ = call("POST", "/api/vm/objects", raw=b"not json")
check("reject: non-JSON body", st == 400, (st, r))
st, r, _ = call("POST", "/api/vm/objects", raw=b"x" * 20000)
check("reject: oversized body", st == 400, (st, r))

for i, fid in enumerate((f2, f3, f4)):
    st, r, _ = call("POST", "/api/vm/objects", {"frame_id": fid, "x": 80 + 10 * i, "y": 30, "w": 140, "h": 140, "label": "item%d" % i})
    check("index frame %d region" % fid, st == 201, r)

st, r, _ = call("POST", "/api/vm/objects/search", dict(box, top_k=4))
check("roi search: exact crop is top-1 with similarity ~1",
      st == 200 and r["results"][0]["region_id"] == rid and r["results"][0]["similarity"] > 0.999, r)
check("roi search: sorted by similarity",
      [x["similarity"] for x in r["results"]] == sorted((x["similarity"] for x in r["results"]), reverse=True))
check("roi search: fields", all(k in r["results"][0] for k in ("frame_id", "bbox", "capture_utc_ms", "source_id", "similarity", "image_exists", "label")), r["results"][0])
check("search label disclaimer present", "not identification" in r["label"])
st, r, _ = call("POST", "/api/vm/objects/search", {"region_id": rid, "top_k": 10})
check("region search excludes itself, 3 others", st == 200 and len(r["results"]) == 3 and all(x["region_id"] != rid for x in r["results"]), r)
st, r, _ = call("POST", "/api/vm/objects/search", dict(box, label="cup"))
check("label filter", st == 200 and [x["region_id"] for x in r["results"]] == [rid], r)
st, r, _ = call("POST", "/api/vm/objects/search", dict(box, min_similarity=0.9999999))
check("min_similarity applied", st == 200, r)
for name, b in [("top_k 0", dict(box, top_k=0)), ("top_k 99", dict(box, top_k=99)), ("no query", {}),
                ("bad sim", dict(box, min_similarity=7)), ("bad roi", dict(box, w=-1))]:
    st, r, _ = call("POST", "/api/vm/objects/search", b)
    check("search reject: " + name, st == 400, (st, r))
st, r, _ = call("POST", "/api/vm/objects/search", {"region_id": 99999})
check("search unknown region -> 400", st == 400, (st, r))

st, r, _ = call("GET", "/api/vm/objects?limit=10")
check("list objects", st == 200 and r["total"] == 4 and len(r["objects"]) == 4, r)
st, r, _ = call("GET", "/api/vm/objects?label=cup")
check("list by label", r["total"] == 1)
st, r, _ = call("GET", "/api/vm/objects?limit=0")
check("list bad limit", st == 400)
st, r, _ = call("GET", "/api/vm/objects/%d" % rid)
check("detail", st == 200 and r["region_id"] == rid and r["image_exists"] is True, r)
st, r, _ = call("GET", "/api/vm/objects/987654")
check("detail 404", st == 404)
st, r, _ = call("GET", "/api/vm/objects/abc")
check("detail bad id 400", st == 400)
st, b, ct = call("GET", "/api/vm/objects/%d/image?max=120" % rid)
check("object crop image is JPEG", st == 200 and ct == "image/jpeg" and b[:2] == b"\xff\xd8", (st, ct))
st, b, ct = call("GET", "/api/vm/frames/%d/crop?x=40&y=40&w=160&h=120&max=100" % f1)
check("frame crop preview JPEG", st == 200 and b[:2] == b"\xff\xd8", (st, ct))
st, b, ct = call("GET", "/api/vm/frames/%d/crop?x=900&y=0&w=50&h=50" % f1)
check("frame crop out of bounds 400", st == 400, (st, b))
st, b, ct = call("GET", "/api/vm/frames/%d/crop?x=1&y=1" % f1)
check("frame crop missing params 400", st == 400)
for m, p in [("DELETE", "/api/vm/objects/1"), ("PUT", "/api/vm/objects"), ("GET", "/api/vm/objects/search")]:
    st, r, _ = call(m, p)
    check("method policy %s %s -> 405" % (m, p), st == 405, st)
st, r, _ = call("GET", "/api/vm/objects/%2e%2e%2f%2e%2e%2fsecret")
check("path traversal rejected", st in (400, 404), st)

print("\n%d failure(s)" % len(fails))
sys.exit(1 if fails else 0)
