#!/usr/bin/env python3
"""Regression test: the Visual Memory gallery must show the newest stored frames and keep up while frames are being
written (it used to load once and then show stale records while the status tiles moved on).

  vm_gallery_live_cdp.py VM_API_SERVER DASHBOARD_SERVER VM_LIVE_WRITER OUTDIR

Starts REAL processes on loopback: vm_api_server (mock embedder), the Go dashboard-server (serving ./dashboard) and a
writer (vm_live_writer) that behaves like the Argus writer process, then drives headless Chrome over CDP.

Covers: gallery order == direct SQLite query == /api/vm/frames; no-store caching; frames inserted while the gallery is open
appear without a reload and without re-creating the existing cards; "oldest first" (not following) keeps its view and offers
a notice instead; "Show newest"; an ingestion stall (no frames for > 30 s) shows a clear warning in the gallery and turns the
health pill red, and recovery clears it; the status API reports storage root, last commit and ingestion state.

Synthetic, people-free frames only. Exit 0 = all checks passed.
"""
import asyncio, base64, json, os, shutil, signal, sqlite3, subprocess, sys, tempfile, threading, time, traceback, urllib.request, websockets

API_BIN, DASH_BIN, WRITER_BIN, OUT = sys.argv[1:5]
os.makedirs(OUT, exist_ok=True)
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
API_PORT, DASH_PORT, CDP_PORT = 20481, 20480, 9382
BASE = f"http://127.0.0.1:{DASH_PORT}/"
tmp = tempfile.mkdtemp(prefix="vm_live_")
store = os.path.join(tmp, "store"); os.makedirs(store)
status_file = os.path.join(tmp, "argus_vm_status.json")
cfgp = os.path.join(tmp, "cfg.json")
json.dump({"visual_memory": {"enabled": True, "storage_root": store, "sample_interval_ms": 5000, "retention_hours": 0, "max_storage_bytes": 0}}, open(cfgp, "w"))
results, logs, procs = [], [], []


def check(name, cond, extra=""):
    results.append(bool(cond)); print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(extra)) if not cond and extra != "" else ""), flush=True)


def get(path, port=DASH_PORT):
    r = urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=10)
    return json.loads(r.read()), r.headers


def writer(count, interval_ms):
    p = subprocess.Popen([WRITER_BIN, store, str(count), str(interval_ms)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return p


# Fake the Argus runtime snapshot (the real one is written by the Argus process every 5 s) so the API sees a live writer.
snap_lock = threading.Lock(); snap_stop = threading.Event()
snap = {"commit": 0}


def snapshot_loop():
    while not snap_stop.is_set():
        with snap_lock:
            body = {"written_utc_ms": int(time.time() * 1000),
                    "stage1": {"samples_seen": 10, "accepted": 10, "encoded": 10, "dropped_queue_full": 0, "dropped_invalid": 0, "jpeg_write_failures": 0,
                               "database_failures": 0, "retention_deletions": 0, "queue_depth": 0, "max_queue_depth": 1, "bytes_on_disk": 0,
                               "last_commit_utc_ms": snap["commit"], "retention_pending": False, "retention_slices": 0},
                    "stage2": None, "model": None}
        tmpf = status_file + ".tmp"; open(tmpf, "w").write(json.dumps(body)); os.replace(tmpf, status_file)
        snap_stop.wait(1.0)


def db_newest(n):
    c = sqlite3.connect(f"file:{store}/visual_memory.db?mode=ro", uri=True)
    return [(r[0], r[1]) for r in c.execute("select id,capture_utc_ms from frames order by capture_utc_ms desc,id desc limit ?", (n,))]


async def main():
    api = subprocess.Popen([API_BIN, "--config", cfgp, "--port", str(API_PORT), "--provider", "mock:64", "--status-file", status_file],
                           stderr=open(os.path.join(tmp, "api.log"), "w"))
    procs.append(api)
    dash = subprocess.Popen([DASH_BIN, "-port", str(DASH_PORT), "-dir", os.environ.get("VM_DASHBOARD_DIR", os.path.join(ROOT_DIR, "dashboard")), "-vm-upstream", f"127.0.0.1:{API_PORT}"],
                            stdout=open(os.path.join(tmp, "dash.log"), "w"), stderr=subprocess.STDOUT)
    procs.append(dash)
    threading.Thread(target=snapshot_loop, daemon=True).start()
    w = writer(4, 250); w.wait()                                   # 4 frames exist before the page is opened
    for _ in range(60):
        try: get("/api/vm/status"); break
        except Exception: time.sleep(0.2)

    # ---- API vs direct database, and caching -------------------------------------------------
    d, hdr = get("/api/vm/frames?limit=10&order=desc")
    direct = db_newest(10)
    check("API frame order equals a direct SQL query (newest first)", [(f["id"], f["capture_utc_ms"]) for f in d["frames"]] == direct, (d["frames"][:2], direct[:2]))
    check("frames API is not cacheable (Cache-Control: no-store)", "no-store" in (hdr.get("Cache-Control") or ""), dict(hdr))
    st, _ = get("/api/vm/status")
    check("status reports the active storage root", st.get("storage_root") == store, st.get("storage_root"))
    check("status: ingestion ok with a fresh frame", st["ingest"]["state"] == "ok" and st["ingest"]["age_ms"] < 20000, st["ingest"])
    check("status: newest frame in the status equals the newest the gallery API returns", st["store"]["newest_utc_ms"] == d["frames"][0]["capture_utc_ms"])
    check("status: no embeddings => latest_embedding_utc_ms is null", st["store"].get("latest_embedding_utc_ms") is None)

    # ---- browser -------------------------------------------------------------------------------
    chrome = subprocess.Popen(["/usr/bin/google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu", f"--remote-debugging-port={CDP_PORT}",
                               f"--user-data-dir={OUT}/profile", "--window-size=1440,1100", "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    procs.append(chrome)
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
            if "exceptionDetails" in r: logs.append("EVALERR " + json.dumps(r["exceptionDetails"])[:200])
            return r.get("result", {}).get("value")

        async def shot(name):
            r = await cmd("Page.captureScreenshot", format="png")
            open(f"{OUT}/{name}.png", "wb").write(base64.b64decode(r["data"]))

        async def wait_for(js, timeout=15):
            t0 = time.time()
            while time.time() - t0 < timeout:
                if await ev(js): return True
                await asyncio.sleep(0.25)
            return False

        ids = "[...document.querySelectorAll('#view-memory .vm-card')].map(c=>+c.dataset.id)"
        await cmd("Runtime.enable"); await cmd("Page.enable")
        await cmd("Page.navigate", url=BASE + "#memory")
        check("gallery renders the 4 stored frames", await wait_for("document.querySelectorAll('#view-memory .vm-card').length===4"))
        first_ids = await ev(ids)
        check("gallery order (DOM) equals the direct SQL query", first_ids == [i for i, _ in db_newest(10)], (first_ids, db_newest(10)))
        check("thumbnails decode in the browser", await wait_for("[...document.querySelectorAll('#view-memory .vm-card img')].every(i=>i.complete&&i.naturalWidth>0)"))
        # tag the existing top card: a merge must keep this very element
        await ev("(()=>{window.__top=document.querySelector('#view-memory .vm-card');window.__top.__tag='kept';return 1})()")
        await shot("1_initial")

        # ---- frames written while the page is open: appear automatically, no reload, existing cards kept ---
        w = writer(3, 400)
        ok = await wait_for("document.querySelectorAll('#view-memory .vm-card').length===7", 25)
        w.wait()
        now_ids = await ev(ids)
        check("3 frames inserted while the gallery was open appear without a reload", ok, now_ids)
        check("the new frames are at the top and in newest-first order", now_ids == sorted(now_ids, reverse=True) and now_ids[0] > first_ids[0], now_ids)
        check("gallery top equals the direct SQL newest again", now_ids[:7] == [i for i, _ in db_newest(7)])
        check("existing cards were merged in place (same DOM element kept)", await ev("window.__top && window.__top.__tag==='kept' && document.contains(window.__top)"))
        check("no 'newer frames' notice while following the newest page", await ev("!document.querySelector('#view-memory .vm-live .vm-banner')"))
        await shot("2_live_update")

        # ---- the user looks at 'oldest first': the view must not move; a notice offers the new frames ---
        await ev("(()=>{const s=document.querySelector('#view-memory select[aria-label=\"Order\"]');s.value='asc';s.dispatchEvent(new Event('change'));return 1})()")
        await wait_for("(()=>{const c=[...document.querySelectorAll('#view-memory .vm-card')].map(c=>+c.dataset.id);return c.length&&c[0]===%d})()" % min(now_ids), 10)
        asc_before = await ev(ids)
        w = writer(2, 300); w.wait()
        ok = await wait_for("!!document.querySelector('#view-memory .vm-live .vm-banner.info')", 25)
        check("not following (oldest first): a 'newer frames' notice appears", ok)
        check("...and the visible page is left exactly as the user had it", (await ev(ids)) == asc_before)
        await ev("document.querySelector('#view-memory .vm-live .vm-banner.info button').click()")
        ok = await wait_for("(()=>{const c=[...document.querySelectorAll('#view-memory .vm-card')].map(c=>+c.dataset.id);return c.length&&c[0]===%d&&!document.querySelector('#view-memory .vm-live .vm-banner')})()" % db_newest(1)[0][0], 10)
        check("'Show newest' returns to newest-first with the latest frame on top", ok, await ev(ids))
        await shot("3_show_newest")

        # ---- ingestion stall: no new frame for > 30 s => clear warning, red pill; recovery clears it --------
        print("waiting for the stall threshold (31 s without a new frame)...", flush=True)
        ok = await wait_for("!!document.querySelector('#view-memory .vm-live .vm-banner.bad')", 50)
        check("ingestion stalled: the gallery shows a clear warning", ok)
        txt = await ev("document.querySelector('#view-memory .vm-live .vm-banner.bad')?.innerText||''")
        check("the warning says no new frame is being stored", "stopped" in txt and "no new frame" in txt, txt)
        check("health pill turns red", await ev("document.getElementById('vmPill').className.includes('vm-red')"), await ev("document.getElementById('vmPill').className"))
        st, _ = get("/api/vm/status")
        check("status API: ingest.state == stalled", st["ingest"]["state"] == "stalled", st["ingest"])
        check("the Stage-2-disabled notice is shown (embeddings are not configured here)", "Stage 2 is disabled" in await ev("document.getElementById('view-memory').innerText"))
        await shot("4_stalled")
        top_before = (await ev(ids))[0]
        w = writer(1, 100); w.wait()
        ok = await wait_for("!document.querySelector('#view-memory .vm-live .vm-banner.bad')&&+document.querySelector('#view-memory .vm-card').dataset.id>%d" % top_before, 25)
        check("recovery: a new frame clears the warning and appears in the gallery", ok)
        check("health pill leaves red after recovery", await ev("!document.getElementById('vmPill').className.includes('vm-red')"))
        await shot("5_recovered")

    snap_stop.set()


try:
    asyncio.run(main())
except BaseException:
    traceback.print_exc(); results.append(False); print("FAIL test aborted by an exception (see above)")
finally:
    snap_stop.set()
    for p in procs:
        try: p.send_signal(signal.SIGTERM)
        except Exception: pass
    time.sleep(0.5)
    print("console/exception log: " + ("; ".join(logs) or "none"))
    check("no console errors or exceptions", not logs, logs)
    shutil.rmtree(tmp, ignore_errors=True)
    print("RESULT %d/%d passed" % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)
