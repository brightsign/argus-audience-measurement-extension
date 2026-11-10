#!/usr/bin/env python3
"""Final stakeholder-demo walkthrough on the REAL player (headless Chrome over CDP; HOST script; no configuration or package change).

  final_demo_walkthrough_cdp.py http://PLAYER:20300 OUTDIR DISPLAY_REF.jpg SESSION_ID MANIFEST.json

Follows docs/FINAL_STAKEHOLDER_DEMO_RUNBOOK.md: Audience Measurement, Visual Memory, camera Visual Search (full frame), camera automatic Object Search, Weston/VOP2 live preview,
display Visual Search (full frame, uploaded reference) and Search by Time, display Object Search with Ad C (bottle and cup, crops, PoP session link), then Proof of Play
correct / wrong / interrupted (these three really play on the player: about 2 minutes). Screenshots: OUTDIR/NN_name.png, report: OUTDIR/walkthrough_report.json.
"""
import asyncio, base64, datetime, importlib.util, json, os, signal, subprocess, sys, time, urllib.request, zoneinfo, websockets

API, OUT, REF, SID, MAN = sys.argv[1].rstrip("/"), sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
os.makedirs(OUT, exist_ok=True)
CDP_PORT = 9633
spec = importlib.util.spec_from_file_location("hwmod", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "api", "vm_objects_display_hw.py")); hw = importlib.util.module_from_spec(spec); spec.loader.exec_module(hw)
man = json.load(open(MAN)); results, logs, procs, REPORT = [], [], [], {}


def check(name, cond, extra=""):
    results.append((name, bool(cond))); print(("PASS " if cond else "FAIL ") + name + ((" -- " + str(extra)[:300]) if not cond and extra != "" else ""), flush=True)


def api(path, timeout=30):
    return json.load(urllib.request.urlopen(API + path, timeout=timeout))


async def main():
    # a FRESH Ad C play first (the demo does the same: Proof of Play -> Schedule play, expected ad-c), so its frames are the newest stored display frames
    rq = urllib.request.Request(API + "/api/pop/sessions", data=json.dumps({"expected_media_id": "ad-c", "capture_backend": "resilience_grpc", "capture_offsets_s": [3, 15, 27]}).encode(), headers={"Content-Type": "application/json", "Origin": API}, method="POST")
    adc_sid = json.load(urllib.request.urlopen(rq, timeout=30))["session_id"]; REPORT["ad_c_session"] = adc_sid
    for _ in range(60):
        time.sleep(2); sv = api("/api/pop/sessions/" + adc_sid)
        if sv.get("end_utc_ms", 0) > 0 and sv.get("verification"): break
    check("0 fresh Ad C play: corroborated, 29-30 history frames", sv["status"] == "playback_corroborated" and 28 <= api("/api/vm/display-frames?session_id=%s&limit=100" % adc_sid)["total"] <= 31, sv["status"])
    fr = [f for f in api("/api/vm/display-frames?session_id=%s&limit=100" % adc_sid)["frames"] if f["kind"] == "history" and 4.0 <= f["playback_offset_ms"] / 1000 <= 8.0 and f["image_available"]]
    pick = fr[0]; t = pick["playback_offset_ms"] / 1000.0
    boxes = {"bottle": hw.gt_box(man, hw.scene_of(t), "water bottle", t), "cup": hw.gt_box(man, hw.scene_of(t), "coffee cup", t)}
    chrome = subprocess.Popen(["/usr/bin/google-chrome", "--headless=new", "--no-sandbox", "--disable-gpu", f"--remote-debugging-port={CDP_PORT}", f"--user-data-dir={OUT}/profile", "--window-size=1440,2400", "about:blank"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL); procs.append(chrome); time.sleep(3)
    ws_url = [x for x in json.load(urllib.request.urlopen(f"http://127.0.0.1:{CDP_PORT}/json")) if x["type"] == "page"][0]["webSocketDebuggerUrl"]
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
            if "exceptionDetails" in r: logs.append("EVALERR " + json.dumps(r["exceptionDetails"])[:200] + " :: " + js[:140])
            return r.get("result", {}).get("value")

        step = [0]

        async def shot(name):
            step[0] += 1; r = await cmd("Page.captureScreenshot", format="png", captureBeyondViewport=True); open(f"{OUT}/{step[0]:02d}_{name}.png", "wb").write(base64.b64decode(r["data"]))

        async def wait_for(js, timeout=20):
            t0 = time.time()
            while time.time() - t0 < timeout:
                if await ev(js): return True
                await asyncio.sleep(0.3)
            return False

        async def goto(h):
            await cmd("Page.navigate", url=API + "/?t=%d%s" % (int(time.time() * 1000), h)); await asyncio.sleep(1.5)

        async def setv(sel, v):
            await ev("(()=>{const e=document.querySelector(%s);e.value=%s;e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new Event('change',{bubbles:true}));return 1})()" % (json.dumps(sel), json.dumps(str(v))))

        SRC = "[...document.querySelectorAll('#view-search [aria-label=\"Search source\"] button')]"
        P = "document.querySelector('[data-role=auto-panel]')"
        await cmd("Page.enable"); await cmd("Runtime.enable"); await cmd("Emulation.setDeviceMetricsOverride", width=1440, height=2400, deviceScaleFactor=1, mobile=False)

        # 1 ---------------------------------------------------------------- Audience Measurement
        await goto("#audience"); await asyncio.sleep(4)
        txt = await ev("document.getElementById('view-audience').innerText"); check("1 Audience Measurement shows live content (LIVE, FPS, present/gazing, attention, NPU)", bool(txt) and "LIVE" in txt and "PRESENT NOW" in txt and "NPU LOAD" in txt, (txt or "")[:200]); await shot("audience_measurement")
        # 2 ---------------------------------------------------------------- Camera Visual Memory
        await goto("#memory")
        check("2 Visual Memory gallery shows stored camera frames", await wait_for("document.querySelectorAll('#view-memory .vm-card img').length>=6 && [...document.querySelectorAll('#view-memory .vm-card img')].slice(0,6).every(i=>i.complete&&i.naturalWidth>0)", 25)); await shot("visual_memory")
        pill = await ev("(document.querySelector('.vm-pill,[data-role=health-pill],header .pill')||{}).innerText"); REPORT["health_pill"] = pill
        # 3 ---------------------------------------------------------------- Camera Visual Search (full frame)
        await goto("#search")
        check("3 Visual Search opens on Audience Camera, Full frame search", await wait_for("/Full frame search/.test(document.getElementById('view-search').innerText)&&/Pick a stored frame/.test(document.getElementById('view-search').innerText)"))
        await ev("[...document.querySelectorAll('#view-search button')].find(b=>/Pick a stored frame/.test(b.textContent)).click(), 1")
        check("3 the stored-frame picker lists camera frames", await wait_for("document.querySelectorAll('.vm-modal .vm-card').length>=6", 25))
        await ev("document.querySelectorAll('.vm-modal .vm-card')[2].click(), 1")
        check("3 Search becomes enabled", await wait_for("!([...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Search').disabled)", 15))
        await ev("[...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Search').click(), 1")
        ok = await wait_for("document.querySelectorAll('#view-search .vm-card').length>=5 && [...document.querySelectorAll('#view-search .vm-card img')].slice(0,5).every(i=>i.complete&&i.naturalWidth>0)", 40)
        check("3 camera full-frame search returns five similar frames (with images and similarity)", ok, await ev("document.getElementById('view-search').innerText.slice(0,300)")); await shot("camera_visual_search")
        # 4 ---------------------------------------------------------------- Camera automatic Object Search
        await goto("#search?mode=objects")
        if not await wait_for(f"!!{P}&&/Automatic object indexing/i.test({P}.innerText)", 8): REPORT["panel_needed_reload"] = True; await goto("#search?mode=objects")      # known intermittent UI race: F5 recovers
        check("4 camera Object search: automatic indexing panel (live scan on) and indexed objects", await wait_for(f"!!{P}&&/Automatic object indexing/i.test({P}.innerText)&&/live scan on/.test({P}.innerText)&&document.querySelectorAll('#view-search .vm-card').length>=3", 25)); await shot("camera_object_search")
        # 5 ---------------------------------------------------------------- Weston/VOP2 live preview (Proof of Play page)
        await goto("#pop")
        check("5 Weston/VOP2 live display preview shows a 640x360 frame", await wait_for("(()=>{const i=document.querySelector('#dispPanel img');return !!i&&i.complete&&i.naturalWidth===640&&i.naturalHeight===360})()", 25), await ev("(document.getElementById('dispPanel')||{innerText:'(no panel)'}).innerText.slice(0,200)")); await shot("weston_live_preview")
        # 6 ---------------------------------------------------------------- Display Visual Search (full frame, uploaded reference)
        await goto("#search?source=display")
        check("6 display Visual Search page states its coverage", await wait_for("/Searchable stored evidence/.test(document.getElementById('view-search').innerText)", 25))
        check("6 the display search panel finished loading (advertisement list filled)", await wait_for("document.querySelectorAll('#view-search select[aria-label=Advertisement] option').length>=3", 40))
        b64 = base64.b64encode(open(REF, "rb").read()).decode()
        await ev("""(()=>{const bin=atob(%s);const u=new Uint8Array(bin.length);for(let i=0;i<bin.length;i++)u[i]=bin.charCodeAt(i);const dt=new DataTransfer();dt.items.add(new File([u],'ref.jpg',{type:'image/jpeg'}));
          const inp=document.querySelector('#view-search input[type=file][aria-label*="display search"]');inp.files=dt.files;inp.dispatchEvent(new Event('change'));return true})()""" % json.dumps(b64))
        await setv("#view-search select[aria-label=Advertisement]", "ad-c")
        check("6 the reference is accepted and Search display evidence is enabled", await wait_for("(()=>{const b=[...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Search display evidence');return !!b&&!b.disabled})()", 15))
        await ev("[...document.querySelectorAll('#view-search button')].find(b=>b.textContent==='Search display evidence').click(), 1")
        check("6 display full-frame search returns match cards (reference beside the stored frame)", await wait_for("document.querySelectorAll('#view-search .ds-hit').length>=3&&[...document.querySelectorAll('#view-search .ds-hit')].slice(0,3).every(c=>[...c.querySelectorAll('img')].every(i=>i.complete&&i.naturalWidth>0))", 30), await ev("document.getElementById('view-search').innerText.slice(0,300)"))
        card = await ev("document.querySelector('#view-search .ds-hit').innerText.replace(/\\n+/g,' | ')"); REPORT["display_search_top_card"] = card
        check("6 a card shows similarity, ad, offset into the play, session", bool(card) and "ad-c" in card and "into the play" in card and "similarity" in card, card); await shot("display_visual_search")
        # 7 ---------------------------------------------------------------- Search by Time
        dt = datetime.datetime.fromtimestamp(pick["capture_utc_ms"] // 1000, zoneinfo.ZoneInfo("Europe/London"))
        await goto("#search?source=display")
        check("7 Search by time panel is on the display page", await wait_for("!!document.querySelector('[data-role=time-search] [data-role=ts-run]')", 25))
        await ev("""((a)=>{const q=s=>document.querySelector('[data-role=time-search] '+s);const set=(el,v)=>{el.value=v;el.dispatchEvent(new Event('input',{bubbles:true}));el.dispatchEvent(new Event('change',{bubbles:true}));};
          set(q('[data-role=ts-tz]'),'device');set(q('[data-role=ts-date]'),a.d);set(q('[data-role=ts-time]'),a.t);set(q('[data-role=ts-window]'),'10');set(q('[data-role=ts-tol]'),'2000');return 1})(%s)""" % json.dumps({"d": dt.strftime("%Y-%m-%d"), "t": dt.strftime("%H:%M:%S")}))
        await ev("(document.querySelector('[data-role=ts-run]').click(),1)")
        check("7 Search by time finds the nearest stored display frame with its Ad C session", await wait_for("!!document.querySelector('[data-role=nearest-frame]')&&/ad-c/.test(document.querySelector('[data-role=nearest-frame]').innerText)", 30), await ev("(document.querySelector('[data-role=time-search]')||{innerText:''}).innerText.slice(0,300)"))
        check("7 the stored JPEG is displayed (640x360)", await wait_for("(()=>{const i=document.querySelector('[data-role=nearest-frame] img');return !!i&&i.complete&&i.naturalWidth===640})()", 15)); await shot("search_by_time")
        # 8 ---------------------------------------------------------------- Display automatic Object Search with Ad C
        await goto("#search?mode=objects")
        await ev(f"{SRC}.find(b=>/BrightSign Display/.test(b.textContent)).click(), 1")
        check("8 BrightSign Display: indexed display objects with bottle and cup classes", await wait_for("/indexed display objects/.test((document.querySelector('[data-role=object-totals]')||{innerText:''}).innerText)&&/bottle/.test((document.querySelector('[data-role=class-chips]')||{innerText:''}).innerText)", 25))
        before = await ev("(document.querySelector('[data-role=object-totals]')||{}).innerText")
        await ev("document.querySelector('[data-role=backfill-run]').click(), 1")
        check("8 the UI button 'Index the 50 most recent retained frames' indexes the fresh Ad C frames", await wait_for("(()=>{const e=document.querySelector('[data-role=backfill-state]');return !!e&&/Backfill done/.test(e.innerText)})()", 120), await ev("(document.querySelector('[data-role=backfill-state]')||{}).innerText"))
        REPORT["display_totals_before"] = before; REPORT["display_totals"] = await ev("(document.querySelector('[data-role=object-totals]')||{}).innerText"); await shot("display_object_search_totals")
        for cls in ("bottle", "cup"):
            x, y, w, h = [int(round(v)) for v in boxes[cls]]
            await ev("[...document.querySelectorAll('#view-search button')].find(b=>/Select object in a stored display frame/.test(b.textContent)).click(), 1")
            check(f"8 [{cls}] picker opens with Ad C frames and no 'JPEG expired' tile", await wait_for("document.querySelectorAll('.vm-modal [data-frame-kind]').length>=20", 25) and (await ev("[...document.querySelectorAll('.vm-modal [data-frame-kind]')].filter(t=>/JPEG expired/.test(t.innerText)).length")) == 0)
            await ev("document.querySelector('.vm-modal [data-frame-kind=history][data-frame-id=\"%d\"]').click(), 1" % pick["id"])
            await wait_for("!!document.querySelector('.vm-roi-stage img')&&document.querySelector('.vm-roi-stage img').complete&&document.querySelector('.vm-roi-stage img').naturalWidth===640")
            for k, v in (("X", x), ("Y", y), ("W", w), ("H", h)): await setv(f".vm-modal input[aria-label={k}]", v)
            await wait_for("(()=>{const i=document.querySelector('.vm-roi-prev img');return !!i&&i.complete&&i.naturalWidth>8})()"); await shot(f"display_{cls}_selected_box")
            await ev("[...document.querySelectorAll('.vm-modal button')].find(b=>b.textContent==='Find Similar Objects').click(), 1")
            ok = await wait_for("(()=>{const g=document.querySelector('[data-role=result-grid]');return !!g&&g.querySelectorAll('.vm-card').length>=5&&[...g.querySelectorAll('.vm-card')].every(c=>c.dataset.source==='display')&&[...g.querySelectorAll('.vm-card img')].slice(0,5).every(i=>i.complete&&i.naturalWidth>0)})()", 40)
            cards = await ev("[...document.querySelectorAll('[data-role=result-grid] .vm-card')].slice(0,5).map(c=>c.innerText.replace(/\\n+/g,' | '))") or []
            REPORT[f"display_{cls}_top_cards"] = cards
            check(f"8 [{cls}] matching crops: display only, thumbnails, similarity, confidence, ad, offset, session", ok and all(("into the play" in c and "session s-" in c) for c in cards), cards[:1]); await shot(f"display_{cls}_results")
            await ev("document.querySelector('[data-role=result-grid] .vm-card').click(), 1")
            check(f"8 [{cls}] compare dialog shows the source frame with its box and the PoP session link", await wait_for("(()=>{const m=document.querySelector('.vm-modal');return !!m&&!!m.querySelector('[data-role=session-link]')&&!!m.querySelector('.vm-roi-frame img')&&m.querySelector('.vm-roi-frame img').complete&&m.querySelector('.vm-roi-frame img').naturalWidth===640})()")); await shot(f"display_{cls}_compare")
            await ev("document.querySelector('.vm-modal [data-role=session-link]').click(), 1")
            check(f"8 [{cls}] the session link opens the Proof-of-Play session", await wait_for("location.hash.startsWith('#pop')", 20)); await asyncio.sleep(3); await shot(f"display_{cls}_pop_session")
            await goto("#search?mode=objects"); await ev(f"{SRC}.find(b=>/BrightSign Display/.test(b.textContent)).click(), 1"); await wait_for("/indexed display objects/.test((document.querySelector('[data-role=object-totals]')||{innerText:''}).innerText)")
        await ev(f"{SRC}.find(b=>/Audience Camera/.test(b.textContent)).click(), 1")
        check("8 back on Audience Camera: camera objects only", await wait_for("(()=>{const c=[...document.querySelectorAll('#view-search .vm-card')];return c.length>=3&&c.every(x=>!/display frame/.test(x.innerText))})()", 20)); await shot("back_to_camera")
        # 9 ---------------------------------------------------------------- Proof of Play: correct, wrong, interrupted
        if os.environ.get("SKIP_POP"):
            check("no console errors", not [l for l in logs if "favicon" not in l], logs[:3])
            for p in procs[::-1]:
                try: p.send_signal(signal.SIGTERM); p.wait(timeout=5)
                except Exception: p.kill()
            json.dump({"results": results, "report": REPORT}, open(f"{OUT}/walkthrough_report.json", "w"), indent=1)
            print("RESULT %d/%d passed (PoP section skipped)" % (sum(1 for _, o in results if o), len(results))); sys.exit(0 if all(o for _, o in results) else 1)
        await goto("#pop"); await ev("window.confirm=()=>true,1")
        check("9 Proof of Play demo controls are present", await wait_for("[...document.querySelectorAll('button')].some(b=>b.textContent==='Verify Correct Ad')", 25))
        ids_before = {s["session_id"] for s in api("/api/pop/sessions?limit=5")["sessions"]}
        for label, want in (("Verify Correct Ad", "playback_corroborated"), ("Verify Wrong Ad", "mismatch"), ("Verify Interrupted Ad", "incomplete")):
            ok = await wait_for("(()=>{const b=[...document.querySelectorAll('button')].find(b=>b.textContent===%s);return !!b&&!b.disabled})()" % json.dumps(label), 90)
            await ev("window.confirm=()=>true,[...document.querySelectorAll('button')].find(b=>b.textContent===%s).click(),1" % json.dumps(label))
            sid = None
            for _ in range(30):
                await asyncio.sleep(1.5); new = [s for s in api("/api/pop/sessions?limit=5")["sessions"] if s["session_id"] not in ids_before]
                if new: sid = new[0]["session_id"]; ids_before.add(sid); break
            if label == "Verify Correct Ad": await asyncio.sleep(12); await shot("pop_correct_in_progress")
            v = None
            for _ in range(60):
                await asyncio.sleep(2); s = api("/api/pop/sessions/" + sid) if sid else {}
                if s.get("end_utc_ms", 0) > 0 and s.get("verification"): v = s; break
            await asyncio.sleep(3)
            check(f"9 {label}: verdict {want}", bool(v) and v["status"] == want and ok, (v or {}).get("status")); REPORT[label] = {"session": sid, "status": (v or {}).get("status"), "mean_sim": ((v or {}).get("verification") or {}).get("mean_sim_expected")}
            await shot("pop_" + label.split()[1].lower())
            await goto("#pop"); await ev("window.confirm=()=>true,1")
        check("9 presentation resumed (display idle, preview available)", await wait_for("true", 1) and api("/api/display/status")["display_health"] in ("idle", "ok", "available"), api("/api/display/status")["display_health"])
        check("no console errors", not [l for l in logs if "favicon" not in l], logs[:3])
    for p in procs[::-1]:
        try: p.send_signal(signal.SIGTERM); p.wait(timeout=5)
        except Exception: p.kill()
    json.dump({"results": results, "report": REPORT}, open(f"{OUT}/walkthrough_report.json", "w"), indent=1)
    print("RESULT %d/%d passed" % (sum(1 for _, o in results if o), len(results)))
    sys.exit(0 if all(o for _, o in results) else 1)

try: asyncio.run(main())
except Exception:
    import traceback; traceback.print_exc()
    for p in procs[::-1]:
        try: p.kill()
        except Exception: pass
    sys.exit(2)
