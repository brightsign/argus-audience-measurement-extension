#!/usr/bin/env python3
"""Live demonstration of Visual Search -> Object search on a REAL player (headless Chrome over CDP; HOST script).

  d4_display_objects_live_cdp.py http://PLAYER:20300 SESSION_ID MANIFEST.json OUTDIR

Opens the player's own dashboard, switches to "BrightSign Display (Weston/VOP2)", shows the indexed bottle/cup counts, selects the bottle (then the cup) in a stored
frame of the product-video session by typing the ground-truth box of the generated video, runs Find Similar Objects, opens the comparison dialog and the Proof-of-Play
session link, then returns to "Audience Camera" and repeats a camera search. Screenshots go to OUTDIR. Read-only: the box search writes nothing, nothing is indexed.
"""
import asyncio, base64, importlib.util, json, math, os, signal, subprocess, sys, time, urllib.request, websockets

API, SID, MAN, OUT = sys.argv[1].rstrip("/"), sys.argv[2], sys.argv[3], sys.argv[4]
os.makedirs(OUT, exist_ok=True)
CDP_PORT = 9622
spec = importlib.util.spec_from_file_location("hwmod", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "api", "vm_objects_display_hw.py")); hw = importlib.util.module_from_spec(spec); spec.loader.exec_module(hw)
man = json.load(open(MAN))
results, logs, procs = [], [], []


def check(name, cond, extra=""):
    results.append(bool(cond)); print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(extra)[:400]) if not cond and extra != "" else ""), flush=True)


def frames():
    """The newest stored Weston frames of the product-video advertisement (the picker shows the newest 48)."""
    d = json.load(urllib.request.urlopen(API + "/api/vm/display-frames?limit=48", timeout=20))
    return [f for f in d["frames"] if f.get("expected_media_id") == "ad-c" and f.get("reported_media_id") == "ad-c"]


async def main():
    fr = [f for f in frames() if f["kind"] == "history" and f.get("playback_offset_ms")]
    ok = [f for f in fr if 4.0 <= f["playback_offset_ms"] / 1000.0 <= 8.0 and f.get("image_available") and urllib.request.urlopen(API + f["image_url"], timeout=20).status == 200 if True]
    pick = ok[0]; t = pick["playback_offset_ms"] / 1000.0
    boxes = {"bottle": hw.gt_box(man, hw.scene_of(t), "water bottle", t), "cup": hw.gt_box(man, hw.scene_of(t), "coffee cup", t)}
    print("frame", pick["id"], "offset %.2f s" % t, {k: [round(x) for x in v] for k, v in boxes.items()})
    chrome = subprocess.Popen(["/usr/bin/google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu", f"--remote-debugging-port={CDP_PORT}", f"--user-data-dir={OUT}/profile", "--window-size=1440,2400", "about:blank"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); procs.append(chrome); time.sleep(3)
    tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{CDP_PORT}/json"))
    ws_url = [x for x in tabs if x["type"] == "page"][0]["webSocketDebuggerUrl"]
    async with websockets.connect(ws_url, max_size=None) as ws:
        n = 0; pend = {}

        async def reader():
            async for m in ws:
                dd = json.loads(m)
                if "id" in dd and dd["id"] in pend: pend[dd["id"]].set_result(dd)
                elif dd.get("method") == "Runtime.exceptionThrown": logs.append("EXC " + json.dumps(dd["params"]["exceptionDetails"])[:300])
                elif dd.get("method") == "Runtime.consoleAPICalled" and dd["params"]["type"] == "error": logs.append("CONSOLE " + json.dumps([a.get("value", a.get("description")) for a in dd["params"]["args"]])[:300])
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

        async def wait_for(js, timeout=20):
            t0 = time.time()
            while time.time() - t0 < timeout:
                if await ev(js): return True
                await asyncio.sleep(0.3)
            return False

        async def setv(sel, v):
            await ev("(()=>{const e=document.querySelector(%s);e.value=%s;e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new Event('change',{bubbles:true}));return 1})()" % (json.dumps(sel), json.dumps(str(v))))

        P = "document.querySelector('[data-role=auto-panel]')"
        SRC = "[...document.querySelectorAll('#view-search [aria-label=\"Search source\"] button')]"
        await cmd("Page.enable"); await cmd("Runtime.enable")
        await cmd("Emulation.setDeviceMetricsOverride", width=1440, height=2400, deviceScaleFactor=1, mobile=False)
        await cmd("Page.navigate", url=API + "/?t=%d#search?mode=objects" % int(time.time() * 1000))
        check("Object search opens on the Audience Camera source (default)", await wait_for(f"!!{P} && /Automatic object indexing/i.test({P}.innerText)"))
        await shot("01_camera_object_search")
        await ev(f"{SRC}.find(b=>/BrightSign Display/.test(b.textContent)).click(), 1")
        check("BrightSign Display selected: display panel with the indexed totals", await wait_for(f"/Automatic object indexing: BrightSign display/i.test({P}.innerText) && /indexed display objects/.test((document.querySelector('[data-role=object-totals]')||{{innerText:''}}).innerText)"),
              await ev(f"({P}||document.body).innerText.slice(0,300)"))
        totals = await ev("(document.querySelector('[data-role=object-totals]')||{}).innerText"); chips = await ev("[...document.querySelectorAll('[data-role=class-chips] button')].map(b=>b.textContent).join('|')")
        print("   totals:", totals, "| classes:", chips)
        check("class chips list bottle and cup", "bottle" in (chips or "") and "cup" in (chips or ""), chips)
        check("the page states that only indexed objects are searched", "not every stored frame" in (await ev("document.getElementById('view-search').innerText")))
        await shot("02_display_indexed_counts")
        for cls in ("bottle", "cup"):
            x, y, w, h = [int(round(v)) for v in boxes[cls]]
            await ev("[...document.querySelectorAll('#view-search button')].find(b=>/Select object in a stored display frame/.test(b.textContent)).click(), 1")
            check(f"[{cls}] the stored-frame picker opens", await wait_for("!!document.querySelector('.vm-modal [data-role=session-filter]')"))
            check(f"[{cls}] the picker lists the stored Ad C frames", await wait_for("document.querySelectorAll('.vm-modal [data-frame-kind]').length>=20"), await ev("document.querySelectorAll('.vm-modal [data-frame-kind]').length"))
            expired = await ev("[...document.querySelectorAll('.vm-modal [data-frame-kind]')].filter(t=>/JPEG expired/.test(t.innerText)).length")
            check(f"[{cls}] the picker shows no 'JPEG expired' tile (every retained frame, incl. checkpoint-linked ones, has its image)", expired == 0, expired)
            check(f"[{cls}] every picker thumbnail loaded", await wait_for("[...document.querySelectorAll('.vm-modal [data-frame-kind] img')].every(i=>i.complete&&i.naturalWidth===640)"))
            await shot(f"03_{cls}_frame_picker")
            await ev("document.querySelector('.vm-modal [data-frame-kind=history][data-frame-id=\"%d\"]').click(), 1" % pick["id"])
            check(f"[{cls}] the frame opens in the object selector", await wait_for("!!document.querySelector('.vm-roi-stage img') && document.querySelector('.vm-roi-stage img').complete && document.querySelector('.vm-roi-stage img').naturalWidth===640"))
            for k, v in (("X", x), ("Y", y), ("W", w), ("H", h)): await setv(f".vm-modal input[aria-label={k}]", v)
            check(f"[{cls}] the crop preview of the box loads", await wait_for("(()=>{const i=document.querySelector('.vm-roi-prev img');return !!i&&i.complete&&i.naturalWidth>8})()"))
            await shot(f"04_{cls}_selected_box")
            await ev("[...document.querySelectorAll('.vm-modal button')].find(b=>b.textContent==='Find Similar Objects').click(), 1")
            ok = await wait_for("(()=>{const g=document.querySelector('[data-role=result-grid]');return !!g&&g.querySelectorAll('.vm-card').length>=5&&[...g.querySelectorAll('.vm-card')].every(c=>c.dataset.source==='display')})()", 40)
            check(f"[{cls}] results: display objects only", ok, await ev("(document.querySelector('[data-role=search-scope]')||{}).innerText"))
            cards = await ev("[...document.querySelectorAll('[data-role=result-grid] .vm-card')].slice(0,8).map(c=>c.innerText.replace(/\\n+/g,' | '))")
            for c in (cards or [])[:4]: print("   card:", c[:220])
            check(f"[{cls}] cards show class, confidence, similarity, ad, offset, session", bool(cards) and all(("into the play" in c and "session s-" in c) for c in cards[:6]), cards[:1])
            await shot(f"05_{cls}_results")
            await ev("document.querySelector('[data-role=result-grid] .vm-card').click(), 1")
            check(f"[{cls}] compare dialog: source frame with its box, ad, offset, session link", await wait_for("(()=>{const m=document.querySelector('.vm-modal');return !!m&&!!m.querySelector('[data-role=session-link]')&&!!m.querySelector('.vm-roi-frame img')&&m.querySelector('.vm-roi-frame img').complete&&m.querySelector('.vm-roi-frame img').naturalWidth===640})()"))
            await shot(f"06_{cls}_compare")
            href = await ev("document.querySelector('.vm-modal [data-role=session-link]').getAttribute('href')"); check(f"[{cls}] the session link points at a Proof-of-Play session", bool(href) and href.startswith("#pop?session="), href)
            if cls == "bottle":
                await ev("document.querySelector('.vm-modal [data-role=session-link]').click(), 1")
                check("[bottle] the link opens the session in Proof of Play", await wait_for("location.hash.startsWith('#pop')", 20), await ev("location.hash")); await asyncio.sleep(3); await shot("07_pop_session")
                await cmd("Page.navigate", url=API + "/?t=%d#search?mode=objects" % int(time.time() * 1000)); await wait_for(f"!!{P}")
                await ev(f"{SRC}.find(b=>/BrightSign Display/.test(b.textContent)).click(), 1"); await wait_for("/indexed display objects/.test((document.querySelector('[data-role=object-totals]')||{innerText:''}).innerText)")
            else:
                await ev("(document.querySelector('.vm-modal [aria-label=Close],.vm-modal .vm-close')||{click(){}}).click(), 1")
        # back to the camera source
        await cmd("Page.navigate", url=API + "/?t=%d#search?mode=objects" % int(time.time() * 1000)); await wait_for(f"!!{P}")
        cam = await ev("(document.querySelector('[data-role=object-totals]')||{}).innerText")
        check("Audience Camera: camera totals only, no display objects", bool(cam) and "display" not in cam.lower(), cam)
        await wait_for("document.querySelectorAll('#view-search .vm-card').length>=3")
        await ev("document.querySelector('#view-search .vm-card').click(), 1")
        check("clicking a camera object card searches from it: camera scope, camera results only", await wait_for("(()=>{const g=document.querySelector('[data-role=result-grid]');const sc=document.querySelector('[data-role=search-scope]');return !!g&&g.querySelectorAll('.vm-card').length>=3&&!!sc&&/camera objects/.test(sc.innerText)&&![...g.querySelectorAll('.vm-card')].some(c=>/display frame/.test(c.innerText))})()"))
        await shot("08_camera_object_detail")
        check("no console errors", not [l for l in logs if "favicon" not in l], logs)
    for p in procs[::-1]:
        try: p.send_signal(signal.SIGTERM); p.wait(timeout=5)
        except Exception: p.kill()
    print("RESULT %d/%d passed" % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)

try: asyncio.run(main())
except Exception:
    import traceback; traceback.print_exc()
    for p in procs[::-1]:
        try: p.kill()
        except Exception: pass
    sys.exit(2)
