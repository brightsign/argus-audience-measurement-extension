"""D4 completion scenario in a real browser: select a coffee cup in a stored frame -> save -> select it in another frame -> find similar -> compare.
usage: demoshots.py BASE OUTDIR"""
import asyncio, json, subprocess, time, base64, urllib.request, sys, os, websockets

BASE = sys.argv[1].rstrip("/") + "/"
OUT = sys.argv[2]; os.makedirs(OUT, exist_ok=True)
PORT = 9352
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

        async def mouse(t, x, y):
            await cmd("Input.dispatchMouseEvent", type=t, x=x, y=y, button="left", buttons=1 if t != "mouseReleased" else 0, clickCount=1)

        async def wait_for(js, timeout=10):
            t0 = time.time()
            while time.time() - t0 < timeout:
                if await ev(js): return True
                await asyncio.sleep(0.2)
            return False

        async def select(fid, roi, label=None):
            await ev("document.querySelectorAll('.vm-modal-head button').forEach(b=>b.click()), 1")
            await cmd("Page.navigate", url=BASE + "#memory"); await asyncio.sleep(1.5)
            await ev(f"(window.VMUI.openDetail({fid}), 1)")
            await wait_for("[...document.querySelectorAll('.vm-modal button')].some(b=>b.textContent==='Select Object')")
            await ev("[...document.querySelectorAll('.vm-modal button')].find(b=>b.textContent==='Select Object').click(), 1")
            await wait_for("document.querySelector('.vm-roi-stage img') && document.querySelector('.vm-roi-stage img').naturalWidth>0")
            await asyncio.sleep(0.4)
            r = await ev("(()=>{const s=document.querySelector('.vm-roi-stage').getBoundingClientRect();const i=document.querySelector('.vm-roi-stage img');return {x:s.left,y:s.top,w:s.width,h:s.height,nw:i.naturalWidth,nh:i.naturalHeight}})()")
            sc = min(r["w"] / r["nw"], r["h"] / r["nh"]); ox, oy = (r["w"] - r["nw"] * sc) / 2, (r["h"] - r["nh"] * sc) / 2
            x0, y0, x1, y1 = roi[0], roi[1], roi[0] + roi[2], roi[1] + roi[3]
            sx0, sy0 = r["x"] + ox + x0 * sc, r["y"] + oy + y0 * sc; sx1, sy1 = r["x"] + ox + x1 * sc, r["y"] + oy + y1 * sc
            await mouse("mousePressed", sx0, sy0)
            for k in range(1, 8): await mouse("mouseMoved", sx0 + (sx1 - sx0) * k / 7, sy0 + (sy1 - sy0) * k / 7)
            await mouse("mouseReleased", sx1, sy1); await asyncio.sleep(1.5)
            vals = await ev("[...document.querySelectorAll('.vm-roi-layout input[type=number]')].map(i=>Number(i.value))")
            check(f"frame {fid}: drag gives ROI {roi} (got {vals})", all(abs(a - b) <= 3 for a, b in zip(vals, roi)), vals)
            if label:
                await ev("(()=>{const l=document.querySelector('.vm-roi-layout input[type=text]');l.value=%s;l.dispatchEvent(new Event('input'));return 1})()" % json.dumps(label))
            return vals

        await cmd("Runtime.enable"); await cmd("Page.enable")
        await cmd("Emulation.setDeviceMetricsOverride", width=1440, height=1000, deviceScaleFactor=1, mobile=False)
        await select(259, (185, 4, 255, 296), "coffee cup")
        await ev("[...document.querySelectorAll('.vm-modal button')].find(b=>b.textContent==='Save Object to Index').click(), 1")
        ok = await wait_for("/Saved as object|Already indexed/.test(document.querySelector('.vm-roi-layout').innerText)", 20)
        check("save coffee cup to index", ok)
        await asyncio.sleep(0.8); await shot("D4_1_select_and_save_cup")
        await select(304, (205, 4, 250, 292))
        await asyncio.sleep(0.8); await shot("D4_2_select_cup_in_other_frame")
        await ev("[...document.querySelectorAll('.vm-modal button')].find(b=>b.textContent==='Find Similar Objects').click(), 1")
        ok = await wait_for("document.querySelectorAll('#view-search .vm-grid')[0] && document.querySelectorAll('#view-search .vm-grid')[0].querySelectorAll('.vm-card').length>=1", 25)
        check("object search results rendered", ok)
        await asyncio.sleep(2.0); await shot("D4_3_object_search_results")
        top = await ev("document.querySelectorAll('#view-search .vm-grid')[0].querySelector('.vm-card').innerText")
        print("TOP CARD:", (top or "").replace("\n", " | "))
        check("top-1 is the indexed coffee cup", "coffee cup" in (top or ""), top)
        await ev("document.querySelectorAll('#view-search .vm-grid')[0].querySelector('.vm-card').click(), 1")
        await wait_for("!!document.querySelector('.vm-modal .vm-roi-frame .vm-roi-box')"); await asyncio.sleep(1.5)
        await shot("D4_4_compare_with_bounding_box")

try:
    asyncio.run(main())
finally:
    chrome.terminate()
    print("console/exception log: " + ("; ".join(logs) or "none"))
    print("RESULT %d/%d passed" % (sum(results), len(results)))
