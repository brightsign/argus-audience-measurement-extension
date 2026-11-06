#!/usr/bin/env python3
"""Real-browser (Chrome CDP) test of the Proof of Play demonstration on the dashboard page /#pop.

  pop_demo_cdp.py VM_API_SERVER DASHBOARD_SERVER ARGUS_DISPLAY_BRIDGE FAKE_RENDERED_OUTPUT_SERVER OUTDIR

Real processes: the OS-service test double (real gRPC), argus_display_bridge, vm_api_server (resilience_grpc backend, isolated schema-2 store,
allow_test_jobs), the Go dashboard server and headless Chrome. The "player" is a small emulator that does what scripts/pop/pop_autorun.brs does:
it reads the job file the scheduling API wrote, "shows" the advertisement on the OS-service double, and writes the same playback events.
Nothing in the verdicts is pre-set: the verification engine decides from the events and the evidence frames that the bridge really fetched.

Drives the page like the demo user: registry, the three controls, live session progress, the Weston/VOP2 preview, evidence beside the matching
reference frames with scores, the final status, history, exports, and the local-test-mode restriction. Synthetic pictures only.
"""
import asyncio, base64, json, os, shutil, signal, subprocess, sys, tempfile, threading, time, traceback, urllib.request, websockets
from PIL import Image, ImageDraw

API_BIN, DASH_BIN, BRIDGE_BIN, FAKE_BIN, OUT = sys.argv[1:6]
os.makedirs(OUT, exist_ok=True)
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
API_PORT, DASH_PORT, CDP_PORT = 20493, 20492, 9394
BASE = f"http://127.0.0.1:{DASH_PORT}/"
SLOT = 14                                    # seconds; the real demo uses 30 (offsets 3/15/27); the page scales its offsets for a short slot
tmp = tempfile.mkdtemp(prefix="popdemo_")
results, logs, procs = [], [], []
dsock, bsock = os.path.join(tmp, "d.sock"), os.path.join(tmp, "b.sock")
sd, root, proot = os.path.join(tmp, "sd"), os.path.join(tmp, "store"), os.path.join(tmp, "popstore")
events = os.path.join(sd, "pop", "events.jsonl"); jobs = os.path.join(sd, "pop", "jobs")


def check(name, cond, extra=""):
    results.append(bool(cond)); print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(extra)[:300]) if not cond and extra != "" else ""), flush=True)


def start(cmd, log):
    p = subprocess.Popen(cmd, stdout=open(os.path.join(tmp, log), "w"), stderr=subprocess.STDOUT)
    procs.append(p); return p


def stop(p):
    try: p.send_signal(signal.SIGTERM); p.wait(timeout=5)
    except Exception:
        try: p.kill()
        except Exception: pass


def ctl(**kw):
    c = {"frame": "IDLE", "pause": False, "black": False, "frozen": False, "unavailable": False, "clock_skew_ms": 0, "provider": 7}
    c.update(kw)
    p = os.path.join(tmp, "control.json"); json.dump(c, open(p + ".tmp", "w")); os.replace(p + ".tmp", p)


def picture(kind, w, h, shift=0):
    im = Image.new("RGB", (w, h)); px = im.load()
    for y in range(h):
        for x in range(w):
            xn, yn = (x + shift) / w, y / h
            if kind == "A": px[x, y] = (int(40 + 200 * xn), int(220 - 150 * yn), 90 + int(60 * abs(xn - 0.5)))
            else: px[x, y] = (200 if (int(xn * 8) % 2) else 30, 40 + int(180 * abs(yn - 0.5) * 2), 220 if (int(yn * 6) % 2) else 60)
    d = ImageDraw.Draw(im)
    if kind == "A": d.ellipse([w * 0.3, h * 0.2, w * 0.7, h * 0.8], outline=(255, 255, 255), width=4)
    else: d.rectangle([w * 0.2, h * 0.3, w * 0.8, h * 0.7], fill=(250, 250, 40))
    return im


def display_picture(kind, shift=0):
    c = Image.new("RGB", (640, 360), (0, 0, 0)); c.paste(picture(kind, 576, 360, shift=shift), (32, 0)); return c


def api(method, path, body=None):
    req = urllib.request.Request(f"http://127.0.0.1:{API_PORT}{path}", method=method, data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json", "Origin": f"http://127.0.0.1:{API_PORT}"})
    try:
        with urllib.request.urlopen(req, timeout=20) as r: return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try: return e.code, json.loads(e.read())
        except Exception: return e.code, {}


# ---- the player emulator: the same events pop_autorun.brs writes, driven by the job files ----
class Player(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True); self.seen, self.paused, self.stop_flag, self.seq, self.t0 = set(), False, False, 0, time.time()

    def emit(self, ev, **kw):
        self.seq += 1
        with open(events, "a") as f: f.write(json.dumps({"ev": ev, "seq": self.seq, "mono_ms": int((time.time() - self.t0) * 1000), **kw}) + "\n")

    def run(self):
        self.emit("boot", mode="jobs"); self.emit("idle_started", ok=True)            # IDLE: the normal presentation; session-less events only
        while not self.stop_flag:
            names = sorted(n for n in os.listdir(jobs) if n.endswith(".json") and n not in self.seen) if os.path.isdir(jobs) else []
            if self.paused or not names: time.sleep(0.2); continue
            n = names[0]; self.seen.add(n)
            job = json.load(open(os.path.join(jobs, n)))
            sid, mid, exp = job["session_id"], job["media_id"], job["expected_media_id"]
            self.emit("session_scheduled_run", session=sid, job_id=job["job_id"], media_id=mid, expected_media_id=exp, capture_count=0)
            self.emit("play_requested", session=sid, media_id=mid, expected_media_id=exp, ok=True)
            time.sleep(0.15); ctl(frame=mid[-1].upper())
            self.emit("play_started", session=sid, media_id=mid, expected_media_id=exp, vcode=3, latency_ms=200)
            t = time.time(); stop_ms = job["max_play_ms"]; reason = "slot_end"
            if job.get("interrupt_at_ms", -1) >= 0: stop_ms, reason = job["interrupt_at_ms"], "interrupted"
            while (time.time() - t) * 1000 < stop_ms and not self.stop_flag: time.sleep(0.05)
            self.emit("play_stopped", session=sid, media_id=mid, reason=reason)
            self.emit("session_end", session=sid, media_id=mid, expected_media_id=exp, reason=reason, actual_play_ms=int((time.time() - t) * 1000))
            ctl(frame="IDLE"); self.emit("idle_started", ok=True)                    # RESUMING -> IDLE after every job


async def main():
    os.makedirs(os.path.join(tmp, "frames")); os.makedirs(jobs); os.makedirs(root)
    open(events, "w").close()
    for k in ("A", "B"): display_picture(k).save(os.path.join(tmp, "frames", k + ".jpg"), quality=88)
    Image.new("RGB", (640, 360), (70, 70, 70)).save(os.path.join(tmp, "frames", "IDLE.jpg"), quality=88)
    ctl()
    dash_cfg_allow = {"v": True}

    def write_cfg(allow):
        c = {"visual_memory": {"enabled": True, "vector_search_enabled": True, "storage_root": root, "embedding": {"enabled": True},
                               "pop": {"enabled": True, "storage_root": proot, "events_path": events, "jobs_dir": jobs, "poll_ms": 50, "sd_path_prefix_from": "SD:/",
                                       "sd_path_prefix_to": sd + "/", "allow_test_jobs": allow, "capture_backend": "resilience_grpc"},
                               "display": {"enabled": True, "bridge_socket": bsock, "poll_ms": 100, "max_age_ms": 1200, "bridge_binary": BRIDGE_BIN,
                                           "os_socket": "unix://" + dsock, "interest_file": os.path.join(tmp, "interest")}}}
        p = os.path.join(tmp, "cfg.json"); json.dump(c, open(p, "w")); return p

    start([FAKE_BIN, "--socket", dsock, "--frames-dir", os.path.join(tmp, "frames"), "--interest-file", os.path.join(tmp, "interest"), "--control-file", os.path.join(tmp, "control.json"),
           "--stats-file", os.path.join(tmp, "stats.json")], "fake.log")
    time.sleep(1.0)
    apip = start([API_BIN, "--config", write_cfg(True), "--port", str(API_PORT), "--provider", "mock:64", "--status-file", os.path.join(tmp, "st.json")], "api.log")
    dash = start([DASH_BIN, "-port", str(DASH_PORT), "-dir", os.path.join(ROOT_DIR, "dashboard"), "-vm-upstream", f"127.0.0.1:{API_PORT}"], "dash.log")
    time.sleep(2.0)
    # register the two advertisements + both reference profiles (synthetic stand-ins for the real provisioning, which scripts/pop/provision_demo.py does from the videos)
    for mid, name in (("ad-a", "Meet BrightSign"), ("ad-b", "Snowy River")):
        st, r = api("POST", "/api/pop/media", {"media_id": mid, "display_name": name, "uri": mid + ".mp4", "duration_s": 60, "slot_s": SLOT, "width": 1920, "height": 1080, "sha256": "0" * 64})
        check(f"register {mid}", st == 201, r)
        kind = mid[-1].upper()
        for prof, mk in (("screenshot", lambda i: picture(kind, 640, 400, shift=i * 4)), ("display_letterbox", lambda i: display_picture(kind, shift=i * 4))):
            d = os.path.join(proot, "pop", "refs_incoming", mid if prof == "screenshot" else mid + ".display_letterbox"); os.makedirs(d)
            for i in range(8): mk(i).save(os.path.join(d, f"{i * 2000}.jpg"), quality=90)
            st, r = api("POST", f"/api/pop/media/{mid}/references/build?profile={prof}")
            check(f"build {prof} references for {mid}", st == 200 and r["added"] == 8, r)
    player = Player(); player.paused = True; player.start()

    chrome = start(["/usr/bin/google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu", f"--remote-debugging-port={CDP_PORT}", f"--user-data-dir={OUT}/profile",
                    "--window-size=1440,1800", "about:blank"], "chrome.log")
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
            r = await cmd("Page.captureScreenshot", format="png", captureBeyondViewport=True)
            open(f"{OUT}/{name}.png", "wb").write(base64.b64decode(r["data"]))

        async def wait_for(js, timeout=12):
            t0 = time.time()
            while time.time() - t0 < timeout:
                if await ev(js): return True
                await asyncio.sleep(0.25)
            return False

        await cmd("Page.enable"); await cmd("Runtime.enable")
        await cmd("Page.navigate", url=BASE + "?t=%d#pop" % int(time.time() * 1000))

        # ---------------------------------------------------------------- 1. registry, controls, live preview
        check("registry lists Ad A and Ad B with their reference frame counts",
              await wait_for("document.querySelectorAll('#view-pop .pop-table tbody tr').length === 2 && document.querySelector('#view-pop .pop-table').innerText.includes('Meet BrightSign') && document.querySelector('#view-pop .pop-table').innerText.includes('Snowy River')", 20))
        reg_txt = await ev("document.querySelector('#view-pop .pop-table').innerText")
        check("registry shows media ids, screenshot and display-output reference counts (8 each) and the hash column", "ad-a" in reg_txt and "ad-b" in reg_txt and reg_txt.count("8") >= 4 and "display output" in reg_txt.lower(), reg_txt)
        btns = await ev("[...document.querySelectorAll('#view-pop button')].filter(b=>/^Verify (Correct|Wrong|Interrupted) Ad$/.test(b.textContent)).map(b=>b.textContent+':'+b.disabled)")
        check("the three demo controls exist and are enabled in local test mode", btns == ["Verify Correct Ad:false", "Verify Wrong Ad:false", "Verify Interrupted Ad:false"], btns)
        check("the live Weston/VOP2 preview is on this page and shows frames", await wait_for("(()=>{const i=document.getElementById('dispImg');return !document.getElementById('dispPanel').hidden&&!i.hidden&&i.complete&&i.naturalWidth===640})()", 15))

        # ---------------------------------------------------------------- 2. correct ad (the player is paused first: nothing may be reported as played)
        check("idle: the state strip says Ready, normal presentation playing, and no session exists",
              await wait_for("(()=>{const s=document.querySelector('#view-pop .pop-state');return s&&!s.hidden&&/Ready/.test(s.innerText)&&/Normal presentation is playing/.test(s.innerText)})()", 10) and api("GET", "/api/pop/sessions")[1]["total"] == 0)
        click = lambda label: ev("[...document.querySelectorAll('#view-pop button')].find(b=>b.textContent==%s).click(), true" % json.dumps(label))
        await click("Verify Correct Ad")
        check("a session is scheduled and the page says it is waiting for the player (no verdict without a playback event)",
              await wait_for("(()=>{const p=document.querySelectorAll('#view-pop section')[0]; return document.body.innerText.includes('Waiting for the player to start')})()", 15))
        check("the state strip says Starting advertisement while the player has not reported a start",
              await ev("/Starting advertisement/.test(document.querySelector('#view-pop .pop-state').innerText)") is True)
        t_txt = await ev("document.body.innerText")
        chips = await ev("[...document.querySelectorAll('#view-pop .pop-chip')].map(c=>c.textContent)")
        check("while waiting: no verification chip says corroborated; the session is only 'Waiting' / 'Scheduled'", "Playback corroborated" not in chips and any(c.startswith("Waiting") or c == "Scheduled" for c in chips), chips)
        ctl_btns = await ev("[...document.querySelectorAll('#view-pop button')].filter(b=>/^Verify (Correct|Wrong|Interrupted) Ad$/.test(b.textContent)).every(b=>b.disabled)")
        check("controls are locked while a session is in progress", ctl_btns is True)
        player.paused = False                                                        # the player now picks the job up
        check("progress: playing, with the expected ad, the actual ad reported by the player and a start time",
              await wait_for("document.body.innerText.includes('Playing') && document.body.innerText.includes('Actual ad (player report)')", 15))
        pt = await ev("[...document.querySelectorAll('#view-pop .panel-head h3')].find(h=>/Current session/.test(h.textContent)).closest('section').innerText")
        check("progress panel names expected and actual ad, elapsed / slot and the evidence count", "Meet BrightSign (ad-a)" in pt and "Elapsed / slot" in pt and "captured" in pt and "resilience_grpc" in pt, pt)
        check("state strip: PLAYING with the ad name and a countdown that falls",
              await wait_for("/PLAYING Meet BrightSign/.test(document.querySelector('#view-pop .pop-state').innerText) && /left of the/.test(document.querySelector('#view-pop .pop-state').innerText)", 10))
        c1 = await ev("document.querySelector('#view-pop .pop-count').innerText"); time.sleep(2.2); c2 = await ev("document.querySelector('#view-pop .pop-count').innerText")
        check("the countdown decreases over time", float(c1.split(' s')[0]) > float(c2.split(' s')[0]), (c1, c2))
        await shot("1_playing")
        check("evidence frames are captured while it plays (count rises)", await wait_for("/[1-3] of 3 captured/.test(document.body.innerText)", 20))
        check("final status: Playback corroborated, computed by the engine", await wait_for("[...document.querySelectorAll('#view-pop tr.pop-row .pop-chip')].some(c=>c.textContent==='Playback corroborated')", 40))
        check("after the verdict the strip reports completion and then returns to Ready (the player resumed the presentation)",
              await wait_for("/Verification completed|Ready/.test(document.querySelector('#view-pop .pop-state').innerText)", 8) and
              await wait_for("/Ready/.test(document.querySelector('#view-pop .pop-state').innerText)&&!/PLAYING/.test(document.querySelector('#view-pop .pop-state').innerText)", 15))
        check("idle playback created no extra session: exactly 1 session after the first test", api("GET", "/api/pop/sessions")[1]["total"] == 1)
        await shot("2_correct")
        await ev("document.querySelector('#view-pop tr.pop-row').click()")
        check("detail opens with three evidence cards", await wait_for("document.querySelectorAll('.vm-modal .pop-ev').length === 3", 15))
        det = await ev("document.querySelector('.vm-modal').innerText")
        check("each card: capture method, provenance, time uncertainty and a similarity score from the engine",
              det.count("resilience_grpc") >= 3 and det.count("weston_vop2_writeback") >= 3 and det.count("time uncertainty") >= 3 and len(__import__('re').findall(r"similarity to expected [01]\.\d{3}", det)) == 3, det[-1800:])
        check("detail explains why the result was assigned", "why this result" in det.lower() and "usable frames match" in det and "Playback:" in det, det[-1800:])
        check("evidence images and their reference frames decode (side by side)", await wait_for("[...document.querySelectorAll('.vm-modal .pop-ev')].every(c=>{const im=[...c.querySelectorAll('img')];return im.length===2&&im.every(i=>i.complete&&i.naturalWidth>0)})", 15))
        check("references used for gRPC evidence are the 640x360 display-profile frames", await ev("[...document.querySelectorAll('.vm-modal .pop-ev')].every(c=>{const im=[...c.querySelectorAll('img')];return im[0].naturalWidth===640&&im[0].naturalHeight===360&&im[1].naturalWidth===640&&im[1].naturalHeight===360})") is True)
        await shot("3_detail_correct")
        await ev("document.querySelector('.vm-modal-head button').click()")

        # ---------------------------------------------------------------- 3. wrong ad
        await click("Verify Wrong Ad")
        check("wrong ad: final status Mismatch", await wait_for("document.querySelectorAll('#view-pop tr.pop-row').length>=2 && [...document.querySelectorAll('#view-pop tr.pop-row .pop-chip')].some(c=>c.textContent==='Mismatch')", 60))
        await ev("[...document.querySelectorAll('#view-pop tr.pop-row')].find(r=>r.innerText.includes('Mismatch')).click()")
        check("mismatch detail: evidence shown beside the expected ad's reference AND the better-matching other ad's reference (3 images)",
              await wait_for("document.querySelectorAll('.vm-modal .pop-ev').length >= 1 && [...document.querySelectorAll('.vm-modal .pop-ev')].some(c=>c.querySelectorAll('img').length===3)", 15))
        det2 = await ev("document.querySelector('.vm-modal').innerText")
        check("mismatch detail names the played ad as reported and the best other media", "player reported ad-b" in det2 and "best other media: ad-b" in det2, det2[:600])
        await shot("4_detail_wrong")
        await ev("document.querySelector('.vm-modal-head button').click()")

        # ---------------------------------------------------------------- 4. interrupted (needs confirmation)
        n_before = await ev("document.querySelectorAll('#view-pop tr.pop-row').length")
        await ev("window.__confirms=[]; window.confirm=(t)=>{window.__confirms.push(t); return false;}")
        await click("Verify Interrupted Ad")
        await asyncio.sleep(1.5)
        check("interrupted: confirmation is asked and declining schedules nothing", len(await ev("window.__confirms")) == 1 and "STOP it" in (await ev("window.__confirms[0]")) and api("GET", "/api/pop/sessions")[1]["total"] == n_before)
        await ev("window.confirm=(t)=>true")
        await click("Verify Interrupted Ad")
        check("interrupted: final status Incomplete", await wait_for("[...document.querySelectorAll('#view-pop tr.pop-row .pop-chip')].some(c=>c.textContent==='Incomplete')", 60))
        await ev("[...document.querySelectorAll('#view-pop tr.pop-row')].find(r=>r.innerText.includes('Incomplete')).click()")
        await wait_for("document.querySelector('.vm-modal') !== null", 10)
        det3 = await ev("document.querySelector('.vm-modal').innerText")
        check("incomplete detail explains that playback ended early", "interrupted" in det3.lower() and "complete needs at least 90 %" in det3, det3[:600])
        await ev("document.querySelector('.vm-modal-head button').click()")

        # ---------------------------------------------------------------- 5. history, exports
        await ev("document.querySelector('#view-pop').scrollIntoView()")
        hist = await ev("[...document.querySelectorAll('#view-pop .panel-head h3')].find(h=>h.textContent==='Playback history').closest('section').innerText")
        check("history: sessions with backend, start/end, played/slot, evidence count and statuses",
              all(t in hist for t in ("Playback corroborated", "Mismatch", "Incomplete", "gRPC (Weston/VOP2)", "Backend", "Start / end", "Played / slot", "Evidence")), hist[:500])
        st, lst = api("GET", "/api/pop/sessions?limit=50")
        check("API: every session records the backend, an evidence count and the player's media report", lst["total"] == 3 and all(s["capture_backend"] == "resilience_grpc" and s["evidence_count"] >= 1 and s["played_media_id"] for s in lst["sessions"]) and lst.get("now_utc_ms", 0) > 0, lst)
        check("export controls exist: CSV link and JSON button", await ev("!!document.querySelector('#view-pop a[download=\"proof_of_play.csv\"]') && [...document.querySelectorAll('#view-pop button')].some(b=>b.textContent==='Export JSON')"))
        try:
            csv = urllib.request.urlopen(f"http://127.0.0.1:{DASH_PORT}/api/pop/sessions/export.csv", timeout=10).read().decode()
            check("CSV export through the dashboard proxy has a header and three sessions", csv.count("\n") == 4 and "session_id" in csv.split("\n")[0], csv[:200])
        except Exception as e:
            check("CSV export through the dashboard proxy", False, e)
        await shot("5_history")

        # ---------------------------------------------------------------- 5b. the command-line trigger does the same through the same API
        r = await asyncio.to_thread(lambda: subprocess.run([sys.executable, os.path.join(ROOT_DIR, "scripts/pop/demo_trigger.py"), "--api", f"http://127.0.0.1:{API_PORT}", "correct"], capture_output=True, text=True, timeout=120))
        check("scripts/pop/demo_trigger.py correct: schedules, waits and reports a verdict that matches the expectation", r.returncode == 0 and "Playback corroborated" in r.stdout and "MATCHES" in r.stdout and r.stdout.count("sim 1.000") == 3, r.stdout[-700:] + r.stderr[-300:])
        check("the CLI session appears on the dashboard history too", await wait_for("document.querySelectorAll('#view-pop tr.pop-row').length === 4", 15))

        # ---------------------------------------------------------------- 6. the controls exist only in explicit local test mode
        player.paused = True
        stop(apip)
        apip2 = start([API_BIN, "--config", write_cfg(False), "--port", str(API_PORT), "--provider", "mock:64", "--status-file", os.path.join(tmp, "st2.json")], "api2.log")
        time.sleep(2.0)
        st, r = api("POST", "/api/pop/sessions", {"expected_media_id": "ad-a", "play_media_id": "ad-b", "capture_backend": "resilience_grpc"})
        check("without allow_test_jobs the server refuses the wrong-ad / interrupt hooks (403)", st == 403, (st, r))
        await cmd("Page.navigate", url=BASE + "?t=%d#pop" % int(time.time() * 1000))
        check("without allow_test_jobs the page shows the controls as disabled with the reason",
              await wait_for("document.body.innerText.includes('Demo controls are off') && [...document.querySelectorAll('#view-pop button')].filter(b=>/^Verify (Correct|Wrong|Interrupted) Ad$/.test(b.textContent)).every(b=>b.disabled)", 20))
        player.stop_flag = True


try:
    asyncio.run(main())
except BaseException:
    traceback.print_exc(); results.append(False); print("FAIL test aborted by an exception")
finally:
    for p in procs: stop(p)
    time.sleep(0.3)
    errs = [l for l in logs if "EXC" in l or "CONSOLE" in l]
    print("console log: " + ("; ".join(errs) or "none"))
    check("no console errors or exceptions on the Proof of Play page", not errs, errs)
    shutil.rmtree(tmp, ignore_errors=True)
    print("RESULT %d/%d passed" % (sum(results), len(results)))
    sys.exit(0 if all(results) else 1)
