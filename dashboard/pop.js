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
  const S = { view: null, media: [], sessions: [], total: 0, status: null, filter: { media_id: "", status: "" }, limit: 25, timer: null, modal: null };

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
      hist: h("section", { class: "panel pop-panel" }),
    };
    root.replaceChildren(h("div", { class: "vm-page" },
      h("div", { class: "panel-head" }, h("h2", { text: "Proof of Play" }), h("span", { class: "muted", id: "popLive", text: "" })),
      S.view.banner, S.view.sched, S.view.hist, S.view.reg));
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
        h("span", { class: "vm-chip" }, "Media", h("b", { text: String(s.media ?? 0) })),
        h("span", { class: "vm-chip" }, "Sessions", h("b", { text: String(s.sessions ?? 0) })),
        h("span", { class: "vm-chip" }, "Screenshots", h("b", { text: String(s.evidence ?? 0) })),
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
      h("td", { class: "muted", text: m.sha256 ? m.sha256.slice(0, 12) + "…" : DASH }),
      h("td", { class: "muted", text: m.uri })));
    sec.replaceChildren(h("div", { class: "panel-head" }, h("h3", { text: "Advertisement registry" })),
      S.media.length ? h("div", { class: "pop-scroll" }, h("table", { class: "pop-table" },
        h("thead", null, h("tr", null, ...["Media", "Kind", "Duration", "Slot", "Reference frames", "SHA-256", "Source"].map((t) => h("th", { text: t })))), h("tbody", null, ...rows)))
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

  // ---- history -------------------------------------------------------------------------------------------------
  function renderHistory() {
    const sec = S.view.hist;
    const fm = h("select", { "aria-label": "Filter by media", onchange: (e) => { S.filter.media_id = e.target.value; refresh(true); } }, h("option", { value: "", text: "All media" }),
      ...S.media.map((m) => h("option", { value: m.media_id, selected: S.filter.media_id === m.media_id, text: m.display_name || m.media_id })));
    const fs = h("select", { "aria-label": "Filter by status", onchange: (e) => { S.filter.status = e.target.value; refresh(true); } }, h("option", { value: "", text: "All statuses" }),
      ...Object.keys(STATUS).map((k) => h("option", { value: k, selected: S.filter.status === k, text: STATUS[k][0] })));
    const q = new URLSearchParams(); if (S.filter.media_id) q.set("media_id", S.filter.media_id); if (S.filter.status) q.set("status", S.filter.status);
    const csv = h("a", { class: "vm-btn small", href: POP + "/sessions/export.csv" + (q.toString() ? "?" + q : ""), download: "proof_of_play.csv", text: "Export CSV" });
    const rows = S.sessions.map((s) => {
      const v = s.verification || {};
      const tr = h("tr", { class: "pop-row", tabindex: "0", onclick: () => openDetail(s.session_id), onkeydown: (e) => { if (e.key === "Enter") openDetail(s.session_id); } },
        h("td", null, U.tsEl(s.start_utc_ms || s.scheduled_utc_ms)),
        h("td", null, h("div", { text: s.session_id }), h("div", { class: "muted", text: "expected " + s.expected_media_id + (s.played_media_id && s.played_media_id !== s.expected_media_id ? " · played " + s.played_media_id : "") })),
        h("td", null, chip(s.status)),
        h("td", { class: "pop-reason", text: s.reason || "" }),
        h("td", { class: "num", text: s.actual_play_ms >= 0 ? fmtMs(s.actual_play_ms) + " / " + fmtMs(s.slot_ms) : DASH }),
        h("td", { class: "num", text: v.frames_ok !== undefined ? `${v.frames_matched}/${v.frames_ok}` + (v.frames_failed ? ` (+${v.frames_failed} failed)` : "") : DASH }),
        h("td", { class: "num", text: v.mean_sim_expected !== undefined && v.frames_ok ? sim(v.mean_sim_expected) : DASH }));
      return tr;
    });
    const more = S.sessions.length < S.total ? h("button", { class: "vm-btn small", type: "button", text: `Load more (${S.sessions.length} of ${S.total})`, onclick: () => { S.limit += 25; refresh(true); } }) : null;
    sec.replaceChildren(h("div", { class: "panel-head" }, h("h3", { text: "Playback history" }), h("span", { class: "vm-row", style: "gap:8px" }, fm, fs, csv)),
      S.sessions.length ? h("div", { class: "pop-scroll" }, h("table", { class: "pop-table" },
        h("thead", null, h("tr", null, ...["Time", "Session", "Status", "Reason", "Played / slot", "Matched / ok", "Mean sim."].map((t) => h("th", { text: t })))), h("tbody", null, ...rows)))
        : h("div", { class: "vm-empty", text: "No playback sessions yet." }), ...(more ? [more] : []));
  }

  // ---- verification detail -------------------------------------------------------------------------------------
  async function openDetail(id) {
    const r = await papi("/sessions/" + encodeURIComponent(id));
    if (r.status !== 200) { alert("Could not load the session: " + msg(r)); return; }
    const d = r.data;
    const vd = d.verification_detail || {};
    const frameScore = {}; (vd.frames || []).forEach((f) => { frameScore[f.evidence_id] = f; });
    const refByOffset = {}; (d.reference_frames || []).forEach((f) => { refByOffset[f.offset_ms] = f; });

    const head = h("div", { class: "pop-detail-head" },
      chip(d.status), h("div", null, h("div", { class: "pop-reason-big", text: d.reason || "" }),
        h("div", { class: "muted", text: `Session ${d.session_id} · expected ${d.expected_media_id}${d.played_media_id ? " · player reported " + d.played_media_id : ""}` })));
    const facts = h("dl", { class: "vm-dl" },
      h("dt", { text: "Played / slot" }), h("dd", { text: d.actual_play_ms >= 0 ? fmtMs(d.actual_play_ms) + " / " + fmtMs(d.slot_ms) : DASH }),
      h("dt", { text: "Ended because" }), h("dd", { text: d.end_reason || DASH }),
      h("dt", { text: "Started" }), h("dd", null, d.start_utc_ms ? U.tsEl(d.start_utc_ms) : DASH),
      h("dt", { text: "Screenshots" }), h("dd", { text: `${vd.frames_ok ?? 0} usable · ${vd.frames_matched ?? 0} match · ${vd.frames_mismatched ?? 0} mismatch · ${vd.frames_failed ?? 0} failed` }),
      h("dt", { text: "Mean / min similarity" }), h("dd", { text: vd.frames_ok ? `${sim(vd.mean_sim_expected)} / ${sim(vd.min_sim_expected)}` : DASH }),
      h("dt", { text: "Engine" }), h("dd", { text: vd.engine_version || DASH }));

    const cards = (d.evidence || []).map((e) => {
      const sc = frameScore[e.evidence_id];
      const ref = sc && refByOffset[sc.best_ref_offset_ms];
      const left = e.status === "ok" && e.image_available
        ? h("img", { class: "pop-img", src: e.image_url, alt: `Screenshot ${e.idx} of the display output`, loading: "lazy" })
        : h("div", { class: "pop-noimg", text: e.status === "failed" ? "Capture failed: " + (e.failure_reason || "no image") : "Screenshot no longer retained" });
      const right = ref ? h("img", { class: "pop-img", src: ref.image_url, alt: "Closest reference frame", loading: "lazy" })
        : h("div", { class: "pop-noimg", text: sc ? "No reference frame" : DASH });
      const verdict = sc ? h("span", { class: "pop-chip " + (sc.verdict === "match" ? "ok" : sc.verdict === "mismatch" ? "bad" : "warn"), text: sc.verdict }) : h("span", { class: "pop-chip", text: e.status });
      return h("div", { class: "pop-ev" },
        h("div", { class: "pop-ev-head" }, h("b", { text: `#${e.idx}` }), h("span", { class: "muted", text: ` at ${fmtMs(e.actual_offset_ms)} (target ${fmtMs(e.target_offset_ms)}, ${e.took_ms} ms)` }), verdict),
        h("div", { class: "pop-cmp" }, h("figure", null, left, h("figcaption", { text: "Display screenshot" })),
          h("figure", null, right, h("figcaption", { text: ref ? `Closest reference of ${d.expected_media_id} @ ${fmtMs(sc.best_ref_offset_ms)}` : "Reference" }))),
        sc ? h("div", { class: "muted", text: `similarity ${sim(sc.sim_expected)} · pHash distance ${sc.phash_dist_expected}` +
          (sc.best_other_media ? ` · best other media: ${sc.best_other_media} ${sim(sc.sim_other)}` : "") }) : null);
    });
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
      h("h3", { text: "Evidence: display screenshots vs reference frames" }), cards.length ? h("div", { class: "pop-evs" }, ...cards) : h("div", { class: "vm-empty", text: "No screenshots were captured for this session." }),
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
      if (s.status === 200) { S.sessions = s.data.sessions; S.total = s.data.total; }
    } else { S.media = []; S.sessions = []; S.total = 0; }
    renderBanner(); renderHistory();
    const sig = JSON.stringify([S.status && S.status.enabled, S.status && S.status.allow_test_jobs, S.media.map((m) => [m.media_id, m.reference_frames])]);
    if (force === true || sig !== S.sig) { S.sig = sig; renderSchedule(); renderRegistry(); }      // keep the form's selections between polls
    const live = document.getElementById("popLive");
    if (live) live.textContent = "updated " + new Date().toLocaleTimeString();
  }

  function show() {
    if (!S.view) build();
    refresh(true);
    clearInterval(S.timer);
    S.timer = setInterval(() => { if (document.body.dataset.view !== "pop") { clearInterval(S.timer); return; } refresh(false); }, 5000);
  }
  U.registerPop(show);
})();
