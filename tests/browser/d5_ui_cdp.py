"""Real-browser (Chrome DevTools protocol) test of the D5 Proof-of-Play page against a dashboard URL that already holds sessions.

  d5_ui_cdp.py BASE OUTDIR [--expect-status status ...]

Checks: the page renders the registry/history, status chips carry the verdict text, the detail modal shows screenshot and
reference images that really loaded (naturalWidth > 0), the CSV link is present, no console errors, the phone layout has no
horizontal page scroll, and the old "not implemented" placeholder is gone. Saves screenshots in OUTDIR (keep them out of Git
when they show people).
"""
import asyncio, base64, json, os, subprocess, sys, time, urllib.request, websockets

BASE = sys.argv[1].rstrip("/") + "/"
OUT = sys.argv[2]; os.makedirs(OUT, exist_ok=True)
EXPECT = sys.argv[sys.argv.index("--expect-status") + 1:] if "--expect-status" in sys.argv else []
PORT = 9362
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

        async def wait_for(js, timeout=12):
            t0 = time.time()
            while time.time() - t0 < timeout:
                if await ev(js): return True
                await asyncio.sleep(0.2)
            return False

        await cmd("Runtime.enable"); await cmd("Page.enable")
        await cmd("Emulation.setDeviceMetricsOverride", width=1440, height=1000, deviceScaleFactor=1, mobile=False)
        await cmd("Page.navigate", url=BASE + "#pop")
        ok = await wait_for("document.querySelectorAll('#view-pop .pop-row').length>0")
        check("history rows rendered", ok)
        txt = await ev("document.getElementById('view-pop').innerText")
        check("placeholder text is gone", "Not implemented yet" not in txt and "placeholder" not in txt.lower(), txt[:200])
        check("states the evidence is display output, not the camera", "presentation output" in txt.lower() or "display-output" in txt.lower() or "display output" in txt.lower())
        check("tab label has no 'preview' badge", "preview" not in (await ev("document.querySelector('#tabs a[data-tab=pop]').innerText")).lower())
        chips = await ev("[...document.querySelectorAll('#view-pop .pop-row .pop-chip')].map(c=>c.textContent)")
        print("status chips:", chips)
        for want in EXPECT:
            check(f"history shows status '{want}'", want in chips, chips)
        check("registry lists media with reference counts", "Reference frames" in txt and await ev("document.querySelectorAll('#view-pop .pop-table')[1] && document.querySelectorAll('#view-pop .pop-table')[1].querySelectorAll('tbody tr').length>0"))
        check("CSV export link", await ev("!!document.querySelector('#view-pop a[href*=\"export.csv\"]')"))
        await asyncio.sleep(0.5); await shot("D5_1_history")

        # status filter
        if EXPECT:
            await ev("(()=>{const s=document.querySelector('#view-pop select[aria-label=\"Filter by status\"]');s.value='%s';s.dispatchEvent(new Event('change'));return 1})()" % ("mismatch" if "Mismatch" in chips else "playback_corroborated"))
            await wait_for("document.querySelectorAll('#view-pop .pop-row').length < %d" % len(chips), 10)     # a real player answers in ~1 s
            rows = await ev("document.querySelectorAll('#view-pop .pop-row').length")
            check("status filter narrows the list", 0 < rows < len(chips), (rows, len(chips)))
            await ev("(()=>{const s=document.querySelector('#view-pop select[aria-label=\"Filter by status\"]');s.value='';s.dispatchEvent(new Event('change'));return 1})()")
            await asyncio.sleep(1.2)

        # open the first corroborated (or first) session
        await ev("(()=>{const rows=[...document.querySelectorAll('#view-pop .pop-row')];const r=rows.find(x=>x.innerText.includes('Playback corroborated'))||rows[0];r.click();return 1})()")
        ok = await wait_for("!!document.querySelector('.vm-modal .pop-ev')", 10)
        check("detail modal with evidence cards opened", ok)
        await asyncio.sleep(2.0)
        imgs = await ev("[...document.querySelectorAll('.vm-modal img.pop-img')].map(i=>[i.naturalWidth,i.complete])")
        check("screenshot and reference images loaded", len(imgs) >= 2 and all(w > 0 and c for w, c in imgs), imgs)
        body = await ev("document.querySelector('.vm-modal').innerText")
        check("detail shows similarity, reference offset and events", "similarity" in body and "Closest reference" in body and "play_started" in body, body[:300])
        check("detail shows re-verify and export", "Re-verify" in body and "Export JSON" in body)
        await shot("D5_2_detail")
        await ev("document.querySelector('.vm-modal-head button').click()")
        await asyncio.sleep(0.4)

        # phone layout
        await cmd("Emulation.setDeviceMetricsOverride", width=390, height=800, deviceScaleFactor=2, mobile=True)
        await cmd("Page.navigate", url=BASE + "#pop"); await asyncio.sleep(2.5)
        sw = await ev("[document.documentElement.scrollWidth, window.innerWidth]")
        check("phone layout: no horizontal page scroll", sw[0] <= sw[1] + 1, sw)
        await shot("D5_3_phone")

        # other tabs still work
        await cmd("Emulation.setDeviceMetricsOverride", width=1440, height=1000, deviceScaleFactor=1, mobile=False)
        for tab in ("memory", "search"):
            await cmd("Page.navigate", url=BASE + "#" + tab); await asyncio.sleep(1.5)
            vis = await ev(f"!document.getElementById('view-{tab}').hidden")
            check(f"{tab} view still renders", vis)

try:
    asyncio.run(main())
finally:
    chrome.terminate()
    print("console/exception log: " + ("; ".join(logs) or "none"))
    check("no console errors or exceptions", not logs, logs)
    print("RESULT %d/%d passed" % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)
