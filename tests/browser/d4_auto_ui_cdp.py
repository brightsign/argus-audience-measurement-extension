#!/usr/bin/env python3
"""Real-browser (Chrome CDP) test of the D4 Object Search dashboard with automatic indexing.

  d4_auto_ui_cdp.py VM_API_SERVER DASHBOARD_SERVER VM_INGEST_DIR OUTDIR

Real processes: vm_api_server (mock embedder, mock colour detector, auto indexing ON, live scan OFF), the Go dashboard server and headless Chrome,
on a synthetic store. Checks the automatic-indexing panel, manual vs automatic badges, class filters/chips, crop thumbnails, detector confidence,
source frame references with bounding boxes, availability of crops and frames after the frames expire, the scope sentence, and the VM health
pill logic for the cumulative Stage-1 database failure counter. Synthetic pictures only.
"""
import asyncio, base64, json, os, shutil, signal, subprocess, sys, tempfile, time, urllib.request, websockets
from PIL import Image, ImageDraw

API_BIN, DASH_BIN, INGEST_BIN, OUT = sys.argv[1:5]
os.makedirs(OUT, exist_ok=True)
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
API_PORT, DASH_PORT, CDP_PORT = 20693, 20692, 9594
BASE = f"http://127.0.0.1:{DASH_PORT}/"
tmp = tempfile.mkdtemp(prefix="d4auto_")
root = os.path.join(tmp, "store")
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


BG = [(128, 128, 128), (60, 70, 80), (200, 190, 180), (90, 120, 90)]
SHAPES = [("bottle", (220, 30, 30), (40, 100)), ("cup", (30, 200, 40), (50, 50)), ("person", (30, 40, 220), (60, 120)), ("laptop", (230, 220, 30), (80, 50)), ("bottle", (220, 30, 30), (36, 96))]


async def main():
    os.makedirs(os.path.join(tmp, "src"))
    files = []
    for i in range(20):
        _, col, (w, h) = SHAPES[i % 5]
        x, y = 30 + (i * 37) % 300, 20 + (i * 23) % 200
        im = Image.new("RGB", (480, 360), BG[(i // 5) % 4]); ImageDraw.Draw(im).rectangle([x, y, x + w - 1, y + h - 1], fill=col)
        p = os.path.join(tmp, "src", f"f{i:02d}.jpg"); im.save(p, quality=92); files.append(p)
    subprocess.run([INGEST_BIN, "--root", root, "--model", "none", "--span-minutes", "30"] + files, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    cfg = os.path.join(tmp, "cfg.json")
    json.dump({"visual_memory": {"enabled": True, "vector_search_enabled": True, "storage_root": root, "stored_width": 640, "stored_height": 360,
                                 "embedding": {"enabled": True, "backend": "rknn", "model_path": "x"},
                                 "objects": {"enabled": True, "auto": {"enabled": True, "live_scan": False, "backfill_pause_ms": 100, "max_backfill_frames": 50}}}}, open(cfg, "w"))
    start([API_BIN, "--config", cfg, "--port", str(API_PORT), "--provider", "mock:64", "--detector", "mock", "--no-reexec", "--status-file", os.path.join(tmp, "st.json")], "api.log")
    start([DASH_BIN, "-port", str(DASH_PORT), "-dir", os.path.join(ROOT_DIR, "dashboard"), "-vm-upstream", f"127.0.0.1:{API_PORT}"], "dash.log")
    time.sleep(2.5)

    chrome = start(["/usr/bin/google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu", f"--remote-debugging-port={CDP_PORT}", f"--user-data-dir={OUT}/profile",
                    "--window-size=1440,2400", "about:blank"], "chrome.log")
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

        P = "document.querySelector('[data-role=auto-panel]')"
        await cmd("Page.enable"); await cmd("Runtime.enable")
        await cmd("Emulation.setDeviceMetricsOverride", width=1440, height=2400, deviceScaleFactor=1, mobile=False)
        await cmd("Page.navigate", url=BASE + "?t=%d#search?mode=objects" % int(time.time() * 1000))
        check("the automatic-indexing panel is shown on Object Search", await wait_for(f"!!{P} && /Automatic object indexing/i.test({P}.innerText)"), await ev(f"({P}||document.body).innerText.slice(0,300)"))
        txt = await ev(f"{P}.innerText")
        check("it states the worker state, live scan off, detector and thresholds", "worker:" in txt and "live scan off" in txt and "min confidence 0.30" in txt, txt)
        check("it lists the allowlisted classes and says people are never indexed",
              "bottle, cup, wine glass, laptop, cell phone, book" in txt and "People are never indexed" in txt, txt)
        check("before any run: 0 frames processed of 20 stored, honest about expired frames", "0 frames processed of 20 stored" in txt and "expired frames cannot be processed" in txt, txt)
        check("the page says object search covers only indexed objects", "not every stored frame" in (await ev("document.getElementById('view-search').innerText")))
        await shot("1_before")

        await ev("document.querySelector('[data-role=backfill-run]').click(), 1")
        await asyncio.sleep(1.0); print("   panel after click:", (await ev(f"{P}.innerText") or "")[-400:].replace("\n", " | "))
        check("backfill runs and finishes in the UI", await wait_for("(document.querySelector('[data-role=backfill-state]')||{}).innerText && /Backfill done: 20\\/20/.test(document.querySelector('[data-role=backfill-state]').innerText)", 40),
              await ev("(document.querySelector('[data-role=backfill-state]')||{}).innerText"))
        txt = await ev(f"{P}.innerText")
        check("progress: 20 frames processed, 16 with objects, 16 automatic objects, per-frame timings", "20 frames processed of 20 stored" in txt and "16 with objects" in txt and "16 automatic objects" in txt and "detector" in txt and "ms" in txt, txt)
        chips = await ev("[...document.querySelectorAll('[data-role=class-chips] button')].map(b=>b.textContent).join('|')")
        check("class chips show per-class counts (people excluded)", chips == "bottle 8|cup 4|laptop 4", chips)
        await wait_for("/16 indexed objects/.test((document.querySelector('[data-role=object-totals]')||{innerText:''}).innerText)", 20)
        tot = await ev("(document.querySelector('[data-role=object-totals]')||{}).innerText")
        check("totals: 16 indexed objects (0 manual · 16 automatic) with crop counts", tot and "16 indexed objects (0 manual · 16 automatic)" in tot and "16 retained, 0 expired" in tot, tot)
        await shot("2_after_backfill")

        # class chip -> filtered grid with badges, class + confidence, crop thumbnails
        await ev("document.querySelector('[data-class=bottle]').click(), 1")
        check("clicking a class chip filters the indexed objects to that class",
              await wait_for("(()=>{const c=[...document.querySelectorAll('#view-search .vm-card')];return c.length===8&&c.every(x=>/auto-indexed/.test(x.innerText)&&/bottle/.test(x.innerText))})()"), await ev("document.querySelectorAll('#view-search .vm-card').length"))
        cardtxt = await ev("document.querySelector('#view-search .vm-card').innerText")
        check("a card shows class, detector confidence, auto-indexed + crop retained badges", "bottle · 90% detector confidence" in cardtxt and "auto-indexed" in cardtxt and "crop retained" in cardtxt, cardtxt)
        check("the crop thumbnail is loaded (the object's own crop JPEG)", await wait_for("(()=>{const i=document.querySelector('#view-search .vm-card img');return !!i&&i.complete&&i.naturalWidth>8})()"))
        await ev("document.querySelector('[data-role=origin-filter]').value='manual'; document.querySelector('[data-role=origin-filter]').dispatchEvent(new Event('change')), 1")
        check("origin filter 'manual only' shows none (nothing was indexed by hand)", await wait_for("/None yet/.test(document.getElementById('view-search').innerText)"))
        await ev("document.querySelector('[data-role=origin-filter]').value=''; document.querySelector('[data-role=origin-filter]').dispatchEvent(new Event('change')), 1")
        await ev("document.querySelector('[data-role=class-filter]').value='bottle'; document.querySelector('[data-role=class-filter]').dispatchEvent(new Event('change')), 1")
        check("class dropdown filter works too", await wait_for("document.querySelectorAll('#view-search .vm-card').length===8"))

        # search from an automatic object (show 10 results so the 7 other bottles can be checked)
        await ev("document.querySelectorAll('#view-search input[aria-label=\"Top K\"]').forEach(k=>{k.value='10';k.dispatchEvent(new Event('input'))}), 1")
        await ev("document.querySelector('#view-search .vm-card').click(), 1")
        check("searching from an indexed object lists similar bottles first", await wait_for("(()=>{const c=[...document.querySelectorAll('#view-search [data-role=class-conf]')];return c.length>=7&&c.slice(0,7).every(x=>/^bottle/.test(x.innerText))})()", 20),
              await ev("[...document.querySelectorAll('#view-search [data-role=class-conf]')].map(x=>x.innerText).join('|')"))
        scope = await ev("(document.querySelector('[data-role=search-scope]')||{}).innerText")
        check("the result states what the search covered (indexed subset, processed frames)", scope and "16 indexed camera objects" in scope and "20 processed frames" in scope and "not searched" in scope, scope)
        rc = await ev("document.querySelector('#view-search .vm-grid:last-of-type .vm-card, #view-search .vm-card .vm-score') && [...document.querySelectorAll('#view-search [data-role=class-conf]')][0].closest('.vm-card').innerText")
        check("a result card carries similarity, timestamp, frame ref with box, origin/crop/frame badges", rc and "frame #" in rc and "auto-indexed" in rc and "crop retained" in rc and "frame JPEG available" in rc, rc)
        await shot("3_search")
        await ev("[...document.querySelectorAll('#view-search [data-role=class-conf]')][0].closest('.vm-card').click(), 1")
        check("compare dialog shows detector class/confidence/version and the source frame with its bounding box",
              await wait_for("(()=>{const m=document.querySelector('.vm-modal');return !!m&&/Detector/.test(m.innerText)&&/confidence/.test(m.innerText)&&/Source frame with bounding box/i.test(m.innerText)&&!!m.querySelector('.vm-roi-box')})()"),
              await ev("(document.querySelector('.vm-modal')||{innerText:''}).innerText.slice(0,400)"))
        await shot("4_compare")
        await ev("document.querySelectorAll('.vm-modal-head button').forEach(b=>b.click()), 1")

        # all full frames expire: crops stay inspectable, badges say so
        removed = 0
        for dp, _, fn in os.walk(os.path.join(root, "frames")):
            for f in fn:
                if f.endswith(".jpg"): os.remove(os.path.join(dp, f)); removed += 1
        check("frame JPEGs removed (simulated expiry)", removed >= 20, removed)
        await cmd("Page.navigate", url=BASE + "?t=%d#search?mode=objects" % int(time.time() * 1000))
        await wait_for(f"!!{P} && /Automatic object indexing/i.test({P}.innerText)")
        await wait_for("!!document.querySelector('[data-role=class-chips] [data-class=cup]')")
        print("   after expiry panel:", (await ev(f"({P}||document.body).innerText") or "")[:500].replace("\n", " | "))
        for _ in range(20):                                   # the panel re-renders when the first status poll lands; click once it is stable
            if await ev("(()=>{const b=document.querySelector('[data-role=class-chips] [data-class=cup]');if(!b)return false;b.click();return true})()"): break
            await asyncio.sleep(0.3)
        await wait_for("document.querySelectorAll('#view-search .vm-card').length===4")
        await ev("document.querySelector('#view-search .vm-card').click(), 1")
        check("expired frames: results still show the object crop and say the frame JPEG expired/missing",
              await wait_for("(()=>{const c=[...document.querySelectorAll('#view-search [data-role=class-conf]')];return c.length>=3&&c.every(x=>/crop retained/.test(x.closest('.vm-card').innerText)&&/frame JPEG (missing|expired)/.test(x.closest('.vm-card').innerText))})()", 20),
              await ev("[...document.querySelectorAll('#view-search .vm-card')].map(x=>x.innerText.replace(/\\n+/g,' ')).slice(0,2).join(' ## ')"))
        check("the crop thumbnail of an expired-frame result still loads", await wait_for("(()=>{const i=document.querySelector('#view-search .vm-card img');return !!i&&i.complete&&i.naturalWidth>8})()"))
        await shot("5_expired")

        # ---- VM health pill: the cumulative Stage-1 DB failure counter
        pill = lambda stage1, commit: ev("""(()=>{const S=window.VMUI.S;const keep=S.status;S.apiState='ok';
            S.status=Object.assign({},keep,{runtime:Object.assign({},keep.runtime||{},{running:true,stale:false,stage1:%s})});
            const r=window.VMUI.healthPill();S.status=keep;return r})()""" % stage1)
        now = int(time.time() * 1000)
        active = await pill(json.dumps({"database_failures": 1, "last_commit_utc_ms": now - 60000, "last_database_error": {"count": 1, "code": 5, "code_name": "SQLITE_BUSY", "message": "database is locked", "operation": "insert.step", "utc_ms": now - 30000, "frame_sequence": 77}}), None)
        check("a DB failure with no commit since is ACTIVE: red 'VM: errors', with code, message, operation, time", active["cls"] == "red" and active["text"] == "VM: errors" and any("SQLITE_BUSY" in r and "database is locked" in r and "insert.step" in r and "no successful commit since" in r for r in active["reasons"]), active)
        hist = await pill(json.dumps({"database_failures": 1, "last_commit_utc_ms": now - 1000, "last_database_error": {"count": 1, "code": 5, "code_name": "SQLITE_BUSY", "message": "database is locked", "operation": "insert.step", "utc_ms": now - 600000, "frame_sequence": 77}}), None)
        check("a failure followed by successful commits is a recovered HISTORICAL one: amber, still listed with details (not hidden)",
              hist["cls"] == "amber" and any("recovered" in r and "SQLITE_BUSY" in r and "1" in r for r in hist["reasons"]), hist)
        legacy = await pill(json.dumps({"database_failures": 1, "last_commit_utc_ms": now}), None)
        check("an older Argus build without error details: counted as a failure and flagged (no details to prove recovery)", legacy["cls"] == "red" and any("no error details" in r for r in legacy["reasons"]), legacy)
        none = await pill(json.dumps({"database_failures": 0, "last_commit_utc_ms": now}), None)
        check("no failures: no DB-failure reason", not any("DB failures" in r for r in none["reasons"]), none)
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
