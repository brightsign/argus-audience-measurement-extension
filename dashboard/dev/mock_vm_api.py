#!/usr/bin/env python3
"""MOCK Visual Memory API for LOCAL UI TESTING ONLY. Synthetic data; implements docs/VISUAL_MEMORY_DASHBOARD_API.md.
Never deploy. /api/vm/status errors[] always says MOCK.
  python3 dev/mock_vm_api.py [--port 20310]
Test hook (not part of the contract): add ?mock_status=429|504|503|400 to a search POST to force that HTTP status."""
import argparse, hashlib, io, json, random, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

try:
    from PIL import Image, ImageDraw
except ImportError:
    Image = None

NOW = int(time.time() * 1000)
N = 119
SRC = ["/dev/video1", "/dev/video2"]
FRAMES = []
for i in range(N):
    fid = N - i  # id 119 newest
    FRAMES.append(dict(id=fid, capture_utc_ms=NOW - i * 60_000 - 5000, source_id=SRC[fid % 2], width=480, height=360,
                       jpeg_bytes=20000 + fid * 37,
                       # oldest 30 frames: JPEG expired (410); frame 100: file missing (404)
                       image_available=fid > 30, image_exists=(fid > 30 and fid != 100), has_embedding=fid % 17 != 0))
BY_ID = {f["id"]: f for f in FRAMES}



def jpeg(fid, w, h):
    if Image is None:
        raise SystemExit("PIL (Pillow) is required for the mock's synthetic JPEGs")
    r = random.Random(fid)
    im = Image.new("RGB", (w, h), (r.randint(30, 200), r.randint(30, 200), r.randint(30, 200)))
    d = ImageDraw.Draw(im)
    for _ in range(8):
        x, y = r.randint(0, w), r.randint(0, h)
        d.ellipse([x, y, x + r.randint(20, w // 2), y + r.randint(20, h // 2)], fill=tuple(r.randint(0, 255) for _ in range(3)))
    d.rectangle([0, h - 28, w, h], fill=(0, 0, 0))
    d.text((8, h - 22), "MOCK synthetic frame #%d" % fid, fill=(255, 255, 255))
    b = io.BytesIO()
    im.save(b, "JPEG", quality=80)
    return b.getvalue()


def status():
    stored = sum(1 for f in FRAMES if f["image_available"])
    return {
        "server_time_utc_ms": int(time.time() * 1000), "configured": True, "config_path": "/MOCK/argus-config.json",
        "vector_search_configured": True, "embedding_configured": True,
        "runtime": {"running": True, "snapshot_age_ms": 2100, "stale": False,
                    "stage1": {"samples_seen": 1234, "accepted": 119, "encoded": 119, "dropped_queue_full": 0, "dropped_invalid": 0,
                               "jpeg_write_failures": 0, "database_failures": 0, "retention_deletions": 30, "queue_depth": 0,
                               "max_queue_depth": 3, "bytes_on_disk": 2400000},
                    "stage2": {"status": "active", "backend": "sqlite-vec v0.1.6", "embedded": 100, "vectors_stored": 100,
                               "vector_bytes": 512000, "queue_depth": 1, "max_queue_depth": 2, "dropped_queue_full": 0,
                               "inference_failures": 0, "invalid_vectors": 0, "decode_failures": 0, "permanent_failures": 0,
                               "db_failures": 0}},
        "model": {"name": "mobilenet_v2_embedder", "version": "tv-imagenet1k-v1", "preprocessing": "pp1", "dimension": 1280,
                  "sha256": hashlib.sha256(b"mock").hexdigest(), "dtype": "float32", "metric": "cosine"},
        "store": {"available": True, "schema_version": 2, "vec_backend": "sqlite-vec v0.1.6", "frames": N, "frames_with_image": stored,
                  "embeddings_ok": sum(1 for f in FRAMES if f["has_embedding"]), "embeddings_failed": 0, "jpeg_bytes": 2400000,
                  "vector_bytes": 512000, "db_bytes": 300000, "quota_bytes": 4000000000, "retention_hours": 48,
                  "embedding_retention_days": 7, "oldest_utc_ms": FRAMES[-1]["capture_utc_ms"], "newest_utc_ms": FRAMES[0]["capture_utc_ms"],
                  "sources": SRC, "disk_free_bytes": 5000000000},
        "sampling": {"interval_ms": 5000, "stored_width": 640, "stored_height": 360, "jpeg_quality": 75},
        "capture": {"fps": 29.0, "dropped_frames": 0, "width": 640, "height": 480, "source": "argus metrics"},
        "search": {"available": True, "reason": "", "model_loaded": True, "busy": False},
        "errors": ["MOCK: synthetic data from dashboard/dev/mock_vm_api.py, not a real Visual Memory store"],
    }


def detail(f):
    d = dict(f)
    d.update(created_utc_ms=f["capture_utc_ms"] + 900, source_type="usb", device_id="MOCK-DEVICE", sequence=f["id"] * 5,
             sha256=hashlib.sha256(str(f["id"]).encode()).hexdigest())
    d.pop("has_embedding")
    emb = {"model_name": "mobilenet_v2_embedder", "model_version": "tv-imagenet1k-v1", "preprocessing": "pp1", "dimension": 1280,
           "created_utc_ms": f["capture_utc_ms"] + 2000}
    d["embeddings"] = [dict(emb, status="ok", error="")] if f["has_embedding"] else [dict(emb, status="failed", error="MOCK: synthetic failure")]
    d["retention"] = {"jpeg_expires_utc_ms": f["capture_utc_ms"] + 48 * 3600000, "embedding_expires_utc_ms": f["capture_utc_ms"] + 7 * 86400000}
    return d


def search(excl, top_k, q):
    cands = [f for f in FRAMES if f["has_embedding"] and f["id"] != excl]
    if q.get("source"):
        cands = [f for f in cands if f["source_id"] == q["source"][0]]
    if q.get("start"):
        cands = [f for f in cands if f["capture_utc_ms"] >= int(q["start"][0])]
    if q.get("end"):
        cands = [f for f in cands if f["capture_utc_ms"] < int(q["end"][0])]
    if q.get("require_image") == ["1"]:
        cands = [f for f in cands if f["image_available"] and f["image_exists"]]
    r = random.Random(excl or 7)
    scored = sorted(((round(r.uniform(0.5, 0.99), 4), f) for f in cands), key=lambda x: -x[0])
    if q.get("min_similarity"):
        scored = [s for s in scored if s[0] >= float(q["min_similarity"][0])]
    res = [{"rank": i + 1, "frame_id": f["id"], "similarity": s, "distance": round(1 - s, 4), "capture_utc_ms": f["capture_utc_ms"],
            "source_id": f["source_id"], "width": f["width"], "height": f["height"], "image_available": f["image_available"],
            "image_exists": f["image_exists"]} for i, (s, f) in enumerate(scored[:top_k])]
    return {"label": "Image/scene similarity (whole-frame MobileNetV2 embedding). Not object detection, not identity matching.",
            "query": {"type": "frame" if excl else "upload", "frame_id": excl, "embedded_with": "stored vector" if excl else "rknn"},
            "model": {"name": "mobilenet_v2_embedder", "version": "tv-imagenet1k-v1", "preprocessing": "pp1", "dimension": 1280},
            "status": {"code": "ok", "message": "", "elapsed_ms": 41.2, "candidates_scanned": len(cands)}, "results": res}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, code, body, ctype="application/json", cache="no-store"):
        if not isinstance(body, (bytes, bytearray)):
            body = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", cache)
        self.end_headers()
        self.wfile.write(body)

    def err(self, code, e, m=""):
        self.send(code, {"error": e, "message": m})

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        p = u.path.rstrip("/")
        if p == "/api/vm/status":
            return self.send(200, status())
        if p == "/api/vm/frames":
            fs = list(FRAMES)
            if q.get("start"):
                fs = [f for f in fs if f["capture_utc_ms"] >= int(q["start"][0])]
            if q.get("end"):
                fs = [f for f in fs if f["capture_utc_ms"] < int(q["end"][0])]
            if q.get("source"):
                fs = [f for f in fs if f["source_id"] == q["source"][0]]
            if q.get("order") == ["asc"]:
                fs.reverse()
            lim = max(1, min(100, int(q.get("limit", ["24"])[0])))
            off = int(q.get("offset", ["0"])[0])
            return self.send(200, {"total": len(fs), "limit": lim, "offset": off, "frames": fs[off:off + lim]})
        parts = p.split("/")
        if len(parts) >= 5 and parts[3] == "frames" and parts[4].isdigit():
            f = BY_ID.get(int(parts[4]))
            if not f:
                return self.err(404, "not_found")
            if len(parts) == 5:
                return self.send(200, detail(f))
            if parts[5] in ("image", "thumb"):
                if not f["image_available"]:
                    return self.err(410, "image_expired")
                if not f["image_exists"]:
                    return self.err(404, "image_missing")
                w = int(q.get("w", ["480"])[0]) if parts[5] == "thumb" else 480
                return self.send(200, jpeg(f["id"], w, w * 3 // 4), "image/jpeg", "private, max-age=300")
        self.err(404, "not_found")

    def do_POST(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        n = int(self.headers.get("Content-Length") or 0)
        self.rfile.read(n)
        if q.get("mock_status"):
            c = int(q["mock_status"][0])
            codes = {429: "busy", 504: "timeout", 503: "not_enabled", 400: "invalid_argument"}
            return self.send(c, {"label": "", "status": {"code": codes.get(c, "database_error"), "message": "MOCK forced status",
                                                         "elapsed_ms": 1, "candidates_scanned": 0}, "results": []})
        k = max(1, min(50, int(q.get("top_k", ["5"])[0])))
        time.sleep(0.4)
        if u.path == "/api/vm/search/image":
            return self.send(200, search(None, k, q))
        if u.path == "/api/vm/search/frame":
            fid = int(q.get("frame_id", ["0"])[0])
            if fid not in BY_ID:
                return self.err(404, "not_found")
            return self.send(200, search(fid, k, q))
        self.err(404, "not_found")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=20310)
    a = ap.parse_args()
    print("MOCK Visual Memory API on 127.0.0.1:%d (synthetic data, UI testing only)" % a.port, flush=True)
    ThreadingHTTPServer(("127.0.0.1", a.port), H).serve_forever()
