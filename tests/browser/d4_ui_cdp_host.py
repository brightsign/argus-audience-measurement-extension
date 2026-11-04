"""Real-browser (Chrome CDP) test of the D4 UI against a dashboard URL. usage: uitest.py BASE OUTDIR [--shots]"""
import asyncio, json, subprocess, time, base64, urllib.request, sys, os, websockets

BASE = sys.argv[1].rstrip("/") + "/"
OUT = sys.argv[2]; os.makedirs(OUT, exist_ok=True)
PORT = 9351
chrome = subprocess.Popen(["/usr/bin/google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu", f"--remote-debugging-port={PORT}",
                           f"--user-data-dir={OUT}/profile", "--window-size=1440,1000", "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2.5)
tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json"))
ws_url = [t for t in tabs if t["type"] == "page"][0]["webSocketDebuggerUrl"]
logs, results = [], []


def check(name, cond, extra=""):
    results.append(bool(cond)); print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(extra)) if not cond and extra != "" else ""))


async def main():
    async with websockets.connect(ws_url, max_size=None) as ws:
        n = 0; pend = {}

        async def reader():
            async for m in ws:
                d = json.loads(m)
                if "id" in d and d["id"] in pend: pend[d["id"]].set_result(d)
                elif d.get("method") == "Runtime.exceptionThrown": logs.append("EXC " + json.dumps(d["params"]["exceptionDetails"])[:300])
                elif d.get("method") == "Runtime.consoleAPICalled" and d["params"]["type"] == "error":
                    logs.append("CONSOLE " + json.dumps([a.get("value", a.get("description")) for a in d["params"]["args"]])[:300])
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

        async def mouse(t, x, y, **k):
            await cmd("Input.dispatchMouseEvent", type=t, x=x, y=y, button="left", buttons=1 if t != "mouseReleased" else 0, clickCount=1, **k)

        async def wait_for(js, timeout=8):
            t0 = time.time()
            while time.time() - t0 < timeout:
                if await ev(js): return True
                await asyncio.sleep(0.15)
            return False

        await cmd("Runtime.enable"); await cmd("Page.enable")
        await cmd("Emulation.setDeviceMetricsOverride", width=1440, height=1000, deviceScaleFactor=1, mobile=False)
        await cmd("Page.navigate", url=BASE + "#search?mode=objects"); await asyncio.sleep(2.0)

        # ---- empty-index state -------------------------------------------------------------------------
        txt = await ev("document.getElementById('view-search').innerText")
        check("object mode shows 'index is empty' guidance", "object index is empty" in (txt or "").lower(), (txt or "")[:200])
        check("mode switch present", "Full frame search" in txt and "Object search" in txt)
        await shot("01_object_search_empty")

        # ---- gallery -> frame detail -> Select Object ---------------------------------------------------
        frames = await ev("fetch('/api/vm/frames?limit=100').then(r=>r.json()).then(d=>d.frames)")
        f43 = [f for f in frames if f["width"] == 640 and f["height"] == 480][0]
        f169 = [f for f in frames if f["width"] == 640 and f["height"] == 360][0]
        for label, frame in (("16:9", f169), ("4:3 (pillarboxed)", f43)):
            await cmd("Page.navigate", url=BASE + "#memory"); await asyncio.sleep(1.2)
            await ev(f"(window.VMUI.openDetail({frame['id']}), 1)")
            ok = await wait_for("[...document.querySelectorAll('.vm-modal button')].some(b=>b.textContent==='Select Object')")
            check(f"[{label}] detail has Select Object button", ok)
            await ev("[...document.querySelectorAll('.vm-modal button')].find(b=>b.textContent==='Select Object').click(), 1")
            ok = await wait_for("!!document.querySelector('.vm-roi-stage img') && document.querySelector('.vm-roi-stage img').complete && document.querySelector('.vm-roi-stage img').naturalWidth>0")
            check(f"[{label}] selector stage + image loaded", ok)
            await asyncio.sleep(0.3)
            r = await ev("(()=>{const s=document.querySelector('.vm-roi-stage').getBoundingClientRect();const i=document.querySelector('.vm-roi-stage img');return {x:s.left,y:s.top,w:s.width,h:s.height,nw:i.naturalWidth,nh:i.naturalHeight}})()")
            # expected fit
            sc = min(r["w"] / r["nw"], r["h"] / r["nh"]); fw, fh = r["nw"] * sc, r["nh"] * sc; ox, oy = (r["w"] - fw) / 2, (r["h"] - fh) / 2
            # drag from image pixel (100,60) to (300,200)
            ip0, ip1 = (100, 60), (300, 200)
            sx0, sy0 = r["x"] + ox + ip0[0] * sc, r["y"] + oy + ip0[1] * sc
            sx1, sy1 = r["x"] + ox + ip1[0] * sc, r["y"] + oy + ip1[1] * sc
            await mouse("mousePressed", sx0, sy0)
            for k in range(1, 6): await mouse("mouseMoved", sx0 + (sx1 - sx0) * k / 5, sy0 + (sy1 - sy0) * k / 5)
            await mouse("mouseReleased", sx1, sy1)
            await asyncio.sleep(0.5)
            vals = await ev("[...document.querySelectorAll('.vm-roi-layout input[type=number]')].map(i=>Number(i.value))")
            ex = [ip0[0], ip0[1], ip1[0] - ip0[0], ip1[1] - ip0[1]]
            check(f"[{label}] drag -> image coords {ex} (got {vals})", all(abs(a - b) <= 2 for a, b in zip(vals, ex)), vals)
            prev = await ev("(()=>{const i=document.querySelector('.vm-roi-prev img');return i&&i.complete?[i.naturalWidth,i.naturalHeight]:null})()")
            check(f"[{label}] server crop preview loaded", prev is not None and prev[0] > 20, prev)
            # drag that starts in the letterbox bar / outside is clamped; tiny selection rejected
            tiny = await ev("(()=>{const f=[...document.querySelectorAll('.vm-roi-layout input[type=number]')];f[2].value=8;f[2].dispatchEvent(new Event('input'));return [...document.querySelectorAll('.vm-modal button')].filter(b=>/Find Similar|Save Object/.test(b.textContent)).map(b=>b.disabled)})()")
            check(f"[{label}] tiny width disables actions", tiny == [True, True], tiny)
            await ev("(()=>{const f=[...document.querySelectorAll('.vm-roi-layout input[type=number]')];f[2].value=200;f[2].dispatchEvent(new Event('input'));return 1})()")
            if label.startswith("4:3"):
                await shot("02_selector_43_pillarbox")
            else:
                await shot("02_selector_169")
            # label + save (only for the 16:9 frame; 4:3 will be searched)
            if label.startswith("4:3"):
                await ev("document.querySelector('.vm-modal-head button').click(), 1")
            if label == "16:9":
                await ev("(()=>{const l=document.querySelector('.vm-roi-layout input[type=text]');l.value='sample';l.dispatchEvent(new Event('input'));return 1})()")
                await ev("[...document.querySelectorAll('.vm-modal button')].find(b=>b.textContent==='Save Object to Index').click(), 1")
                ok = await wait_for("/Saved as object #\\d+/.test(document.querySelector('.vm-roi-layout').innerText)")
                check("save object -> confirmation text", ok)
                await shot("03_saved")
                await ev("document.querySelector('.vm-modal-head button').click(), 1")
        # ---- run object search from the 4:3 selection -----------------------------------------------------
        await ev("(()=>{const f=[...document.querySelectorAll('.vm-modal button')];return 1})()")
        await cmd("Page.navigate", url=BASE + "#memory"); await asyncio.sleep(1.0)
        # index three more objects through the UI endpoint (same as pressing Save)
        for i, (x, y, w, h) in enumerate([(60, 40, 220, 160), (300, 120, 240, 200), (20, 100, 300, 200)]):
            st = await ev(f"fetch('/api/vm/objects',{{method:'POST',headers:{{'Content-Type':'application/json'}},body:JSON.stringify({{frame_id:{frames[3+i]['id']},x:{x},y:{y},w:{w},h:{h},label:'obj{i}'}})}}).then(r=>r.status)")
            check(f"index extra object {i} via API from page context (same-origin)", st == 201, st)
        await cmd("Page.navigate", url=BASE + "#search?mode=objects"); await asyncio.sleep(2.0)
        await ev("window.VMObjects.searchFromRoi({id:%d,width:640,height:480}, {x:100,y:60,w:200,h:140}), 1" % f43["id"])
        ok = await wait_for("document.querySelectorAll('#view-search .vm-grid')[0] && document.querySelectorAll('#view-search .vm-grid')[0].querySelectorAll('.vm-card').length>=4")
        check("object search renders result cards", ok)
        info = await ev("document.getElementById('view-search').innerText")
        check("results show cosine disclaimer and timing", "not calibrated" in info.lower() or "not identification" in info.lower(), info[:300])
        sims = await ev("[...document.querySelectorAll('#view-search .vm-grid')[0].querySelectorAll('.vm-score')].map(e=>parseFloat(e.textContent))")
        check("similarities sorted descending", sims == sorted(sims, reverse=True) and len(sims) >= 4, sims)
        await shot("04_object_search_results")
        await ev("document.querySelector('#view-search .vm-grid .vm-card').click(), 1")
        ok = await wait_for("!!document.querySelector('.vm-modal .vm-roi-frame .vm-roi-box')")
        check("compare modal shows bounding box overlay", ok)
        box = await ev("(()=>{const f=document.querySelector('.vm-modal .vm-roi-frame').getBoundingClientRect();const b=document.querySelector('.vm-modal .vm-roi-box').getBoundingClientRect();return {fw:f.width,fh:f.height,bx:b.left-f.left,by:b.top-f.top,bw:b.width,bh:b.height,ar:getComputedStyle(document.querySelector('.vm-modal .vm-roi-frame')).aspectRatio}})()")
        top = (await ev("fetch('/api/vm/objects/search',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({frame_id:%d,x:100,y:60,w:200,h:140})}).then(r=>r.json())" % f43["id"]))["results"][0]
        W, H = top["image_width"], top["image_height"]; b = top["bbox"]
        exp = [b["x"] / W * box["fw"], b["y"] / H * box["fh"], b["w"] / W * box["fw"], b["h"] / H * box["fh"]]
        got = [box["bx"], box["by"], box["bw"], box["bh"]]
        check("overlay box position/size matches stored bbox (+-2px)", all(abs(a - c) <= 2.5 for a, c in zip(exp, got)), (exp, got))
        await shot("05_compare_overlay")
        await ev("document.querySelector('.vm-modal-head button').click(), 1")
        # ---- full-frame mode still works ---------------------------------------------------------------------
        await ev("[...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Full frame search').click(), 1")
        vis = await ev("!document.querySelector('#view-search .vm-drop').closest('[hidden]')")
        check("full-frame search (D3) still visible after switching back", vis)
        await shot("06_full_frame_mode")
        # ---- touch support: synthetic touch drag ------------------------------------------------------------
        await cmd("Page.navigate", url=BASE + "#memory"); await asyncio.sleep(1.0)
        await ev(f"(window.VMUI.openDetail({f169['id']}), 1)")
        await wait_for("[...document.querySelectorAll('.vm-modal button')].some(b=>b.textContent==='Select Object')")
        await ev("[...document.querySelectorAll('.vm-modal button')].find(b=>b.textContent==='Select Object').click(), 1")
        await wait_for("document.querySelector('.vm-roi-stage img') && document.querySelector('.vm-roi-stage img').naturalWidth>0")
        r = await ev("(()=>{const s=document.querySelector('.vm-roi-stage').getBoundingClientRect();return {x:s.left,y:s.top,w:s.width,h:s.height}})()")
        p = lambda fx, fy: [{"x": r["x"] + r["w"] * fx, "y": r["y"] + r["h"] * fy, "id": 1}]
        await cmd("Input.dispatchTouchEvent", type="touchStart", touchPoints=p(0.2, 0.2))
        await cmd("Input.dispatchTouchEvent", type="touchMove", touchPoints=p(0.5, 0.5))
        await cmd("Input.dispatchTouchEvent", type="touchEnd", touchPoints=[])
        await asyncio.sleep(0.4)
        vals = await ev("[...document.querySelectorAll('.vm-roi-layout input[type=number]')].map(i=>Number(i.value))")
        check("touch drag selects a region (~20%..50% of frame)", vals[2] > 100 and vals[3] > 60 and abs(vals[0] - 128) <= 4, vals)


try:
    asyncio.run(main())
finally:
    chrome.terminate()
    print("\nconsole/exception log:\n" + ("\n".join(logs) or "none"))
    print("RESULT %d/%d passed" % (sum(results), len(results)))
    sys.exit(0 if all(results) and not logs else 1)
