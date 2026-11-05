#!/usr/bin/env python3
"""Real-browser (Chrome CDP) test of the dashboard "Display output" panel.

  display_panel_cdp.py VM_API_SERVER DASHBOARD_SERVER ARGUS_DISPLAY_BRIDGE FAKE_RENDERED_OUTPUT_SERVER OUTDIR

Real processes: the OS-service test double (real gRPC), argus_display_bridge, vm_api_server, the Go dashboard server (serving ./dashboard)
and headless Chrome. Checks: hidden while the feature is disabled; visible with a decoded image and its metadata when enabled; follows a
change of the displayed picture; shows black/frozen signals and the stale state; polling stops when the audience view is left (the OS
producer idles); no console errors from the panel. Synthetic pictures only.
"""
import asyncio, base64, json, os, shutil, signal, subprocess, sys, tempfile, time, traceback, urllib.request, websockets
from PIL import Image, ImageDraw

API_BIN, DASH_BIN, BRIDGE_BIN, FAKE_BIN, OUT = sys.argv[1:6]
os.makedirs(OUT, exist_ok=True)
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
API_PORT, DASH_PORT, CDP_PORT = 20491, 20490, 9392
BASE = f"http://127.0.0.1:{DASH_PORT}/"
tmp = tempfile.mkdtemp(prefix="dpanel_")
results, logs, procs = [], [], []


def check(name, cond, extra=""):
    results.append(bool(cond)); print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(extra)[:240]) if not cond and extra != "" else ""), flush=True)


def start(cmd, log):
    p = subprocess.Popen(cmd, stdout=open(os.path.join(tmp, log), "w"), stderr=subprocess.STDOUT)
    procs.append(p); return p


def stop(p):
    try: p.send_signal(signal.SIGTERM); p.wait(timeout=5)
    except Exception:
        try: p.kill()
        except Exception: pass


def fstats():
    try: return json.load(open(os.path.join(tmp, "stats.json")))
    except Exception: return {"frames_produced": 0, "health_calls": 0}


def ctl(**kw):
    c = {"frame": "A", "pause": False, "black": False, "frozen": False, "unavailable": False, "clock_skew_ms": 0, "provider": 7}
    c.update(kw)
    p = os.path.join(tmp, "control.json"); json.dump(c, open(p + ".tmp", "w")); os.replace(p + ".tmp", p)


def pic(kind):
    im = Image.new("RGB", (640, 360), (0, 0, 0)); d = ImageDraw.Draw(im)
    d.rectangle([32, 0, 608, 360], fill=(30, 90, 200) if kind == "A" else (210, 60, 40))
    d.ellipse([240, 100, 400, 260], fill=(250, 250, 250))
    return im


async def main():
    os.makedirs(os.path.join(tmp, "frames"))
    pic("A").save(os.path.join(tmp, "frames", "A.jpg"), quality=88); pic("B").save(os.path.join(tmp, "frames", "B.jpg"), quality=88)
    Image.new("RGB", (640, 360), (0, 0, 0)).save(os.path.join(tmp, "frames", "BLACK.jpg"), quality=88)
    ctl()
    dsock, bsock = os.path.join(tmp, "d.sock"), os.path.join(tmp, "b.sock")
    root = os.path.join(tmp, "store"); os.makedirs(root)

    def cfg(display):
        c = {"visual_memory": {"enabled": True, "storage_root": root}}
        if display: c["visual_memory"]["display"] = {"enabled": True, "bridge_socket": bsock, "poll_ms": 100, "bridge_binary": BRIDGE_BIN,
                                                      "os_socket": "unix://" + dsock, "interest_file": os.path.join(tmp, "interest")}
        p = os.path.join(tmp, "cfg_%d.json" % display); json.dump(c, open(p, "w")); return p

    # ---- disabled
    api = start([API_BIN, "--config", cfg(False), "--port", str(API_PORT), "--provider", "mock:64", "--status-file", os.path.join(tmp, "st.json")], "api0.log")
    dash = start([DASH_BIN, "-port", str(DASH_PORT), "-dir", os.path.join(ROOT_DIR, "dashboard"), "-vm-upstream", f"127.0.0.1:{API_PORT}"], "dash.log")
    time.sleep(1.5)

    chrome = start(["/usr/bin/google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu", f"--remote-debugging-port={CDP_PORT}", f"--user-data-dir={OUT}/profile",
                    "--window-size=1440,1300", "about:blank"], "chrome.log")
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

        async def wait_for(js, timeout=12):
            t0 = time.time()
            while time.time() - t0 < timeout:
                if await ev(js): return True
                await asyncio.sleep(0.25)
            return False

        await cmd("Runtime.enable"); await cmd("Page.enable")
        await cmd("Page.navigate", url=BASE)
        await asyncio.sleep(2.5)
        check("disabled: the Display output panel is not rendered at all", await ev("document.getElementById('dispPanel').hidden === true && getComputedStyle(document.getElementById('dispPanel')).display === 'none'"))

        # ---- enabled
        stop(api)
        fake = start([FAKE_BIN, "--socket", dsock, "--frames-dir", os.path.join(tmp, "frames"), "--interest-file", os.path.join(tmp, "interest"),
                      "--control-file", os.path.join(tmp, "control.json"), "--stats-file", os.path.join(tmp, "stats.json")], "fake.log")
        time.sleep(1.0)
        api = start([API_BIN, "--config", cfg(True), "--port", str(API_PORT), "--provider", "mock:64", "--status-file", os.path.join(tmp, "st.json")], "api1.log")
        time.sleep(1.5)
        await cmd("Page.navigate", url=BASE)
        check("enabled: the panel appears", await wait_for("document.getElementById('dispPanel').hidden === false", 15))
        check("enabled: the placeholder text is gone once a frame is shown", await wait_for("getComputedStyle(document.getElementById('dispEmpty')).display === 'none'", 10))
        check("enabled: a frame is decoded and shown", await wait_for("(()=>{const i=document.getElementById('dispImg');return !i.hidden&&i.complete&&i.naturalWidth===640&&i.naturalHeight===360})()", 15))
        await wait_for("document.getElementById('dispMeta').innerText.includes('640×360')", 10)     # the metadata follows the status poll (1 s)
        meta = await ev("document.getElementById('dispMeta').innerText")
        check("metadata: health, source, rate, capture time, age, size, uncertainty, black/frozen", all(k in meta for k in ("Display health", "Weston", "Capture rate", "Captured", "Frame age", "640×360", "Time uncertainty", "Black screen", "Frozen frame")), meta)
        check("metadata: available, not black, not frozen", "available" in meta and "Black screen\nno" in meta and "Frozen frame\nno" in meta, meta)
        await asyncio.sleep(2.5)
        fps_txt = await ev("[...document.querySelectorAll('#dispMeta dt')].find(d=>d.textContent==='Capture rate').nextSibling.textContent")
        check("capture rate near 5 fps", 3.5 <= float((fps_txt or "0").split()[0]) <= 6.5, fps_txt)
        px = lambda: ev("(()=>{const i=document.getElementById('dispImg');const c=document.createElement('canvas');c.width=640;c.height=360;const x=c.getContext('2d');x.drawImage(i,0,0);const d=x.getImageData(320,60,1,1).data;return [d[0],d[1],d[2]]})()")
        a = await px()
        check("the shown picture is ad A's (blue-ish)", a[2] > 150 and a[0] < 90, a)
        await shot("1_panel_A")
        ctl(frame="B")
        ok = await wait_for("(()=>{const i=document.getElementById('dispImg');const c=document.createElement('canvas');c.width=640;c.height=360;const x=c.getContext('2d');x.drawImage(i,0,0);const d=x.getImageData(320,60,1,1).data;return d[0]>150&&d[2]<90})()", 8)
        check("a change of the displayed picture appears in the panel", ok)
        ctl(frame="BLACK", black=True)
        check("black screen is flagged", await wait_for("document.getElementById('dispMeta').innerText.includes('black for')", 8))
        check("black frame: the picture is shown (black) and no 'no frame' text overlays it", await ev("getComputedStyle(document.getElementById('dispEmpty')).display === 'none' && !document.getElementById('dispImg').hidden"))
        await shot("2_black")
        ctl(frame="A", frozen=True)
        check("frozen frame is flagged", await wait_for("document.getElementById('dispMeta').innerText.includes('frozen for')", 8))
        ctl(frame="A", pause=True)
        check("producer stopped: the panel shows the stale state", await wait_for("/stale|unavailable/.test(document.getElementById('dispMeta').innerText) && !document.getElementById('dispMeta').innerText.includes('Display health\\navailable')", 12))
        await shot("3_stale")
        ctl(frame="A")
        check("producer back: available again", await wait_for("document.getElementById('dispMeta').innerText.includes('Display health\\navailable')", 12))

        # ---- leaving the audience view stops all polling and the producer idles
        await cmd("Page.navigate", url=BASE + "#memory")
        await asyncio.sleep(9.0)
        f1 = fstats(); await asyncio.sleep(2.5); f2 = fstats()
        check("leaving the audience view: no more requests, the OS producer idles", f2["frames_produced"] == f1["frames_produced"] and f2["health_calls"] == f1["health_calls"], (f1, f2))
        await cmd("Page.navigate", url=BASE)
        check("coming back resumes the preview", await wait_for("(()=>{const i=document.getElementById('dispImg');return !i.hidden&&i.complete&&i.naturalWidth===640})()", 12))
        await shot("4_back")

try:
    asyncio.run(main())
except BaseException:
    traceback.print_exc(); results.append(False); print("FAIL test aborted by an exception")
finally:
    for p in procs: stop(p)
    time.sleep(0.3)
    panel_errors = [l for l in logs if "display" in l.lower() or "dispImg" in l or "dispPanel" in l or "EXC" in l]
    print("console log (panel-related): " + ("; ".join(panel_errors) or "none"))
    check("no console errors or exceptions from the panel", not panel_errors, panel_errors)
    shutil.rmtree(tmp, ignore_errors=True)
    print("RESULT %d/%d passed" % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)
