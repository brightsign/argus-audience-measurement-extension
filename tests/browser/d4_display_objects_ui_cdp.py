#!/usr/bin/env python3
"""Real-browser (Chrome CDP) test of Visual Search -> Object search with the source "BrightSign Display (Weston/VOP2)".

  d4_display_objects_ui_cdp.py VM_API_SERVER DASHBOARD_SERVER VM_INGEST_DIR VM_POP_FIXTURE OUTDIR

Real processes: vm_api_server (mock detector + embedder, display auto indexing on), the Go dashboard server, headless Chrome. Data: a synthetic camera store and
a Proof-of-Play store written through the real PopStore API (640x360 Weston-style frames with letterbox bars; the camera ids and the PoP ids overlap on purpose).
Checks: the source switch, the display automatic-indexing panel and backfill, object cards with ad / offset / session, choosing an object in a stored display frame,
search (display scope only), the link to the Proof-of-Play session, source isolation, expired parent frames. Synthetic pictures only.
"""
import asyncio, base64, json, os, shutil, signal, subprocess, sys, tempfile, time, urllib.request, websockets
from PIL import Image, ImageDraw

API_BIN, DASH_BIN, INGEST_BIN, FIXTURE_BIN, OUT = sys.argv[1:6]
os.makedirs(OUT, exist_ok=True)
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
API_PORT, DASH_PORT, CDP_PORT = 20713, 20712, 9611
BASE = f"http://127.0.0.1:{DASH_PORT}/"
tmp = tempfile.mkdtemp(prefix="d4disp_")
CAM, POP = os.path.join(tmp, "cam"), os.path.join(tmp, "pop")
results, logs, procs = [], [], []


def check(name, cond, extra=""):
    results.append(bool(cond)); print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(extra)[:400]) if not cond and extra != "" else ""), flush=True)


def start(cmd, log):
    p = subprocess.Popen(cmd, stdout=open(os.path.join(tmp, log), "w"), stderr=subprocess.STDOUT); procs.append(p); return p


def stop(p):
    try:
        p.send_signal(signal.SIGTERM); p.wait(timeout=5)
    except Exception:
        try: p.kill()
        except Exception: pass


RED, GREEN, BLUE, YELLOW = (220, 30, 30), (30, 200, 40), (30, 40, 220), (230, 220, 30)


def display_frame(path, col, x, y, w, h):
    im = Image.new("RGB", (640, 360), (0, 0, 0)); d = ImageDraw.Draw(im)
    d.rectangle([80, 0, 559, 359], fill=(128, 128, 128)); d.rectangle([x, y, x + w - 1, y + h - 1], fill=col); im.save(path, quality=92)


async def main():
    os.makedirs(os.path.join(tmp, "src")); os.makedirs(os.path.join(tmp, "dsp")); os.makedirs(os.path.join(tmp, "jobs")); open(os.path.join(tmp, "events.jsonl"), "w").close()
    cam_files = []
    for i in range(6):
        col, (w, h) = ((RED, (40, 100)) if i % 2 == 0 else (GREEN, (50, 50)))
        im = Image.new("RGB", (480, 360), (128, 128, 128)); ImageDraw.Draw(im).rectangle([30 + i * 30, 40, 30 + i * 30 + w - 1, 40 + h - 1], fill=col)
        p = os.path.join(tmp, "src", f"c{i}.jpg"); im.save(p, quality=92); cam_files.append(p)
    subprocess.run([INGEST_BIN, "--root", CAM, "--model", "none", "--span-minutes", "30"] + cam_files, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    now = int(time.time() * 1000); TA, TB = now - 3600000, now - 3000000
    args = [FIXTURE_BIN, "--root", POP, "--session", f"sa:ad-a:ad-a:{TA}", "--session", f"sb:ad-b:ad-a:{TB}"]
    spec = [("sa", 0, RED, 100, 100, 40, 100), ("sa", 1, GREEN, 200, 120, 56, 56), ("sa", 2, BLUE, 300, 100, 60, 120), ("sa", 3, RED, 84, 80, 50, 120),
            ("sb", 0, RED, 200, 100, 40, 100), ("sb", 1, GREEN, 300, 150, 56, 56), ("sb", 2, RED, 400, 90, 36, 96)]
    for sid, k, col, x, y, w, h in spec:
        f = os.path.join(tmp, "dsp", f"{sid}_{k}.jpg"); display_frame(f, col, x, y, w, h)
        args += ["--hist", f"{sid}:{k}:{(TA if sid == 'sa' else TB) + 500 + k * 1000}:{f}:{'ad-a' if sid == 'sa' else 'ad-b'}"]
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
    cfg = os.path.join(tmp, "cfg.json")
    json.dump({"visual_memory": {"enabled": True, "vector_search_enabled": True, "storage_root": CAM, "stored_width": 640, "stored_height": 360,
                                 "embedding": {"enabled": True, "backend": "rknn", "model_path": "x"},
                                 "objects": {"enabled": True, "auto": {"enabled": True, "live_scan": False, "backfill_pause_ms": 60, "max_backfill_frames": 50, "display": {"enabled": True, "live_scan": False}}},
                                 "pop": {"enabled": True, "storage_root": POP, "events_path": os.path.join(tmp, "events.jsonl"), "jobs_dir": os.path.join(tmp, "jobs"), "poll_ms": 100,
                                         "capture_backend": "resilience_grpc", "history": {"enabled": True}}}}, open(cfg, "w"))
    start([API_BIN, "--config", cfg, "--port", str(API_PORT), "--provider", "mock:64", "--detector", "mock", "--no-reexec", "--status-file", os.path.join(tmp, "st.json")], "api.log")
    start([DASH_BIN, "-port", str(DASH_PORT), "-dir", os.path.join(ROOT_DIR, "dashboard"), "-vm-upstream", f"127.0.0.1:{API_PORT}"], "dash.log")
    time.sleep(2.5)
    chrome = start(["/usr/bin/google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu", f"--remote-debugging-port={CDP_PORT}", f"--user-data-dir={OUT}/profile", "--window-size=1440,2600", "about:blank"], "chrome.log")
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

        async def wait_for(js, timeout=15):
            t0 = time.time()
            while time.time() - t0 < timeout:
                if await ev(js): return True
                await asyncio.sleep(0.25)
            return False

        async def setv(sel, v):
            await ev("(()=>{const e=document.querySelector(%s);e.value=%s;e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new Event('change',{bubbles:true}));return 1})()" % (json.dumps(sel), json.dumps(str(v))))

        P = "document.querySelector('[data-role=auto-panel]')"
        await cmd("Page.enable"); await cmd("Runtime.enable")
        await cmd("Emulation.setDeviceMetricsOverride", width=1440, height=2600, deviceScaleFactor=1, mobile=False)
        await cmd("Page.navigate", url=BASE + "?t=%d#search?mode=objects" % int(time.time() * 1000))
        check("Object search opens on the Audience Camera source (unchanged default)", await wait_for(f"!!{P} && /Automatic object indexing/i.test({P}.innerText) && !/display/i.test({P}.querySelector('h2').innerText)"))
        src_btns = await ev("[...document.querySelectorAll('#view-search [aria-label=\"Search source\"] button')].map(b=>b.textContent).join('|')")
        check("the source switch offers Audience Camera and BrightSign Display (Weston/VOP2)", src_btns == "Audience Camera|BrightSign Display (Weston/VOP2)", src_btns)
        cam_tot = await ev("(document.querySelector('[data-role=object-totals],[data-role=empty-index]')||{}).innerText")
        # ---- switch to the display source, still in Object search mode
        await ev("[...document.querySelectorAll('#view-search [aria-label=\"Search source\"] button')].find(b=>/BrightSign Display/.test(b.textContent)).click(), 1")
        check("BrightSign Display + Object search: the display panel and an honest empty state are shown",
              await wait_for(f"/Automatic object indexing: BrightSign display/i.test({P}.innerText) && !!document.querySelector('[data-role=empty-index]') && /No BrightSign display objects are indexed yet/.test(document.querySelector('[data-role=empty-index]').innerText)"),
              await ev(f"({P}||document.body).innerText.slice(0,300)"))
        txt = await ev(f"{P}.innerText")
        check("the panel says frames exist only for registered Proof-of-Play sessions and shows 0 of 7 processed", "0 display frames processed of 7 stored" in txt and "registered Proof-of-Play sessions" in txt, txt)
        check("the object-search note says it searches indexed objects only", "not every stored frame" in (await ev("document.getElementById('view-search').innerText")))
        await shot("1_display_empty")

        # ---- backfill display frames
        await ev("document.querySelector('[data-role=backfill-run]').click(), 1")
        check("display backfill runs and finishes in the UI", await wait_for("(()=>{const e=document.querySelector('[data-role=backfill-state]');return !!e&&/Backfill done: 7\\/7/.test(e.innerText)})()", 40), await ev("(document.querySelector('[data-role=backfill-state]')||{}).innerText"))
        txt = await ev(f"{P}.innerText")
        check("progress: 7 display frames processed, 6 with objects (the blue 'person' frame has none), 6 automatic objects", "7 display frames processed of 7 stored" in txt and "6 with objects" in txt and "6 automatic objects" in txt, txt)
        chips = await ev("[...document.querySelectorAll('[data-role=class-chips] button')].map(b=>b.textContent).join('|')")
        check("class chips: bottle 4, cup 2", chips == "bottle 4|cup 2", chips)
        check("totals say display objects and processed display frames", await wait_for("/6 indexed display objects \\(0 manual · 6 automatic\\)/.test((document.querySelector('[data-role=object-totals]')||{innerText:''}).innerText)", 20),
              await ev("(document.querySelector('[data-role=object-totals]')||{}).innerText"))
        await shot("2_display_indexed")

        # ---- cards: advertisement, offset, session, class/confidence
        await wait_for("document.querySelectorAll('#view-search .vm-card').length===6")
        card = await ev("[...document.querySelectorAll('#view-search .vm-card')].find(c=>/session sb/.test(c.innerText)).innerText.replace(/\\n+/g,' | ')")
        check("a display object card shows class, confidence, the advertisement (expected vs player-reported), the offset into the play and the session",
              card and "bottle" in card and "ad ad-b (player reported ad-a)" in card and "into the play" in card and "±252 ms" in card and "session sb" in card and "display frame #" in card, card)

        # ---- choose an object in a stored DISPLAY frame
        await ev("[...document.querySelectorAll('#view-search button')].find(b=>/Select object in a stored display frame/.test(b.textContent)).click(), 1")
        check("the display frame picker opens with the Proof-of-Play sessions", await wait_for("!!document.querySelector('.vm-modal [data-role=session-filter]') && document.querySelector('.vm-modal [data-role=session-filter]').options.length>=3"), await ev("(document.querySelector('.vm-modal')||{innerText:''}).innerText.slice(0,300)"))
        await setv(".vm-modal [data-role=session-filter]", "sa")
        check("choosing a session lists its stored frames (4 history frames)", await wait_for("document.querySelectorAll('.vm-modal [data-frame-kind]').length===4"), await ev("document.querySelectorAll('.vm-modal [data-frame-kind]').length"))
        check("the frame thumbnails load (Weston JPEGs, 640x360)", await wait_for("(()=>{const i=document.querySelector('.vm-modal [data-frame-kind] img');return !!i&&i.complete&&i.naturalWidth===640&&i.naturalHeight===360})()"))
        await ev("document.querySelector('.vm-modal [data-frame-kind=history][data-frame-id=\"1\"]').click(), 1")
        check("the object selector opens on the display frame", await wait_for("!!document.querySelector('.vm-roi-stage img') && document.querySelector('.vm-roi-stage img').complete && document.querySelector('.vm-roi-stage img').naturalWidth===640"), await ev("(document.querySelector('.vm-modal')||{innerText:''}).innerText.slice(0,200)"))
        check("its title says BrightSign display frame", "BrightSign display frame #1 (history)" in (await ev("document.querySelector('.vm-modal-head').innerText")))
        for k, v in (("X", 100), ("Y", 100), ("W", 40), ("H", 100)): await setv(f".vm-modal input[aria-label={k}]", v)
        check("the crop preview of the box comes from the display frame", await wait_for("(()=>{const i=document.querySelector('.vm-roi-prev img');return !!i&&i.complete&&i.naturalWidth===40&&i.naturalHeight===100&&/display-frames\\/history\\/1\\/crop/.test(i.src)})()"), await ev("(document.querySelector('.vm-roi-prev img')||{}).src"))
        await shot("3_display_selector")
        await ev("[...document.querySelectorAll('.vm-modal button')].find(b=>b.textContent==='Find Similar Objects').click(), 1")
        check("search results: only display objects, top hit is the identical object, scope says display objects only",
              await wait_for("(()=>{const g=document.querySelector('[data-role=result-grid]');const s=document.querySelector('[data-role=search-scope]');return !!g&&g.querySelectorAll('.vm-card').length>=5&&[...g.querySelectorAll('.vm-card')].every(c=>c.dataset.source==='display')&&!!s&&/display objects only/.test(s.innerText)&&/6 indexed display objects/.test(s.innerText)})()", 20),
              await ev("(document.querySelector('[data-role=search-scope]')||{}).innerText"))
        first = await ev("document.querySelector('[data-role=result-grid] .vm-card .vm-score').innerText")
        check("the top similarity is ~1 (the very object that was selected)", first and float(first) > 0.999, first)
        await shot("4_display_results")

        # ---- compare dialog + session link
        await ev("document.querySelector('[data-role=result-grid] .vm-card').click(), 1")
        check("the compare dialog shows the advertisement, playback offset, session and the source display frame with its bounding box",
              await wait_for("(()=>{const m=document.querySelector('.vm-modal');return !!m&&/BrightSign display frame #\\d+ \\(history, Weston\\/VOP2\\)/.test(m.innerText)&&/ad ad-a/.test(m.innerText)&&/into the play/.test(m.innerText)&&!!m.querySelector('[data-role=session-link]')&&!!m.querySelector('.vm-roi-box')})()"),
              await ev("(document.querySelector('.vm-modal')||{innerText:''}).innerText.slice(0,500)"))
        check("the source display frame image loads", await wait_for("(()=>{const i=document.querySelector('.vm-modal .vm-roi-frame img');return !!i&&i.complete&&i.naturalWidth===640})()"))
        await shot("5_display_compare")
        href = await ev("document.querySelector('.vm-modal [data-role=session-link]').getAttribute('href')")
        check("the session link points at the Proof-of-Play session of that frame", href in ("#pop?session=sa", "#pop?session=sb"), href)
        await ev("document.querySelector('.vm-modal [data-role=session-link]').click(), 1")
        check("clicking it opens that session in Proof of Play", await wait_for("location.hash.startsWith('#pop') && /(^|[^a-z])s[ab]([^a-z]|$)/.test(document.body.innerText)", 20), await ev("location.hash"))
        await shot("6_pop_session")

        # ---- source isolation in the UI
        await cmd("Page.navigate", url=BASE + "?t=%d#search?mode=objects" % int(time.time() * 1000))
        await wait_for(f"!!{P} && /Automatic object indexing/i.test({P}.innerText)")
        cam_txt = await ev("(document.querySelector('[data-role=object-totals],[data-role=empty-index]')||{}).innerText")
        check("back on the Audience Camera source: camera totals only (no display objects counted)", cam_txt and "display" not in cam_txt.lower() and "6 indexed" not in cam_txt, cam_txt)
        await ev("[...document.querySelectorAll('#view-search [aria-label=\"Search source\"] button')].find(b=>/BrightSign Display/.test(b.textContent)).click(), 1")
        await wait_for("/6 indexed display objects/.test((document.querySelector('[data-role=object-totals]')||{innerText:''}).innerText)", 20)
        await ev("[...document.querySelectorAll('#view-search [aria-label=\"Search source\"] button')].find(b=>/Audience Camera/.test(b.textContent)).click(), 1")
        check("switching sources resets the query and lists camera objects only", await wait_for("(()=>{const c=[...document.querySelectorAll('#view-search .vm-card')];return c.every(x=>!/display frame/.test(x.innerText))&&!/display/i.test((document.querySelector('[data-role=auto-panel] h2')||{innerText:'display'}).innerText)})()"))

        # ---- expired parent frames: the object crop stays inspectable
        removed = 0
        for dp, _, fn in os.walk(os.path.join(POP, "pop", "history")):
            for f in fn:
                if f.endswith(".jpg"): os.remove(os.path.join(dp, f)); removed += 1
        check("display history JPEGs removed (simulated expiry)", removed >= 7, removed)
        await cmd("Page.navigate", url=BASE + "?t=%d#search?mode=objects" % int(time.time() * 1000))
        await wait_for(f"!!{P}")
        await ev("[...document.querySelectorAll('#view-search [aria-label=\"Search source\"] button')].find(b=>/BrightSign Display/.test(b.textContent)).click(), 1")
        await wait_for("document.querySelectorAll('#view-search .vm-card').length>=6")
        await ev("document.querySelector('#view-search .vm-card').click(), 1")
        check("expired parent: the result card keeps the object crop and says the display frame JPEG is gone",
              await wait_for("(()=>{const c=document.querySelector('#view-search [data-role=result-grid] .vm-card');return !!c&&/crop retained/.test(c.innerText)&&/frame JPEG (missing|expired)/.test(c.innerText)})()", 20),
              await ev("(document.querySelector('#view-search .vm-card')||{innerText:''}).innerText"))
        check("the crop thumbnail still loads", await wait_for("(()=>{const i=document.querySelector('#view-search [data-role=result-grid] .vm-card img');return !!i&&i.complete&&i.naturalWidth>8})()"))
        await ev("document.querySelector('#view-search [data-role=result-grid] .vm-card').click(), 1")
        check("the compare dialog states that the display frame JPEG is gone and still shows ad / offset / session",
              await wait_for("(()=>{const m=document.querySelector('.vm-modal');return !!m&&!!m.querySelector('[data-role=frame-gone]')&&/ad ad-/.test(m.innerText)&&!!m.querySelector('[data-role=session-link]')})()"),
              await ev("(document.querySelector('.vm-modal')||{innerText:''}).innerText.slice(0,500)"))
        await shot("7_display_expired")
        check("no console errors", not [l for l in logs if "favicon" not in l], logs)
    for p in procs[::-1]: stop(p)
    print("RESULT %d/%d passed" % (sum(results), len(results)))
    shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(0 if all(results) else 1)

try:
    asyncio.run(main())
except Exception:
    import traceback; traceback.print_exc()
    for p in procs[::-1]: stop(p)
    sys.exit(2)
