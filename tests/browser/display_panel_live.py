#!/usr/bin/env python3
"""Hardware viewer for the dashboard "Display output" panel (HOST tool, read-only): keeps a real headless Chrome on the audience view of a player
dashboard for N seconds, acting as the viewer whose frame requests keep the OS producer awake, and records what it saw.

  display_panel_live.py BASE OUTDIR SECONDS

Writes OUTDIR/live.json (panel text samples, decoded image size, number of frames actually received and shown, console errors) and screenshots of the
page at the start, every ~2 minutes and at the end. Screenshots show the presentation (and the audience camera if it shows people): keep OUTDIR out of Git.
"""
import asyncio, base64, json, os, subprocess, sys, time, urllib.request, websockets

BASE = sys.argv[1].rstrip("/") + "/"
OUT = sys.argv[2]; SECS = int(sys.argv[3])
os.makedirs(OUT, exist_ok=True)
PORT = 9401
chrome = subprocess.Popen(["/usr/bin/google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu", f"--remote-debugging-port={PORT}",
                           f"--user-data-dir={OUT}/profile", "--window-size=1440,1300", "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(2.5)
tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{PORT}/json"))
ws_url = [t for t in tabs if t["type"] == "page"][0]["webSocketDebuggerUrl"]
logs, samples = [], []


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
            return r.get("result", {}).get("value")

        async def shot(name):
            r = await cmd("Page.captureScreenshot", format="png")
            open(f"{OUT}/{name}.png", "wb").write(base64.b64decode(r["data"]))

        await cmd("Runtime.enable"); await cmd("Page.enable")
        await cmd("Page.addScriptToEvaluateOnNewDocument", source="performance.setResourceTimingBufferSize(200000);")
        await cmd("Page.navigate", url=BASE)
        t0 = time.time()
        for _ in range(60):
            if await ev("!document.getElementById('dispPanel').hidden"): break
            await asyncio.sleep(0.5)
        await asyncio.sleep(3)
        await shot("viewer_start")
        last_shot = time.time()
        while time.time() - t0 < SECS:
            await asyncio.sleep(10)
            s = await ev("""(()=>{const i=document.getElementById('dispImg');
              const r=performance.getEntriesByType('resource').filter(e=>e.name.includes('/api/display/frame.jpg'));
              return {t:Date.now(), panel_visible:!document.getElementById('dispPanel').hidden, img:[i.naturalWidth,i.naturalHeight,i.hidden],
                      requests:r.length, ok200:r.filter(e=>e.responseStatus===200).length, no204:r.filter(e=>e.responseStatus===204).length,
                      meta:document.getElementById('dispMeta').innerText}})()""")
            s["elapsed_s"] = round(time.time() - t0, 1)
            samples.append(s)
            if time.time() - last_shot > 120:
                await shot(f"viewer_{int(s['elapsed_s'])}s"); last_shot = time.time()
        await shot("viewer_end")

try:
    asyncio.run(main())
finally:
    chrome.terminate()
    res = {"seconds": SECS, "samples": samples, "console_errors": logs}
    if samples:
        first, last = samples[0], samples[-1]
        dt = (last["t"] - first["t"]) / 1000.0
        res["frames_received_200"] = last["ok200"] - first["ok200"]
        res["displayed_fps"] = round((last["ok200"] - first["ok200"]) / dt, 2) if dt > 0 else None
        res["requests_204_nothing_newer"] = last["no204"] - first["no204"]
    json.dump(res, open(f"{OUT}/live.json", "w"), indent=1)
    print(json.dumps({k: v for k, v in res.items() if k != "samples"}, indent=1))
