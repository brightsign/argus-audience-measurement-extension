#!/usr/bin/env python3
"""Real-browser (Chrome CDP) test of Visual Search with the source "BrightSign Display (Weston/VOP2)".

  display_search_cdp.py VM_API_SERVER DASHBOARD_SERVER ARGUS_DISPLAY_BRIDGE FAKE_RENDERED_OUTPUT_SERVER OUTDIR

Real processes: the OS-service test double (real gRPC), argus_display_bridge, vm_api_server (resilience_grpc backend, isolated schema-2 store), the
Go dashboard server and headless Chrome. Two real Proof-of-Play sessions are run (ad A correct, ad B correct) through the player emulator, so the
display evidence that is searched was captured by the bridge from gRPC frames and persisted by D5. Nothing in a search result is pre-set.
Synthetic pictures only.
"""
import asyncio, base64, io, json, os, re, shutil, signal, subprocess, sys, tempfile, threading, time, traceback, urllib.request, urllib.error, websockets
from PIL import Image, ImageDraw

API_BIN, DASH_BIN, BRIDGE_BIN, FAKE_BIN, OUT = sys.argv[1:6]
os.makedirs(OUT, exist_ok=True)
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
API_PORT, DASH_PORT, CDP_PORT = 20593, 20592, 9494
BASE = f"http://127.0.0.1:{DASH_PORT}/"
SLOT = 9
tmp = tempfile.mkdtemp(prefix="dsearch_")
results, logs, procs = [], [], []
dsock, bsock = os.path.join(tmp, "d.sock"), os.path.join(tmp, "b.sock")
sd, root, proot = os.path.join(tmp, "sd"), os.path.join(tmp, "store"), os.path.join(tmp, "popstore")
events = os.path.join(sd, "pop", "events.jsonl"); jobs = os.path.join(sd, "pop", "jobs")


def check(name, cond, extra=""):
    results.append(bool(cond)); print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(extra)[:400]) if not cond and extra != "" else ""), flush=True)


def start(cmd, log):
    p = subprocess.Popen(cmd, stdout=open(os.path.join(tmp, log), "w"), stderr=subprocess.STDOUT); procs.append(p); return p


def stop(p):
    try: p.send_signal(signal.SIGTERM); p.wait(timeout=5)
    except Exception:
        try: p.kill()
        except Exception: pass


def ctl(**kw):
    c = {"frame": "IDLE", "pause": False, "black": False, "frozen": False, "unavailable": False, "clock_skew_ms": 0, "provider": 7}; c.update(kw)
    p = os.path.join(tmp, "control.json"); json.dump(c, open(p + ".tmp", "w")); os.replace(p + ".tmp", p)


def picture(kind, w, h, shift=0):
    im = Image.new("RGB", (w, h)); px = im.load()
    for y in range(h):
        for x in range(w):
            xn, yn = (x + shift) / w, y / h
            if kind == "A": px[x, y] = (int(40 + 200 * xn), int(220 - 150 * yn), 90 + int(60 * abs(xn - 0.5)))
            elif kind == "B": px[x, y] = (200 if (int(xn * 8) % 2) else 30, 40 + int(180 * abs(yn - 0.5) * 2), 220 if (int(yn * 6) % 2) else 60)
            else: px[x, y] = (int(255 * ((x // 40 + y // 40) % 2)), 20, int(255 * (1 - (x // 40 + y // 40) % 2)))      # "C": never on the display
    d = ImageDraw.Draw(im)
    if kind == "A": d.ellipse([w * 0.3, h * 0.2, w * 0.7, h * 0.8], outline=(255, 255, 255), width=4)
    elif kind == "B": d.rectangle([w * 0.2, h * 0.3, w * 0.8, h * 0.7], fill=(250, 250, 40))
    return im


def display_picture(kind, shift=0):
    c = Image.new("RGB", (640, 360), (0, 0, 0)); c.paste(picture(kind, 576, 360, shift=shift), (32, 0)); return c


def jpeg_bytes(im, q=90):
    b = io.BytesIO(); im.save(b, "JPEG", quality=q); return b.getvalue()


def http(method, path, body=None, ctype="application/json", port=API_PORT, origin=True):
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
    def __init__(self):
        super().__init__(daemon=True); self.seen, self.stop_flag, self.seq, self.t0 = set(), False, 0, time.time()

    def emit(self, ev, **kw):
        self.seq += 1
        with open(events, "a") as f: f.write(json.dumps({"ev": ev, "seq": self.seq, "mono_ms": int((time.time() - self.t0) * 1000), **kw}) + "\n")

    def run(self):
        self.emit("boot", mode="jobs"); self.emit("idle_started", ok=True)
        while not self.stop_flag:
            names = sorted(n for n in os.listdir(jobs) if n.endswith(".json") and n not in self.seen) if os.path.isdir(jobs) else []
            if not names: time.sleep(0.2); continue
            n = names[0]; self.seen.add(n); job = json.load(open(os.path.join(jobs, n)))
            sid, mid, exp = job["session_id"], job["media_id"], job["expected_media_id"]
            self.emit("session_scheduled_run", session=sid, job_id=job["job_id"], media_id=mid, expected_media_id=exp, capture_count=0)
            self.emit("play_requested", session=sid, media_id=mid, expected_media_id=exp, ok=True)
            time.sleep(0.15); ctl(frame=mid[-1].upper())
            self.emit("play_started", session=sid, media_id=mid, expected_media_id=exp, vcode=3, latency_ms=200)
            t = time.time()
            while (time.time() - t) * 1000 < job["max_play_ms"] and not self.stop_flag: time.sleep(0.05)
            self.emit("play_stopped", session=sid, media_id=mid, reason="slot_end")
            self.emit("session_end", session=sid, media_id=mid, expected_media_id=exp, reason="slot_end", actual_play_ms=int((time.time() - t) * 1000))
            ctl(frame="IDLE"); self.emit("idle_started", ok=True)


def run_session(player, media):
    st, r = http("POST", "/api/pop/sessions", {"expected_media_id": media, "play_media_id": media, "capture_offsets_s": [2, 4.5, 7], "capture_backend": "resilience_grpc"})
    if st != 201: return None
    sid = r["session_id"]; t0 = time.time()
    while time.time() - t0 < SLOT + 40:
        s = http("GET", "/api/pop/sessions/" + sid)[1]
        if s.get("verification"): return s
        time.sleep(1)
    return None


async def main():
    os.makedirs(os.path.join(tmp, "frames")); os.makedirs(jobs); os.makedirs(root); open(events, "w").close()
    for k in ("A", "B"): display_picture(k).save(os.path.join(tmp, "frames", k + ".jpg"), quality=88)
    Image.new("RGB", (640, 360), (70, 70, 70)).save(os.path.join(tmp, "frames", "IDLE.jpg"), quality=88)
    ctl()
    c = {"visual_memory": {"enabled": True, "vector_search_enabled": True, "storage_root": root, "embedding": {"enabled": True},
                           "pop": {"enabled": True, "storage_root": proot, "events_path": events, "jobs_dir": jobs, "poll_ms": 50, "sd_path_prefix_from": "SD:/",
                                   "sd_path_prefix_to": sd + "/", "allow_test_jobs": True, "capture_backend": "resilience_grpc"},
                           "display": {"enabled": True, "bridge_socket": bsock, "poll_ms": 100, "max_age_ms": 1200, "bridge_binary": BRIDGE_BIN,
                                       "os_socket": "unix://" + dsock, "interest_file": os.path.join(tmp, "interest")}}}
    cfg = os.path.join(tmp, "cfg.json"); json.dump(c, open(cfg, "w"))
    start([FAKE_BIN, "--socket", dsock, "--frames-dir", os.path.join(tmp, "frames"), "--interest-file", os.path.join(tmp, "interest"), "--control-file", os.path.join(tmp, "control.json"),
           "--stats-file", os.path.join(tmp, "stats.json")], "fake.log")
    time.sleep(1.0)
    start([API_BIN, "--config", cfg, "--port", str(API_PORT), "--provider", "mock:64", "--status-file", os.path.join(tmp, "st.json")], "api.log")
    start([DASH_BIN, "-port", str(DASH_PORT), "-dir", os.path.join(ROOT_DIR, "dashboard"), "-vm-upstream", f"127.0.0.1:{API_PORT}"], "dash.log")
    time.sleep(2.0)

    # ---- before any evidence exists: the API reports coverage zero and a search says so (no error)
    st, cov = http("GET", "/api/pop/search/display")
    check("coverage before any session: zero searchable rows, schema 2, default minimum = the PoP match threshold", st == 200 and cov["searchable_rows"] == 0 and cov["schema_v2"] is True and abs(cov["min_similarity_default"] - 0.8) < 1e-6, cov)
    st, r = http("POST", "/api/pop/search/display", jpeg_bytes(display_picture("A")), "image/jpeg")
    check("search with no stored evidence: 200, 'No matching stored evidence', no closest frame, and no claim that nothing was played",
          st == 200 and r["no_match"] and r["message"] == "No matching stored evidence" and "closest_below_minimum" not in r and "does not show that the content was never played" in r["message_detail"], (st, r))

    for mid, name in (("ad-a", "Meet BrightSign"), ("ad-b", "Snowy River")):
        st, r = http("POST", "/api/pop/media", {"media_id": mid, "display_name": name, "uri": mid + ".mp4", "duration_s": 60, "slot_s": SLOT, "width": 1920, "height": 1080, "sha256": "0" * 64})
        check(f"register {mid}", st == 201, r); kind = mid[-1].upper()
        for prof, mk in (("screenshot", lambda i: picture(kind, 640, 400, shift=i * 4)), ("display_letterbox", lambda i: display_picture(kind, shift=i * 4))):
            d = os.path.join(proot, "pop", "refs_incoming", mid if prof == "screenshot" else mid + ".display_letterbox"); os.makedirs(d)
            for i in range(8): mk(i).save(os.path.join(d, f"{i * 2000}.jpg"), quality=90)
            st, r = http("POST", f"/api/pop/media/{mid}/references/build?profile={prof}")
            check(f"build {prof} references for {mid}", st == 200 and r["added"] == 8, r)
    player = Player(); player.start()
    sa = await asyncio.to_thread(run_session, player, "ad-a"); sb = await asyncio.to_thread(run_session, player, "ad-b")
    check("two real D5 sessions ran (A then B) and were verified by the engine", sa and sb and sa["status"] == "playback_corroborated" and sb["status"] == "playback_corroborated", (sa and sa.get("status"), sb and sb.get("status")))
    sid_a, sid_b = sa["session_id"], sb["session_id"]

    # ---- API contract on the persisted evidence
    st, cov = http("GET", "/api/pop/search/display")
    check("coverage: 6 searchable display frames from 2 sessions, with their capture time range", st == 200 and cov["searchable_rows"] == 6 and cov["sessions"] == 2 and cov["oldest_utc_ms"] <= cov["newest_utc_ms"], cov)
    refA = jpeg_bytes(display_picture("A"))
    st, r = http("POST", "/api/pop/search/display?top_k=10", refA, "image/jpeg")
    hits = r.get("results", [])
    check("API: searching with ad A's picture returns only session A's three frames, ranked, each at or above the match threshold",
          st == 200 and len(hits) == 3 and all(h["session_id"] == sid_a for h in hits) and all(h["similarity"] >= 0.8 for h in hits) and [h["rank"] for h in hits] == [1, 2, 3], (st, [(h["session_id"], h["similarity"]) for h in hits]))
    h0 = hits[0] if hits else {}
    need = ["evidence_id", "session_id", "similarity", "capture_utc_ms", "offset_in_play_ms", "advertisement_id", "advertisement_name", "expected_media_id", "played_media_id", "session_status", "session_link", "image_url", "capture_method", "provenance", "time_uncertainty_ms"]
    check("API: each hit carries image, capture timestamp, advertisement id, playback session id and link", all(k in h0 for k in need) and h0.get("advertisement_id") == "ad-a" and h0["session_link"] == "#pop?session=" + sid_a, h0)
    check("API: hits come from the Weston/VOP2 path with provenance and uncertainty", h0.get("capture_method") == "resilience_grpc" and "weston_vop2_writeback" in h0.get("provenance", "") and 0 <= h0.get("time_uncertainty_ms", -1) < 1500, h0)
    ts = [h["capture_utc_ms"] for h in hits]
    sess = http("GET", "/api/pop/sessions/" + sid_a)[1]
    check("API: capture time matches the stored evidence of that session", sorted(ts) == sorted(e["utc_ms"] for e in sess["evidence"]), (ts, [e["utc_ms"] for e in sess["evidence"]]))
    sti, img = 0, b""
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{API_PORT}{h0['image_url']}", timeout=10) as rr: sti, img = rr.status, rr.read()
    except Exception as e: logs.append("img " + str(e))
    check("API: the hit's image URL serves the stored JPEG", sti == 200 and img[:2] == b"\xff\xd8" and Image.open(io.BytesIO(img)).size == (640, 360))
    st, r = http("POST", "/api/pop/search/display?media_id=ad-b", refA, "image/jpeg")
    check("API: advertisement filter ad-b with ad A's picture finds nothing and says 'No matching stored evidence' (closest below minimum shown)",
          st == 200 and r["results"] == [] and r["message"] == "No matching stored evidence" and r["closest_below_minimum"]["session_id"] == sid_b and r["closest_below_minimum"]["similarity"] < 0.8, r)
    check("API: the no-match message does not claim the content was never played", "does not show that the content was never played" in r["message_detail"], r["message_detail"])
    st, r = http("POST", "/api/pop/search/display", jpeg_bytes(display_picture("C")), "image/jpeg")
    check("API: a picture that was never on the display gives no match", st == 200 and r["results"] == [] and r["no_match"] and r["status"]["candidates_scanned"] == 6, r)
    t_lo, t_hi = min(ts), max(ts)
    st, r = http("POST", f"/api/pop/search/display?start={t_lo}&end={t_lo + 1}", refA, "image/jpeg")
    check("API: capture-time window restricts the candidates (half-open)", st == 200 and r["status"]["candidates_scanned"] == 1 and len(r["results"]) == 1, r)
    st, r = http("POST", "/api/pop/search/display?method=brightscript_screenshot", refA, "image/jpeg")
    check("API: the screenshot method finds no candidates in a gRPC-only store", st == 200 and r["status"]["candidates_scanned"] == 0 and r["no_match"], r)
    st, r = http("POST", "/api/pop/search/display?min_similarity=0.0&top_k=2", refA, "image/jpeg")
    check("API: top_k bounds the hits; a lower minimum is applied as requested and reported", st == 200 and len(r["results"]) == 2 and r["options"]["min_similarity"] == 0 and r["options"]["min_similarity_default"] is False, r)
    for q, why in (("top_k=0", "top_k 0"), ("top_k=51", "top_k 51"), ("min_similarity=2", "min 2"), ("method=x", "method"), ("start=5&end=5", "empty window")):
        st, r = http("POST", "/api/pop/search/display?" + q, refA, "image/jpeg")
        check(f"API: invalid argument rejected ({why})", st == 400, (st, r))
    st, r = http("POST", "/api/pop/search/display", b"not a jpeg", "image/jpeg")
    check("API: a non-JPEG body is rejected", st == 400, (st, r))
    st, r = http("POST", "/api/pop/search/display", refA, "image/jpeg", port=DASH_PORT, origin=False)
    check("dashboard proxy: cross-origin / no-origin upload is refused (403)", st == 403, (st, r))
    st, r = http("POST", "/api/pop/search/display", refA, "image/jpeg", port=DASH_PORT)
    check("dashboard proxy: same-origin upload reaches the search", st == 200 and len(r["results"]) == 3, (st, r))
    before = http("GET", "/api/pop/status")[1]
    http("POST", "/api/pop/search/display", refA, "image/jpeg")
    after = http("GET", "/api/pop/status")[1]
    check("searching writes nothing: sessions, evidence and verifications are unchanged", all(before.get(k) == after.get(k) for k in ("sessions", "events", "evidence", "verifications")), (before, after))

    # ---- the page
    chrome = start(["/usr/bin/google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu", f"--remote-debugging-port={CDP_PORT}", f"--user-data-dir={OUT}/profile",
                    "--window-size=1440,2200", "about:blank"], "chrome.log")
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

        async def wait_for(js, timeout=12):
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

        await cmd("Page.enable"); await cmd("Runtime.enable")
        await cmd("Page.navigate", url=BASE + "?t=%d#search" % int(time.time() * 1000))
        check("Visual Search opens on the Audience Camera source with its existing modes and controls unchanged",
              await wait_for("(()=>{const v=document.getElementById('view-search');const t=v.innerText;return /Audience Camera/.test(t)&&/Full frame search/.test(t)&&/Object search/.test(t)&&/Pick a stored frame/.test(t)&&/Top K/.test(t)})()", 15))
        check("the source selector offers Audience Camera and BrightSign Display (Weston/VOP2)", await ev("[...document.querySelectorAll('#view-search [aria-label=\"Search source\"] button')].map(b=>b.textContent).join('|')") == "Audience Camera|BrightSign Display (Weston/VOP2)")
        check("camera search panel is visible and the display panel hidden", await ev("(()=>{const f=[...document.querySelectorAll('#view-search .vm-fs')];return f.length>=1&&!f[0].closest('div[hidden]')&&f.slice(1).every(x=>!!x.closest('div[hidden]'))})()") is True)
        await shot("1_camera_source")
        await ev("[...document.querySelectorAll('#view-search button')].find(b=>/^BrightSign Display/.test(b.textContent)).click(), true")
        check("selecting BrightSign Display shows its own note and hides the camera search", await wait_for("(()=>{const v=document.getElementById('view-search').innerText;return /Weston\\/VOP2 output frames/.test(v)&&/stored as evidence/.test(v)})()", 10))
        check("the display page states the coverage: 6 frames from 2 sessions with the capture range", await wait_for("/Searchable stored evidence: 6 frames from 2 playback sessions, captured between/.test(document.getElementById('view-search').innerText)", 10), await ev("document.getElementById('view-search').innerText.slice(0,900)"))
        check("the advertisement list is filled from the registry", await wait_for("[...document.querySelectorAll('#view-search select[aria-label=Advertisement] option')].map(o=>o.value).join()==',ad-a,ad-b'", 10))
        check("Search is disabled until a reference is chosen", await ev("[...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Search display evidence').disabled") is True)
        await upload(refA)
        check("the uploaded reference is previewed and Search becomes enabled", await wait_for("(()=>{const b=[...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Search display evidence');return !b.disabled && /Uploaded reference/.test(document.getElementById('view-search').innerText)})()", 10))
        await ev("[...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Search display evidence').click(), true")
        check("results: three match cards, each with the reference beside the stored frame", await wait_for("document.querySelectorAll('#view-search .ds-hit').length===3", 15))
        check("both images of every card decode (reference and stored 640x360 frame)", await wait_for("[...document.querySelectorAll('#view-search .ds-hit')].every(c=>{const im=[...c.querySelectorAll('img')];return im.length===2&&im.every(i=>i.complete&&i.naturalWidth>0)})", 15))
        txt = await ev("document.querySelector('#view-search .ds-hit').innerText")
        check("a card shows similarity, capture time, offset in the play, advertisement name and id, session id and status",
              re.search(r"similarity [01]\.\d{4}", txt) and "Meet BrightSign" in txt and "ad-a" in txt and sid_a in txt and "Playback corroborated" in txt and "into the play" in txt and "Weston/VOP2 frame" in txt, txt)
        check("cards are ranked by similarity", (lambda s: s == sorted(s, reverse=True))([float(x) for x in await ev("[...document.querySelectorAll('#view-search .ds-hit .vm-score')].map(e=>e.textContent.replace('similarity ',''))")]))
        await shot("2_results")
        await ev("document.querySelector('#view-search .ds-hit a[data-session-link]').click(), true")
        check("the link opens that playback session on the Proof of Play page", await wait_for("location.hash==='#pop'||document.body.dataset.view==='pop'", 10) and
              await wait_for("document.querySelectorAll('.vm-modal .pop-ev').length===3 && document.querySelector('.vm-modal').innerText.includes('%s')" % sid_a, 15), await ev("location.hash+' '+(document.querySelector('.vm-modal')||{}).innerText"))
        await shot("3_session_from_link")
        await ev("document.querySelector('.vm-modal-head button').click()")
        # no-match path in the page
        await cmd("Page.navigate", url=BASE + "?t=%d#search?source=display" % int(time.time() * 1000))
        check("the deep link #search?source=display opens the BrightSign Display source", await wait_for("/Searchable stored evidence/.test(document.getElementById('view-search').innerText)", 15))
        await upload(jpeg_bytes(display_picture("C")))
        await wait_for("!([...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Search display evidence').disabled)", 10)
        await ev("[...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Search display evidence').click(), true")
        check("no match: the page says 'No matching stored evidence' and shows the closest frame as below the minimum",
              await wait_for("/No matching stored evidence/.test(document.querySelector('#view-search .ds-nomatch')?.innerText||'') && document.querySelectorAll('#view-search .ds-closest').length===1", 15))
        nm = await ev("document.querySelector('#view-search .ds-nomatch').innerText + ' ' + document.querySelector('#view-search .ds-closest').innerText")
        check("the no-match text does not claim the content was never played", "does not show that the content was never played" in nm and "closest stored frame" in nm.lower(), nm)
        await shot("4_no_match")
        await cmd("Page.navigate", url=BASE + "?t=%d#search" % int(time.time() * 1000))
        await wait_for("true", 1)
        check("switching back to the Audience Camera source restores the camera search", await wait_for("(()=>{const f=[...document.querySelectorAll('#view-search .vm-fs')];return f.length===0||!f[0].closest('div[hidden]')})()", 5))
        player.stop_flag = True


try:
    asyncio.run(main())
except BaseException:
    traceback.print_exc(); results.append(False); print("FAIL test aborted by an exception")
finally:
    for p in procs: stop(p)
    time.sleep(0.3)
    errs = [l for l in logs if "EXC" in l or "CONSOLE" in l or "EVALERR" in l]
    print("console log: " + ("; ".join(errs) or "none"))
    check("no console errors or exceptions", not errs, errs)
    shutil.rmtree(tmp, ignore_errors=True)
    print("RESULT %d/%d passed" % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)
