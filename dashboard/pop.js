/* Argus dashboard, D5 Proof of Play: media registry, scheduling, playback history, verification detail with a
   screenshot-vs-reference comparison, export. Vanilla JS; relies on window.VMUI from vm.js. All server strings are rendered
   with textContent. Contract: docs/VISUAL_MEMORY_PROOF_OF_PLAY.md.
   Evidence comes from the player's PRESENTATION OUTPUT (display capture), never from the audience camera. */
(() => {
  "use strict";
  if (!window.VMUI) return;
  const U = window.VMUI, h = U.h, isNum = U.isNum, DASH = U.DASH;
  const POP = "/api/pop";
  const STATUS = {
    scheduled: ["Scheduled", ""],
    playback_reported: ["Playback reported", "info"],
    visual_match: ["Visual match", "ok"],
    playback_corroborated: ["Playback corroborated", "ok"],
    mismatch: ["Mismatch", "bad"],
    incomplete: ["Incomplete", "warn"],
    capture_unsupported: ["Capture unsupported", "warn"],
  };
  const NOTE = "Proof of Play compares what the player reports with screenshots of its presentation output (the HDMI/display path), " +
    "using the same MobileNetV2 image embedding as Visual Search. Similarity scores are not probabilities, and a corroborated play is " +
    "not a certified impression: it says nothing about whether anyone watched. The audience camera is never used as evidence.";
  const S = { view: null, media: [], sessions: [], total: 0, status: null, filter: { media_id: "", status: "" }, limit: 25, timer: null, modal: null, nowSkew: 0, active: null, tick: 0, demoSel: {} };

  async function papi(path, opts) {
    try {
      const r = await fetch(POP + path, Object.assign({ cache: "no-store", credentials: "same-origin" }, opts));
      let data = null;
      try { data = await r.json(); } catch (_) {}
      return { status: r.status, data };
    } catch (e) { return { status: 0, data: null }; }
  }
  const post = (path, body) => papi(path, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body || {}) });
  const msg = (r) => (r.data && (r.data.message || r.data.error)) || (r.status === 0 ? "dashboard server unreachable" : "HTTP " + r.status);
  const fmtMs = (v) => (isNum(v) && v >= 0 ? (v / 1000).toFixed(1) + " s" : DASH);
  const sim = (v) => (isNum(v) && v >= 0 ? v.toFixed(3) : DASH);
  function chip(status) {
    const s = STATUS[status] || [status, ""];
    return h("span", { class: "pop-chip " + s[1], text: s[0] });
  }

  // ---- page skeleton -----------------------------------------------------------------------------------------
  function build() {
    const root = document.getElementById("view-pop");
    S.view = {
      banner: h("div", { class: "vm-banner" }), reg: h("section", { class: "panel pop-panel" }), sched: h("section", { class: "panel pop-panel" }),
      hist: h("section", { class: "panel pop-panel" }), demo: h("section", { class: "panel pop-panel" }), prog: h("section", { class: "panel pop-panel" }),
    };
    const disp = document.getElementById("dispPanel");              // Display output preview (display.js): lives in this view, under the banner
    root.replaceChildren(h("div", { class: "vm-page" },
      h("div", { class: "panel-head" }, h("h2", { text: "Proof of Play" }), h("span", { class: "muted", id: "popLive", text: "" })),
      S.view.banner, S.view.reg, S.view.demo, ...(disp ? [disp] : []), S.view.prog, S.view.sched, S.view.hist));
  }

  function renderBanner() {
    const st = S.status, b = S.view.banner;
    b.className = "vm-banner" + (st && st.enabled ? "" : " warn");
    const kids = [h("p", null, h("b", { text: "Display-output evidence. " }), NOTE)];
    if (!st) kids.push(h("p", { text: "Loading status…" }));
    else if (!st.enabled) kids.push(h("p", null, h("b", { text: "Not enabled. " }), "Set visual_memory.pop.enabled in the player configuration to collect Proof of Play data. Nothing is collected while it is off."));
    else {
      const a = st.adapter || {}, s = st.store || {};
      kids.push(h("div", { class: "vm-chips", style: "margin-top:8px" },
        h("span", { class: "vm-chip" + (a.running ? "" : " bad") }, "Event adapter", h("b", { text: a.running ? "running" : "stopped" })),
        h("span", { class: "vm-chip" }, "Events", h("b", { text: String(a.events_ingested ?? 0) })),
        h("span", { class: "vm-chip" + (a.errors ? " warn" : "") }, "Adapter errors", h("b", { text: String(a.errors ?? 0) })),
        h("span", { class: "vm-chip" }, "Media", h("b", { text: String(S.media.length || s.media || 0) })),
        h("span", { class: "vm-chip" }, "Sessions", h("b", { text: String(s.sessions ?? 0) })),
        h("span", { class: "vm-chip" }, "Evidence frames", h("b", { text: String(s.evidence ?? 0) })),
        h("span", { class: "vm-chip" }, "Evidence size", h("b", { text: U.fmtBytes ? U.fmtBytes(s.evidence_bytes || 0) : String(s.evidence_bytes || 0) }))));
      if (a.last_error) kids.push(h("p", { class: "muted", text: "Last adapter message: " + a.last_error }));
    }
    b.replaceChildren(...kids);
  }

  // ---- registry ---------------------------------------------------------------------------------------------
  function renderRegistry() {
    const sec = S.view.reg;
    const rows = S.media.map((m) => h("tr", null,
      h("td", null, h("b", { text: m.display_name || m.media_id }), h("div", { class: "muted", text: m.media_id })),
      h("td", { text: m.kind }),
      h("td", { class: "num", text: m.duration_s ? m.duration_s.toFixed(1) + " s" : "live" }),
      h("td", { class: "num", text: m.slot_s + " s" }),
      h("td", { class: "num", text: String(m.reference_frames) }),
      h("td", { class: "num", text: String(m.display_reference_frames || 0) }),
      h("td", { class: "muted", text: m.sha256 ? m.sha256.slice(0, 12) + "…" : DASH }),
      h("td", { class: "muted", text: m.uri })));
    sec.replaceChildren(h("div", { class: "panel-head" }, h("h3", { text: "Advertisement registry" })),
      ...(S.media.length ? [h("p", { class: "muted", text: "References are built from the original video files only (never from captured evidence). “Display output” references use the Weston/VOP2 geometry: a 640×360 frame with the picture 576×360 centred between black bars. Embeddings: MobileNetV2, the same model as Visual Search." })] : []),
      S.media.length ? h("div", { class: "pop-scroll" }, h("table", { class: "pop-table" },
        h("thead", null, h("tr", null, ...["Media", "Kind", "Duration", "Slot", "Reference frames (screenshot)", "Reference frames (display output)", "SHA-256", "Source"].map((t) => h("th", { text: t })))), h("tbody", null, ...rows)))
        : h("div", { class: "vm-empty", text: "No media registered. Register assets with scripts/pop/register_media.py (see docs/VISUAL_MEMORY_PROOF_OF_PLAY.md)." }));
  }

  // ---- scheduling ----------------------------------------------------------------------------------------------
  function renderSchedule() {
    const sec = S.view.sched, st = S.status;
    if (!st || !st.enabled || !S.media.length) { sec.replaceChildren(); sec.hidden = true; return; }
    sec.hidden = false;
    const opts = (sel) => S.media.map((m) => h("option", { value: m.media_id, text: (m.display_name || m.media_id) + (m.reference_frames ? "" : " (no references)") }));
    const exp = h("select", { id: "popExp", "aria-label": "Expected advertisement" }, ...opts());
    const out = h("span", { class: "muted", role: "status", id: "popSchedOut" });
    const extra = [];
    let play = null, intr = null, brk = null;
    if (st.allow_test_jobs) {
      play = h("select", { "aria-label": "Play media" }, h("option", { value: "", text: "same as expected" }), ...opts());
      intr = h("input", { type: "number", min: "0", step: "1", placeholder: "interrupt after s", "aria-label": "Interrupt after seconds", style: "width:130px" });
      brk = h("input", { type: "checkbox", id: "popBrk" });
      extra.push(h("span", { class: "muted", text: "Test hooks:" }), h("label", null, "play ", play), intr, h("label", { for: "popBrk" }, brk, " break screenshot"));
    }
    const go = h("button", { class: "vm-btn primary", type: "button", text: "Schedule play", onclick: async () => {
      go.disabled = true; out.textContent = "Scheduling…";
      const body = { expected_media_id: exp.value };
      if (play && play.value) body.play_media_id = play.value;
      if (intr && intr.value !== "") body.interrupt_at_s = Number(intr.value);
      if (brk && brk.checked) body.break_capture = true;
      const r = await post("/sessions", body);
      go.disabled = false;
      out.textContent = r.status === 201 ? `Scheduled ${r.data.session_id}: the player starts it within a few seconds; the verdict appears in the history.` : "Could not schedule: " + msg(r);
      if (r.status === 201) refresh(true);
    } });
    sec.replaceChildren(h("div", { class: "panel-head" }, h("h3", { text: "Schedule a play" })),
      h("div", { class: "vm-row", style: "gap:10px;flex-wrap:wrap;align-items:center" }, h("label", null, "Expected ", exp), ...extra, go, out),
      h("p", { class: "muted", text: "The player application plays the media for its slot length and takes screenshots of the display output; the result is verified automatically." }));
  }

  // ---- demonstration controls (explicit local test mode only) -------------------------------------------------------
  // These use the ordinary scheduling API with the existing test hooks (visual_memory.pop.allow_test_jobs). Only advertisements that are
  // REGISTERED can be chosen: there is no free media path and no command execution here.
  const pickMedia = (re, fallbackIdx) => S.media.find((m) => re.test(m.media_id + " " + (m.display_name || ""))) || S.media[fallbackIdx];
  function demoOffsets(m) {
    const slot = (m && m.slot_s) || 30;
    return slot >= 30 ? [3, 15, 27].filter((t) => t < slot) : [0.1, 0.5, 0.9].map((f) => Math.round(f * slot * 10) / 10);
  }
  // "about 10 s" on the normal 30 s slot; proportionally earlier on a short test slot so the interruption is always inside the slot
  const demoInterrupt = (m) => { const slot = (m && m.slot_s) || 30; return slot >= 15 ? 10 : Math.max(1, Math.round(slot * 0.7)); };
  function renderDemo() {
    const sec = S.view.demo, st = S.status;
    if (!st || !st.enabled || S.media.length < 2) { sec.replaceChildren(); sec.hidden = true; return; }
    sec.hidden = false;
    const opt = (m) => h("option", { value: m.media_id, text: (m.display_name || m.media_id) + " (" + m.media_id + ")" });
    const a0 = (S.demoSel.a && S.media.find((m) => m.media_id === S.demoSel.a)) || pickMedia(/ad-a|meet/i, 0);
    const b0 = (S.demoSel.b && S.media.find((m) => m.media_id === S.demoSel.b)) || pickMedia(/ad-b|snowy/i, 1);
    const selA = h("select", { id: "popDemoA", "aria-label": "Ad A (expected)" }, ...S.media.map(opt));
    const selB = h("select", { id: "popDemoB", "aria-label": "Ad B (the wrong ad)" }, ...S.media.map(opt));
    selA.value = a0.media_id; selB.value = b0.media_id === a0.media_id ? (S.media.find((m) => m.media_id !== a0.media_id) || b0).media_id : b0.media_id;
    selA.onchange = () => { S.demoSel.a = selA.value; }; selB.onchange = () => { S.demoSel.b = selB.value; };
    const backends = [["brightscript_screenshot", "BrightScript screenshot"]];
    if (st.display_source_available && !st.schema_legacy_v1) backends.push(["resilience_grpc", "Resilience gRPC (Weston/VOP2)"]);
    const be = h("select", { id: "popDemoBe", "aria-label": "Capture backend" }, ...backends.map(([v, t]) => h("option", { value: v, text: t })));
    be.value = backends.some(([v]) => v === st.capture_backend) ? st.capture_backend : backends[0][0];
    const out = h("div", { class: "muted", role: "status", id: "popDemoOut" });
    const enabled = !!st.allow_test_jobs;
    const busy = !!S.active;
    const run = async (label, body, confirmText) => {
      if (confirmText && !window.confirm(confirmText)) return;
      out.textContent = "Scheduling “" + label + "”…";
      const r = await post("/sessions", Object.assign({ capture_backend: be.value, capture_offsets_s: demoOffsets(S.media.find((m) => m.media_id === selA.value)) }, body));
      out.textContent = r.status === 201 ? `Scheduled ${r.data.session_id} (${label}). The player starts it within a few seconds: follow it in “Current session” below.` : "Could not schedule: " + msg(r);
      if (r.status === 201) refresh(true);
    };
    const btn = (label, cls, fn) => h("button", { class: "vm-btn " + cls, type: "button", disabled: !enabled || busy, text: label, onclick: fn });
    sec.replaceChildren(h("div", { class: "panel-head" }, h("h3", { text: "Demonstration" }), h("span", { class: "muted", text: enabled ? "local test mode" : "disabled" })),
      ...(enabled ? [] : [h("p", { class: "muted", text: "Demo controls are off. They need visual_memory.pop.allow_test_jobs=true in the player configuration (an explicit local test mode)." })]),
      h("div", { class: "vm-row", style: "gap:10px;flex-wrap:wrap;align-items:center" }, h("label", null, "Ad A (expected) ", selA), h("label", null, "Ad B (wrong ad) ", selB), h("label", null, "Capture ", be)),
      h("div", { class: "vm-row", style: "gap:10px;flex-wrap:wrap;margin-top:10px" },
        btn("Verify Correct Ad", "primary", () => run("Verify Correct Ad", { expected_media_id: selA.value, play_media_id: selA.value })),
        btn("Verify Wrong Ad", "", () => run("Verify Wrong Ad", { expected_media_id: selA.value, play_media_id: selB.value })),
        btn("Verify Interrupted Ad", "", () => run("Verify Interrupted Ad", { expected_media_id: selA.value, play_media_id: selA.value, interrupt_at_s: demoInterrupt(S.media.find((m) => m.media_id === selA.value)) },
          `This will start Ad A on the player and STOP it after about ${demoInterrupt(S.media.find((m) => m.media_id === selA.value))} seconds to test the Incomplete verdict. The presentation on the screen is interrupted. Continue?`))),
      ...(busy ? [h("p", { class: "muted", text: "A session is in progress; the controls unlock when it has a verdict." })] : []),
      out,
      h("p", { class: "muted", text: "Correct: expected A, A plays. Wrong: expected A, B plays. Interrupted: expected A, stopped after about 10 s. Evidence is requested at about " +
        demoOffsets(S.media.find((m) => m.media_id === selA.value)).join(", ") + " s into the play. Results are produced by the real verification engine; nothing here is pre-set." }));
  }

  // ---- current session: live progress ----------------------------------------------------------------------------
  function activeSession() {
    const now = Date.now() + S.nowSkew;
    return S.sessions.find((x) => !x.verification && (now - (x.scheduled_utc_ms || 0)) < ((x.slot_ms || 30000) + 120000)) || null;
  }
  const mediaName = (id) => { const m = S.media.find((x) => x.media_id === id); return m ? (m.display_name || m.media_id) + " (" + m.media_id + ")" : (id || DASH); };
  function renderProgress() {
    const sec = S.view.prog, st = S.status;
    if (!st || !st.enabled) { sec.replaceChildren(); sec.hidden = true; return; }
    const a = S.active || S.sessions[0];
    if (!a) { sec.replaceChildren(); sec.hidden = true; return; }
    sec.hidden = false;
    const now = Date.now() + S.nowSkew;
    const started = a.start_utc_ms > 0;
    const slot = a.slot_ms || 0;
    const ended = a.end_utc_ms > 0;
    const elapsed = started ? (ended ? a.actual_play_ms : Math.max(0, now - a.start_utc_ms)) : 0;
    const pct = slot ? Math.min(100, Math.round(100 * elapsed / slot)) : 0;
    const phase = a.verification ? "Verified" : ended ? "Verifying" : started ? "Playing" : "Waiting for the player to start";
    const bar = h("div", { class: "pop-bar", role: "progressbar", "aria-valuemin": "0", "aria-valuemax": "100", "aria-valuenow": String(pct) }, h("div", { class: "pop-bar-fill", style: "width:" + pct + "%" }));
    sec.replaceChildren(h("div", { class: "panel-head" }, h("h3", { text: S.active ? "Current session" : "Latest session" }), a.verification ? chip(a.status) : h("span", { class: "pop-chip info", text: phase })),
      h("dl", { class: "vm-dl" },
        h("dt", { text: "Session" }), h("dd", { text: a.session_id }),
        h("dt", { text: "Expected ad" }), h("dd", { text: mediaName(a.expected_media_id) }),
        h("dt", { text: "Actual ad (player report)" }), h("dd", { text: a.played_media_id ? mediaName(a.played_media_id) : "not reported yet" }),
        h("dt", { text: "Started" }), h("dd", null, started ? U.tsEl(a.start_utc_ms) : DASH),
        h("dt", { text: "Elapsed / slot" }), h("dd", { text: started ? fmtMs(elapsed) + " / " + fmtMs(slot) : DASH }),
        h("dt", { text: "Evidence" }), h("dd", { text: `${a.evidence_count || 0} of ${a.captures_requested || 0} captured (${a.evidence_ok || 0} usable)` + (a.capture_backend ? " · " + a.capture_backend : "") }),
        h("dt", { text: "Status" }), h("dd", { text: a.verification ? (a.status_label + (a.reason ? " — " + a.reason : "")) : phase })),
      bar,
      h("div", { class: "vm-row", style: "gap:8px;margin-top:8px" }, h("button", { class: "vm-btn small", type: "button", text: "Open evidence and scores", onclick: () => openDetail(a.session_id) })));
  }

  // ---- history -------------------------------------------------------------------------------------------------
  function renderHistory() {
    const sec = S.view.hist;
    const fm = h("select", { "aria-label": "Filter by media", onchange: (e) => { S.filter.media_id = e.target.value; refresh(true); } }, h("option", { value: "", text: "All media" }),
      ...S.media.map((m) => h("option", { value: m.media_id, selected: S.filter.media_id === m.media_id, text: m.display_name || m.media_id })));
    const fs = h("select", { "aria-label": "Filter by status", onchange: (e) => { S.filter.status = e.target.value; refresh(true); } }, h("option", { value: "", text: "All statuses" }),
      ...Object.keys(STATUS).map((k) => h("option", { value: k, selected: S.filter.status === k, text: STATUS[k][0] })));
    const q = new URLSearchParams(); if (S.filter.media_id) q.set("media_id", S.filter.media_id); if (S.filter.status) q.set("status", S.filter.status);
    const csv = h("a", { class: "vm-btn small", href: POP + "/sessions/export.csv" + (q.toString() ? "?" + q : ""), download: "proof_of_play.csv", text: "Export CSV" });
    const json = h("button", { class: "vm-btn small", type: "button", text: "Export JSON", onclick: async () => {
      const r = await papi("/sessions?limit=500" + (S.filter.media_id ? "&media_id=" + encodeURIComponent(S.filter.media_id) : "") + (S.filter.status ? "&status=" + encodeURIComponent(S.filter.status) : ""));
      if (r.status !== 200) { alert("Could not export: " + msg(r)); return; }
      const blob = new Blob([JSON.stringify(r.data, null, 2)], { type: "application/json" });
      const a = h("a", { href: URL.createObjectURL(blob), download: "proof_of_play.json" }); document.body.append(a); a.click(); a.remove();
    } });
    const rows = S.sessions.map((s) => {
      const v = s.verification || {};
      const tr = h("tr", { class: "pop-row", tabindex: "0", onclick: () => openDetail(s.session_id), onkeydown: (e) => { if (e.key === "Enter") openDetail(s.session_id); } },
        h("td", null, U.tsEl(s.start_utc_ms || s.scheduled_utc_ms)),
        h("td", null, h("div", { text: s.session_id }), h("div", { class: "muted", text: "expected " + s.expected_media_id + (s.played_media_id && s.played_media_id !== s.expected_media_id ? " · played " + s.played_media_id : "") })),
        h("td", null, !s.verification && s.start_utc_ms > 0 ? h("span", { class: "pop-chip info", text: s.end_utc_ms > 0 ? "Verifying" : "Playing" }) : chip(s.status)),
        h("td", { class: "pop-reason", text: s.reason || "" }),
        h("td", { class: "muted", text: s.capture_backend === "resilience_grpc" ? "gRPC (Weston/VOP2)" : s.capture_backend === "brightscript_screenshot" ? "screenshot" : DASH }),
        h("td", { class: "num" }, s.start_utc_ms > 0 ? U.tsEl(s.start_utc_ms) : DASH, h("div", { class: "muted", text: s.end_utc_ms > 0 ? "end " + new Date(s.end_utc_ms).toLocaleTimeString() : "" })),
        h("td", { class: "num", text: s.actual_play_ms >= 0 ? fmtMs(s.actual_play_ms) + " / " + fmtMs(s.slot_ms) : DASH }),
        h("td", { class: "num", text: (s.evidence_count || 0) + (v.frames_ok !== undefined ? ` · ${v.frames_matched}/${v.frames_ok} match` : "") + (v.frames_failed ? ` (+${v.frames_failed} failed)` : "") }),
        h("td", { class: "num", text: v.mean_sim_expected !== undefined && v.frames_ok ? sim(v.mean_sim_expected) : DASH }));
      return tr;
    });
    const more = S.sessions.length < S.total ? h("button", { class: "vm-btn small", type: "button", text: `Load more (${S.sessions.length} of ${S.total})`, onclick: () => { S.limit += 25; refresh(true); } }) : null;
    sec.replaceChildren(h("div", { class: "panel-head" }, h("h3", { text: "Playback history" }), h("span", { class: "vm-row", style: "gap:8px" }, fm, fs, csv, json)),
      S.sessions.length ? h("div", { class: "pop-scroll" }, h("table", { class: "pop-table" },
        h("thead", null, h("tr", null, ...["Time", "Session", "Status", "Reason", "Backend", "Start / end", "Played / slot", "Evidence", "Mean sim."].map((t) => h("th", { text: t })))), h("tbody", null, ...rows)))
        : h("div", { class: "vm-empty", text: "No playback sessions yet." }), ...(more ? [more] : []));
  }

  // ---- verification detail -------------------------------------------------------------------------------------
  async function openDetail(id) {
    const r = await papi("/sessions/" + encodeURIComponent(id));
    if (r.status !== 200) { alert("Could not load the session: " + msg(r)); return; }
    const d = r.data;
    const vd = d.verification_detail || {};
    const frameScore = {}; (vd.frames || []).forEach((f) => { frameScore[f.evidence_id] = f; });
    // Reference frames come in two profiles: the screenshot profile and the display-output profile (for the OS rendered frames); the evidence is
    // always compared with the references of its own capture method.
    const isGrpc = (e) => e.capture_method === "resilience_grpc";
    const refsCache = {};
    async function refsOf(mediaId, grpc) {
      const key = mediaId + (grpc ? ".display_letterbox" : "");
      if (!refsCache[key]) {
        const rr = await papi("/media/" + encodeURIComponent(key) + "/references");
        refsCache[key] = {}; if (rr.status === 200) (rr.data.references || []).forEach((f) => { refsCache[key][f.offset_ms] = f; });
      }
      return refsCache[key];
    }
    const need = [];
    for (const e of d.evidence || []) {
      const sc0 = frameScore[e.evidence_id]; if (!sc0) continue;
      need.push(refsOf(d.expected_media_id, isGrpc(e)));
      if (sc0.best_other_media && (sc0.verdict === "mismatch" || sc0.sim_other > sc0.sim_expected)) need.push(refsOf(sc0.best_other_media, isGrpc(e)));
    }
    await Promise.all(need);

    const head = h("div", { class: "pop-detail-head" },
      chip(d.status), h("div", null, h("div", { class: "pop-reason-big", text: d.reason || "" }),
        h("div", { class: "muted", text: `Session ${d.session_id} · expected ${d.expected_media_id}${d.played_media_id ? " · player reported " + d.played_media_id : ""}` })));
    const facts = h("dl", { class: "vm-dl" },
      h("dt", { text: "Played / slot" }), h("dd", { text: d.actual_play_ms >= 0 ? fmtMs(d.actual_play_ms) + " / " + fmtMs(d.slot_ms) : DASH }),
      h("dt", { text: "Ended because" }), h("dd", { text: d.end_reason || DASH }),
      h("dt", { text: "Started" }), h("dd", null, d.start_utc_ms ? U.tsEl(d.start_utc_ms) : DASH),
      h("dt", { text: "Capture backend" }), h("dd", { text: d.capture_backend || DASH }),
      h("dt", { text: "Evidence frames" }), h("dd", { text: `${vd.frames_ok ?? 0} usable · ${vd.frames_matched ?? 0} match · ${vd.frames_mismatched ?? 0} mismatch · ${vd.frames_failed ?? 0} failed` }),
      h("dt", { text: "Mean / min similarity" }), h("dd", { text: vd.frames_ok ? `${sim(vd.mean_sim_expected)} / ${sim(vd.min_sim_expected)}` : DASH }),
      h("dt", { text: "Engine" }), h("dd", { text: vd.engine_version || DASH }));

    const uncTxt = (e) => (e.time_uncertainty_ms == null ? "not reported" : "≤ " + fmtMs(e.time_uncertainty_ms));
    const cards = (d.evidence || []).map((e) => {
      const sc = frameScore[e.evidence_id];
      const grpc = isGrpc(e);
      const ref = sc && (refsCache[d.expected_media_id + (grpc ? ".display_letterbox" : "")] || {})[sc.best_ref_offset_ms];
      const oref = sc && sc.best_other_media && sc.best_other_ref_offset_ms >= 0 && (sc.verdict === "mismatch" || sc.sim_other > sc.sim_expected)
        ? (refsCache[sc.best_other_media + (grpc ? ".display_letterbox" : "")] || {})[sc.best_other_ref_offset_ms] : null;
      const kind = grpc ? "Weston/VOP2 display frame" : "Display screenshot";
      const left = e.status === "ok" && e.image_available
        ? h("img", { class: "pop-img", src: e.image_url, alt: `${kind} ${e.idx}`, loading: "lazy" })
        : h("div", { class: "pop-noimg", text: e.status === "failed" ? "Capture failed: " + (e.failure_reason || "no image") : "Image no longer retained" });
      const right = ref ? h("img", { class: "pop-img", src: ref.image_url, alt: "Closest reference frame", loading: "lazy" })
        : h("div", { class: "pop-noimg", text: sc ? "No reference frame" : DASH });
      const verdict = sc ? h("span", { class: "pop-chip " + (sc.verdict === "match" ? "ok" : sc.verdict === "mismatch" ? "bad" : "warn"), text: sc.verdict }) : h("span", { class: "pop-chip", text: e.status });
      const figs = [h("figure", null, left, h("figcaption", { text: kind })),
        h("figure", null, right, h("figcaption", { text: ref ? `Closest reference of ${d.expected_media_id} @ ${fmtMs(sc.best_ref_offset_ms)} · similarity ${sim(sc.sim_expected)}` : "Reference" }))];
      if (oref) figs.push(h("figure", null, h("img", { class: "pop-img", src: oref.image_url, alt: "Closest reference frame of the other advertisement", loading: "lazy" }),
        h("figcaption", { text: `Closest reference of ${sc.best_other_media} @ ${fmtMs(sc.best_other_ref_offset_ms)} · similarity ${sim(sc.sim_other)}` })));
      return h("div", { class: "pop-ev" },
        h("div", { class: "pop-ev-head" }, h("b", { text: `#${e.idx}` }), h("span", { class: "muted", text: ` at ${fmtMs(e.actual_offset_ms)} (target ${fmtMs(e.target_offset_ms)}, ${e.took_ms} ms)` }), verdict),
        h("div", { class: "pop-cmp" }, ...figs),
        sc ? h("div", { class: "muted", text: `similarity to expected ${sim(sc.sim_expected)} · pHash distance ${sc.phash_dist_expected}` +
          (sc.best_other_media ? ` · best other media: ${sc.best_other_media} ${sim(sc.sim_other)}` : "") }) : null,
        h("div", { class: "muted", text: `${e.capture_method || "capture"} · captured ${e.utc_ms ? new Date(e.utc_ms).toLocaleTimeString() : DASH} · time uncertainty ${uncTxt(e)}` +
          (e.provenance ? " · " + e.provenance : "") }));
    });
    // Why this result: the engine's own numbers next to the thresholds in force (nothing is recomputed or invented here).
    const th = (S.status && S.status.thresholds) || {};
    const why = [];
    why.push(h("li", { text: `Result: ${d.status_label || d.status}. ${d.reason || ""}` }));
    if (d.actual_play_ms >= 0 && d.slot_ms) why.push(h("li", { text: `Playback: ${fmtMs(d.actual_play_ms)} of a ${fmtMs(d.slot_ms)} slot (${Math.round(100 * d.actual_play_ms / d.slot_ms)} %); complete needs at least ${Math.round(100 * (th.min_play_ratio || 0.9))} % and an ordinary end (${d.end_reason || "no end reason"}).` }));
    else why.push(h("li", { text: "No playback end was reported by the player, so playback cannot be called complete." }));
    if (vd.frames_ok) why.push(h("li", { text: `Evidence: ${vd.frames_matched} of ${vd.frames_ok} usable frames match the expected advertisement (a frame matches at similarity ≥ ${(th.match_sim ?? 0.8).toFixed(2)}, mismatches below ${(th.reject_sim ?? 0.6).toFixed(2)} or when another registered advertisement matches clearly better); a corroborated play needs at least ${Math.round(100 * (th.min_match_fraction || 0.6))} % matching frames.` }));
    else why.push(h("li", { text: "No usable evidence frame, so no visual verdict can be given." }));
    if ((d.evidence || []).some((e) => e.status === "failed")) why.push(h("li", { text: "Failed captures: " + (d.evidence || []).filter((e) => e.status === "failed").map((e) => `#${e.idx} ${e.failure_reason || ""}`).join("; ") }));
    why.push(h("li", { text: "A frame near the start or end of the play is not used when the display timing is uncertain; thresholds are in force as configured (0.80/0.60 are initial values, not a calibration)." }));
    const t0 = (d.events || []).length ? d.events[0].mono_ms : 0;
    const timeline = h("ol", { class: "pop-timeline" }, ...(d.events || []).map((e) => h("li", null,
      h("span", { class: "muted", text: `+${((e.mono_ms - t0) / 1000).toFixed(2)} s ` }), h("b", { text: e.type }),
      h("span", { class: "muted", text: e.detail && e.detail.reason ? " — " + e.detail.reason : (e.detail && e.detail.vcode !== undefined ? ` — video event ${e.detail.vcode}` : "") }))));
    const reverify = h("button", { class: "vm-btn small", type: "button", text: "Re-verify", onclick: async () => { const x = await post("/sessions/" + encodeURIComponent(id) + "/verify"); if (x.status === 200) { S.modal && S.modal.close(); openDetail(id); refresh(true); } else alert(msg(x)); } });
    const exp = h("button", { class: "vm-btn small", type: "button", text: "Export JSON", onclick: () => {
      const blob = new Blob([JSON.stringify(d, null, 2)], { type: "application/json" });
      const a = h("a", { href: URL.createObjectURL(blob), download: `proof_of_play_${d.session_id}.json` }); document.body.append(a); a.click(); a.remove();
    } });
    const body = h("div", null, head, h("div", { class: "vm-row", style: "gap:8px;margin:10px 0" }, reverify, exp), facts,
      h("h3", { text: "Why this result" }), h("ul", { class: "pop-why" }, ...why), h("h3", { text: "Evidence: display frames vs reference frames" }), cards.length ? h("div", { class: "pop-evs" }, ...cards) : h("div", { class: "vm-empty", text: "No evidence frames were captured for this session." }),
      h("h3", { text: "Playback events (player clock)" }), timeline.children.length ? timeline : h("div", { class: "vm-empty", text: "No playback events received." }),
      h("p", { class: "muted", text: "Event times use the player's monotonic clock; wall-clock time is assigned when the event is read (accuracy about a quarter of a second)." }));
    S.modal = U.openModal("Proof of Play — " + d.session_id, body, { wide: true, onClose: () => { S.modal = null; } });
  }

  // ---- data ----------------------------------------------------------------------------------------------------
  async function refresh(force) {
    if (S.modal && !force) return;
    const st = await papi("/status");
    S.status = st.status === 200 ? st.data : null;
    if (S.status && S.status.enabled) {
      const m = await papi("/media"); if (m.status === 200) S.media = m.data.media;
      const q = new URLSearchParams({ limit: String(S.limit) });
      if (S.filter.media_id) q.set("media_id", S.filter.media_id); if (S.filter.status) q.set("status", S.filter.status);
      const s = await papi("/sessions?" + q);
      if (s.status === 200) { S.sessions = s.data.sessions; S.total = s.data.total; if (s.data.now_utc_ms) S.nowSkew = s.data.now_utc_ms - Date.now(); }
    } else { S.media = []; S.sessions = []; S.total = 0; }
    const wasBusy = !!S.active; S.active = activeSession();
    renderBanner(); renderProgress();
    const hsig = JSON.stringify([S.sessions.map((x) => [x.session_id, x.status, x.evidence_count, x.evidence_ok, x.end_utc_ms]), S.total, S.filter, S.media.length]);
    if (force === true || hsig !== S.hsig) { S.hsig = hsig; renderHistory(); }
    const sig = JSON.stringify([S.status && S.status.enabled, S.status && S.status.allow_test_jobs, S.status && S.status.capture_backend, !!S.active, S.media.map((m) => [m.media_id, m.reference_frames])]);
    if (force === true || sig !== S.sig) { S.sig = sig; renderSchedule(); renderRegistry(); renderDemo(); }      // keep the form's selections between polls
    const live = document.getElementById("popLive");
    if (live) live.textContent = "updated " + new Date().toLocaleTimeString();
  }

  function show() {
    if (!S.view) build();
    refresh(true);
    clearInterval(S.timer);
    // 1 s while a session is in progress (live progress), 5 s otherwise
    S.timer = setInterval(() => {
      if (document.body.dataset.view !== "pop") { clearInterval(S.timer); return; }
      if (S.active || ++S.tick % 5 === 0) refresh(false);
      else if (S.view) renderProgress();
    }, 1000);
  }
  U.registerPop(show);
})();
