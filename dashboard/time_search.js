/* Argus dashboard: "Search by time" for the BrightSign Display (Weston/VOP2) source of Visual Search.
   The user picks a date, a time and a timezone; the page converts that wall-clock time to a UTC instant in the browser (Intl, DST-aware, with
   explicit handling of ambiguous and non-existent local times) and asks the player:
     GET /api/display/history/nearest?time=<ISO-8601 Z>&max_diff_ms=...   the single nearest stored display frame
     GET /api/display/history/search?time=<ISO-8601 Z>&window_s=...        every PoP session overlapping the window, with its stored frames
   Both read the Proof-of-Play database only (stored timestamps and vectors; no model). Everything the server sends is rendered with textContent.
   Wording rule: what the PLAYER REPORTED and what the stored frames SHOW are separate blocks, and an empty answer says "no retained evidence",
   never "nothing was displayed". */
(() => {
  "use strict";
  if (!window.VMUI) return;
  const U = window.VMUI, h = U.h, isNum = U.isNum, DASH = U.DASH, fmtInt = U.fmtInt;
  const DISPLAY = "/api/display/history";
  const STATUS_CLASS = { playback_corroborated: "ok", visual_match: "ok", mismatch: "bad", incomplete: "warn", capture_unsupported: "warn", playback_reported: "info" };
  const E = {}, TS = { info: null, built: false, ctrl: null, last: null };
  const BROWSER_TZ = (() => { try { return Intl.DateTimeFormat().resolvedOptions().timeZone || "UTC"; } catch (_) { return "UTC"; } })();
  const p2 = (n) => String(n).padStart(2, "0");

  // ---- timezone arithmetic ------------------------------------------------------------------------------------------------------------------------
  function dtf(tz) {
    return new Intl.DateTimeFormat("en-US", { timeZone: tz, hourCycle: "h23", year: "numeric", month: "2-digit", day: "2-digit", hour: "2-digit", minute: "2-digit", second: "2-digit" });
  }
  // offset of `tz` from UTC at the instant utcMs, in ms (local - utc)
  function zoneOffsetMs(utcMs, tz) {
    if (tz === "UTC") return 0;
    const p = {}; dtf(tz).formatToParts(new Date(Math.floor(utcMs / 1000) * 1000)).forEach((x) => { p[x.type] = x.value; });
    return Date.UTC(+p.year, +p.month - 1, +p.day, +p.hour, +p.minute, +p.second) - Math.floor(utcMs / 1000) * 1000;
  }
  // wall-clock time in tz -> UTC. {utc} | {utc, other, ambiguous} (clocks went back: two instants) | {utc, gap} (clocks went forward: no such time)
  function wallToUtc(y, mo, d, hh, mi, ss, tz) {
    const naive = Date.UTC(y, mo - 1, d, hh, mi, ss), day = 86400000;
    const offs = new Set([zoneOffsetMs(naive - day, tz), zoneOffsetMs(naive + day, tz)]);
    const c = [];
    offs.forEach((o) => { const u = naive - o; if (zoneOffsetMs(u, tz) === o) c.push(u); });
    c.sort((a, b) => a - b);
    if (c.length === 1) return { utc: c[0] };
    if (c.length === 2) return { utc: c[0], other: c[1], ambiguous: true };
    return { utc: naive - zoneOffsetMs(naive - day, tz), gap: true };
  }
  function fmtZoned(ms, tz, withMs) {
    const off = zoneOffsetMs(ms, tz), d = new Date(ms + off);
    const base = `${d.getUTCFullYear()}-${p2(d.getUTCMonth() + 1)}-${p2(d.getUTCDate())} ${p2(d.getUTCHours())}:${p2(d.getUTCMinutes())}:${p2(d.getUTCSeconds())}` + (withMs ? "." + String(((ms % 1000) + 1000) % 1000).padStart(3, "0") : "");
    const m = Math.round(off / 60000), sgn = m < 0 ? "-" : "+", a = Math.abs(m);
    return `${base} (UTC${sgn}${p2(Math.floor(a / 60))}:${p2(a % 60)}${tz === "UTC" ? "" : " " + tz})`;
  }
  const isoUtc = (ms) => new Date(ms).toISOString();

  // ---- API ------------------------------------------------------------------------------------------------------------------------------------------
  async function dget(path, signal) {
    try {
      const r = await fetch(DISPLAY + path, { cache: "no-store", credentials: "same-origin", signal });
      let data = null; try { data = await r.json(); } catch (_) {}
      return { status: r.status, data };
    } catch (e) { return { status: 0, data: null, aborted: !!(e && e.name === "AbortError") }; }
  }
  function errText(r) {
    const d = r.data || {};
    if (r.status === 0) return "Network error: could not reach the dashboard server.";
    if (d.error === "pop_disabled") return "Proof of Play is not enabled on this player, so there is no display history to look up.";
    if (r.status === 404) return "This player's package has no time lookup yet (older version).";
    return "The lookup failed (HTTP " + r.status + ")." + (d.message ? " " + d.message : "");
  }

  // ---- inputs ---------------------------------------------------------------------------------------------------------------------------------------
  function zones() {
    const dev = TS.info && TS.info.device_timezone ? TS.info.device_timezone.name : "";
    const z = [];
    if (dev) z.push(["device", dev, `Device timezone (${dev})`]);
    if (!dev || dev !== BROWSER_TZ) z.push(["browser", BROWSER_TZ, `This browser (${BROWSER_TZ})`]);
    z.push(["utc", "UTC", "UTC"]);
    z.push(["other", "", "Other timezone…"]);
    return z;
  }
  const chosenTz = () => {
    const v = E.tz.value;
    if (v === "other") return (E.tzOther.value || "").trim();
    const z = zones().find((x) => x[0] === v);
    return z ? z[1] : "UTC";
  };
  function resolveInput() {
    const tz = chosenTz();
    try { dtf(tz); } catch (_) { return { error: `"${tz}" is not a timezone name this browser knows (use an IANA name such as Europe/London).` }; }
    const dm = /^(\d{4})-(\d{2})-(\d{2})$/.exec(E.date.value || ""), tm = /^(\d{2}):(\d{2})(?::(\d{2}))?$/.exec(E.time.value || "");
    if (!dm || !tm) return { error: "Choose a date and a time." };
    const r = wallToUtc(+dm[1], +dm[2], +dm[3], +tm[1], +tm[2], +(tm[3] || 0), tz);
    r.tz = tz;
    if (r.ambiguous && E.pick.value === "second") { const t = r.utc; r.utc = r.other; r.other = t; r.picked = "second"; }
    return r;
  }
  function updateResolved() {
    E.resolved.replaceChildren();
    const r = resolveInput();
    if (r.error) { E.resolved.append(h("span", { class: "muted", text: r.error })); E.pickRow.hidden = true; E.run.disabled = true; return null; }
    E.run.disabled = false;
    E.resolved.append(h("span", { class: "muted", text: "Searches the instant " }), h("b", { "data-role": "resolved-utc", text: isoUtc(r.utc) }),
      h("span", { class: "muted", text: "  ·  " + fmtZoned(r.utc, r.tz, false) }));
    E.pickRow.hidden = !r.ambiguous;
    if (r.ambiguous) E.resolved.append(h("div", { class: "vm-banner warn", "data-role": "tz-ambiguous", text: `That local time happens twice (clocks go back): ${isoUtc(r.utc)} and ${isoUtc(r.other)}. Pick which one to search.` }));
    if (r.gap) E.resolved.append(h("div", { class: "vm-banner warn", "data-role": "tz-gap", text: "That local time does not exist (clocks go forward). The first instant after the change is searched." }));
    return r;
  }
  function toDateTime(ms, tz) {
    const off = zoneOffsetMs(ms, tz), d = new Date(ms + off);
    return [`${d.getUTCFullYear()}-${p2(d.getUTCMonth() + 1)}-${p2(d.getUTCDate())}`, `${p2(d.getUTCHours())}:${p2(d.getUTCMinutes())}:${p2(d.getUTCSeconds())}`];
  }
  function setTime(ms) {
    const [d, t] = toDateTime(ms, chosenTz());
    E.date.value = d; E.time.value = t; E.pick.value = "first"; updateResolved();
  }

  // ---- rendering ------------------------------------------------------------------------------------------------------------------------------------
  const simText = (s) => (s && s.available ? s.similarity_to_expected.toFixed(4) : null);
  function frameImg(f, sess, tz) {
    const url = f.image && f.image.available ? f.image.url : null;
    if (!url) return h("div", { class: "vm-empty ts-expired", "data-role": "image-expired", text: "JPEG expired: retention removed the image. The time, session and score below are kept." });
    const sim = simText(f.similarity);
    const img = h("img", { src: url, alt: "Stored display frame", loading: "lazy", "data-zoom": "1",
      "data-title": (f.is_verification_checkpoint ? "Verification checkpoint" : "History sample") + " · " + (f.expected_media_id || "") + (isNum(f.playback_offset_ms) ? " · " + (f.playback_offset_ms / 1000).toFixed(1) + " s into the play" : ""),
      "data-meta": [sim ? "similarity to the expected advertisement " + sim : "", "recorded " + f.recorded_utc + "  ·  " + fmtZoned(f.recorded_utc_ms, tz, true), "session " + f.session_id,
        "frame " + (f.frame_id ? "history #" + f.frame_id : "checkpoint #" + f.evidence_id) + " · " + fmtInt(f.width) + "×" + fmtInt(f.height)].filter(Boolean).join("|") });
    img.addEventListener("error", () => img.replaceWith(h("div", { class: "vm-empty", text: "JPEG no longer available" })));
    return img;
  }
  function timingLine(f) {
    const d = f.time_diff_ms, dir = d === 0 ? "exactly at the requested time" : Math.abs(d) + " ms " + (d < 0 ? "before" : "after") + " the requested time";
    const u = isNum(f.time_uncertainty_ms) ? "capture time ±" + f.time_uncertainty_ms + " ms" : "capture time has no stated bound (approximate)";
    return dir + " · " + u;
  }
  function frameMeta(f, tz) {
    const sm = f.similarity || {}, prov = f.provenance || {};
    const rows = [
      ["Frame", f.frame_id ? `history frame #${f.frame_id} (sample ${f.index})` + (f.evidence_id ? ` · also verification checkpoint #${f.evidence_id}` : "") : `verification checkpoint #${f.evidence_id}`],
      ["Recorded (UTC)", f.recorded_utc], ["Recorded (selected zone)", fmtZoned(f.recorded_utc_ms, tz, true)],
      ["Requested", f.requested_utc], ["Time difference", timingLine(f)],
      ["Timestamp quality", f.timestamp_quality === "bounded" ? "bounded (±" + f.time_uncertainty_ms + " ms)" : "unknown bound"],
      ["Within the play", isNum(f.playback_offset_ms) ? (f.playback_offset_ms / 1000).toFixed(2) + " s after the player-reported start" : DASH],
      ["Source", (prov.origin || "") + " · " + (prov.capture_method || "") + (prov.detail ? " · " + prov.detail : "")],
      ["Image", fmtInt(f.width) + " × " + fmtInt(f.height) + " · " + (f.image && f.image.available ? "available" : "expired (removed by retention)")],
      ["Similarity", sm.available ? `${sm.similarity_to_expected.toFixed(4)} to the expected advertisement's closest reference (${(sm.best_reference_offset_ms / 1000).toFixed(0)} s)` + (sm.matches_expected_reference ? " · at or above the match threshold" : " · below the match threshold") : (sm.reason ? "not available: " + sm.reason : DASH)],
    ];
    return h("dl", { class: "vm-dl ts-dl" }, ...rows.flatMap(([k, v]) => [h("dt", { text: k }), h("dd", { text: v })]));
  }
  function sessionBlocks(s) {
    const pb = s.playback || {}, vv = s.visual_verification || {}, ck = vv.checkpoints || {};
    const matches = pb.reported_matches_schedule;
    const reported = h("div", { class: "ts-col", "data-role": "player-reported" },
      h("h4", { text: "Reported by the player" }),
      h("p", null, h("span", { class: "pop-chip " + (pb.player_reported ? "info" : ""), text: pb.player_reported ? "Player reported playback" : "No playback reported" })),
      h("dl", { class: "vm-dl" },
        h("dt", { text: "Expected advertisement" }), h("dd", { "data-role": "expected-ad", text: pb.expected_media_id || DASH }),
        h("dt", { text: "Reported advertisement" }), h("dd", { "data-role": "reported-ad", text: pb.reported_media_id || "nothing reported" + (pb.state === "scheduled" ? " (not started)" : "") }),
        matches === false ? [h("dt", { text: "Schedule" }), h("dd", { class: "ts-warn", text: "the player reported a different advertisement than scheduled" })] : null,
        h("dt", { text: "Reported play" }), h("dd", null, pb.start_utc_ms ? U.tsEl(pb.start_utc_ms) : DASH, pb.end_utc_ms ? [" → ", U.tsEl(pb.end_utc_ms)] : "", " ", h("span", { class: "muted", text: pb.end_reason ? "(" + pb.end_reason + ")" : "" })),
        isNum(pb.requested_offset_ms) ? [h("dt", { text: "Requested time was" }), h("dd", { "data-role": "requested-offset", text: (pb.requested_within_reported_play ? (pb.requested_offset_ms / 1000).toFixed(1) + " s into the reported play" : "outside the reported play (" + (pb.requested_offset_ms / 1000).toFixed(1) + " s from its start)") })] : null),
      h("p", { class: "muted", text: pb.basis || "" }));
    const verdictText = { matches_expected: "The stored frames match the expected advertisement", contradicts_expected: "The stored frames show different content than expected", not_established: "Visual corroboration not established" }[vv.visual_verdict] || DASH;
    const visual = h("div", { class: "ts-col", "data-role": "visual-verification" },
      h("h4", { text: "Seen on the display (stored frames)" }),
      h("p", null, h("span", { class: "pop-chip " + (STATUS_CLASS[vv.status] || ""), "data-role": "verify-status", text: vv.status_label || DASH }), " ",
        h("span", { class: "pop-chip " + (vv.visually_corroborated ? "ok" : vv.visual_verdict === "contradicts_expected" ? "bad" : ""), "data-role": "visual-verdict", text: verdictText })),
      h("dl", { class: "vm-dl" },
        h("dt", { text: "Verification checkpoints" }), h("dd", { text: `${ck.matched || 0} of ${ck.ok || 0} matched` + (ck.mismatched ? ` · ${ck.mismatched} mismatched` : "") + (ck.failed ? ` · ${ck.failed} not captured` : "") }),
        isNum(vv.mean_sim_expected) ? [h("dt", { text: "Mean similarity" }), h("dd", { text: vv.mean_sim_expected.toFixed(4) + " (lowest " + vv.min_sim_expected.toFixed(4) + ")" })] : null,
        h("dt", { text: "Reason" }), h("dd", { text: vv.reason || DASH })),
      h("p", { class: "muted", text: vv.basis || "" }));
    return h("div", { class: "ts-cols" }, reported, visual);
  }
  function sessionCard(s, tz, nearestRef) {
    const frames = s.frames || [];
    const thumbs = h("div", { class: "pop-hist ts-frames", "data-role": "session-frames" });
    frames.forEach((f, i) => {
      const near = nearestRef && ((f.frame_id && nearestRef.frame_id === f.frame_id) || (!f.frame_id && f.evidence_id && nearestRef.evidence_id === f.evidence_id));
      const sim = simText(f.similarity);
      thumbs.append(h("figure", { class: near ? "ts-near" : "", "data-frame": String(f.frame_id || "e" + f.evidence_id) },
        frameImg(f, s, tz),
        h("figcaption", null,
          h("span", { class: "dh-role " + (f.is_verification_checkpoint ? "ck" : "hist"), text: f.is_verification_checkpoint ? "checkpoint" : "history" }),
          h("span", { text: fmtZoned(f.recorded_utc_ms, tz, false).split(" (")[0].slice(11) }),
          h("span", { class: "muted", text: (f.playback_offset_ms / 1000).toFixed(1) + " s" }),
          sim ? h("span", { class: "muted", text: "sim " + sim }) : null,
          near ? h("b", { text: "nearest" }) : null)));
    });
    return h("div", { class: "panel ts-session", "data-session": s.session_id },
      h("div", { class: "panel-head" }, h("h3", { text: "Playback session " + s.session_id }),
        h("a", { class: "vm-btn small", href: "#pop?session=" + encodeURIComponent(s.session_id), "data-session-link": s.session_id, text: "Open Proof-of-Play session" })),
      sessionBlocks(s),
      frames.length
        ? h("div", null, h("h4", { text: `Stored display frames in the window (${fmtInt(s.frames_in_window)}${s.frames_truncated ? ", showing the " + frames.length + " nearest" : ""})` }), thumbs)
        : h("div", { class: "vm-banner warn", "data-role": "no-frames-session", text: s.scheduled_but_not_started ? "This session was scheduled in the window but the player never reported it starting." : "The player reported this session, but no stored display frame of it lies in the window. It is not visually corroborated by a stored frame here (history was not recording, or the frames were removed by retention)." }));
  }
  function renderResults(near, win, tz, req) {
    const box = E.results; box.replaceChildren();
    box.append(h("p", { class: "muted", "data-role": "ts-request", text: `Requested ${isoUtc(req.utc)} · ${fmtZoned(req.utc, req.tz, false)} · window ±${req.windowS} s · nearest-frame tolerance ±${req.tol} ms` }));
    const state = (near.data && near.data.evidence_state) || "";
    // 1. the nearest frame
    if (near.status === 200 && near.data && near.data.found) {
      const f = near.data.frame, s = near.data.session;
      box.append(h("div", { class: "panel ts-nearest", "data-role": "nearest-frame", "data-state": state },
        h("div", { class: "panel-head" }, h("h2", { text: "Nearest stored display frame" }),
          h("span", { class: "pop-chip " + (f.image.available ? "ok" : "warn"), text: f.image.available ? "image available" : "image expired" })),
        h("div", { class: "vm-cmp" }, h("div", { class: "vm-thumb ds-img" }, frameImg(f, s, tz)), h("div", null, frameMeta(f, tz))),
        s ? sessionBlocks(s) : null,
        h("div", { class: "vm-row", style: "gap:8px" },
          h("a", { class: "vm-btn small", href: "#pop?session=" + encodeURIComponent(f.session_id), "data-session-link": f.session_id, text: "Open Proof-of-Play session " + f.session_id }),
          f.image.available ? h("button", { class: "vm-btn small", type: "button", "data-role": "search-with-frame", text: "Search with this frame", onclick: () => reuse(f.image.url) }) : null,
          f.image.available ? h("a", { class: "vm-btn small", href: f.image.url, target: "_blank", rel: "noopener", text: "Open JPEG" }) : null),
        f.image.available ? null : h("p", { class: "muted", text: near.data.message })));
    } else if (near.status === 200 && near.data) {
      const d = near.data;
      box.append(h("div", { class: "panel ts-noevidence", "data-role": "no-evidence", "data-state": d.evidence_state },
        h("div", { class: "panel-head" }, h("h2", { text: "No retained display evidence for that time" })),
        h("p", { "data-role": "no-evidence-message", text: d.message || "" }),
        d.nearest_outside_tolerance ? h("p", { class: "muted", text: `Nearest retained frame: ${d.nearest_outside_tolerance.recorded_utc} (${Math.abs(d.nearest_outside_tolerance.time_diff_ms)} ms ${d.nearest_outside_tolerance.time_diff_ms < 0 ? "earlier" : "later"}, session ${d.nearest_outside_tolerance.session_id}).` }) : null,
        (d.playback_sessions_at_time || []).length ? h("div", null, h("h4", { text: "Playback the player reported at that time" }), ...d.playback_sessions_at_time.map((s) => sessionCard(Object.assign({ frames: [] }, s), tz, null))) : null));
    } else box.append(h("div", { class: "vm-banner bad", text: errText(near) }));
    // 2. everything in the window
    if (win.status === 200 && win.data) {
      const sess = win.data.sessions || [];
      box.append(h("h2", { style: "margin-top:16px", text: "Playback sessions overlapping the window" }));
      box.append(h("p", { class: win.data.evidence_state === "no_evidence" ? "" : "muted", "data-role": "window-message", text: win.data.message || "" }));
      const nr = near.data && near.data.found ? { frame_id: near.data.frame.frame_id, evidence_id: near.data.frame.evidence_id } : null;
      sess.forEach((s) => box.append(sessionCard(s, tz, nr)));
      const cov = win.data.coverage || {};
      box.append(h("p", { class: "muted", "data-role": "coverage", text: `Retained display frames: ${fmtInt(cov.frames_retained)}${cov.oldest_utc ? " · from " + cov.oldest_utc + " to " + cov.newest_utc : ""} · 1 FPS history recording is ${cov.history_recording ? "on" : "off"}. Frames are stored only while a registered Proof-of-Play session plays.` }));
    } else if (win.status !== 200) box.append(h("div", { class: "vm-banner bad", text: errText(win) }));
  }
  async function reuse(url) {
    try {
      const r = await fetch(url, { credentials: "same-origin", cache: "no-store" });
      if (!r.ok) throw new Error("HTTP " + r.status);
      const b = await r.blob();
      if (!b.size || (b.type && b.type !== "image/jpeg")) throw new Error("not a JPEG");
      if (U.useDisplayReference) U.useDisplayReference(new File([b], "stored-frame.jpg", { type: "image/jpeg" }));
    } catch (e) { E.msg.textContent = "Could not use this frame as a reference: " + e.message; }
  }

  async function run() {
    if (TS.ctrl) return;
    const r = updateResolved();
    if (!r) return;
    const w = Math.floor(Number(E.window.value)), tol = Math.floor(Number(E.tol.value));
    if (!(w >= 1 && w <= 3600)) { E.msg.textContent = "The window must be 1–3600 seconds."; return; }
    if (!(tol >= 0 && tol <= 3600000)) { E.msg.textContent = "The tolerance must be 0–3600000 ms."; return; }
    E.msg.textContent = ""; TS.ctrl = new AbortController(); E.run.disabled = true; E.spin.hidden = false;
    const iso = encodeURIComponent(isoUtc(r.utc));
    try {
      const [near, win] = await Promise.all([dget(`/nearest?time=${iso}&max_diff_ms=${tol}`, TS.ctrl.signal), dget(`/search?time=${iso}&window_s=${w}`, TS.ctrl.signal)]);
      if (near.aborted || win.aborted) return;
      TS.last = { near, win };
      renderResults(near, win, r.tz, { utc: r.utc, tz: r.tz, windowS: w, tol });
    } finally { TS.ctrl = null; E.spin.hidden = true; E.run.disabled = false; }
  }

  async function loadInfo() {
    const r = await dget("/info");
    TS.info = r.status === 200 ? r.data : null;
    const keep = E.tz.value;
    E.tz.replaceChildren(...zones().map(([v, , t]) => h("option", { value: v, text: t })));
    E.tz.value = zones().some((x) => x[0] === keep) ? keep : zones()[0][0];
    E.info.replaceChildren();
    if (!TS.info) { E.info.append(h("span", { class: "muted", text: r.status === 404 ? "Time lookup is not available on this player (older package)." : errText(r) })); return; }
    const f = TS.info.display_frames || {}, hi = TS.info.history || {}, dz = TS.info.device_timezone || {};
    E.info.append(h("span", { class: "muted", "data-role": "ts-info", text:
      `Device timezone: ${dz.name || DASH}${dz.source ? " (" + dz.source + ")" : ""} · ${fmtInt(f.retained)} display frames retained` + (f.oldest_utc ? ` from ${f.oldest_utc} to ${f.newest_utc}` : "") +
      ` · JPEGs kept ${hi.jpeg_retention_days || "∞"} d, metadata ${hi.metadata_retention_days || "∞"} d · all times are stored in UTC` }));
    E.latest.disabled = !f.newest_utc_ms;
    if (!E.date.value) { const ms = f.newest_utc_ms || Date.now(); setTime(ms); } else updateResolved();
  }

  function build(slot) {
    TS.built = true;
    E.info = h("div"); E.resolved = h("div", { "aria-live": "polite" }); E.results = h("div", { "aria-live": "polite" }); E.msg = h("span", { class: "muted" });
    E.date = h("input", { type: "date", "aria-label": "Date", "data-role": "ts-date" });
    E.time = h("input", { type: "time", step: "1", "aria-label": "Time", "data-role": "ts-time" });
    E.tz = h("select", { "aria-label": "Timezone", "data-role": "ts-tz" });
    E.tzOther = h("input", { type: "text", list: "ts-tzlist", placeholder: "e.g. America/New_York", "aria-label": "Other timezone (IANA name)", hidden: true, style: "min-width:14em", "data-role": "ts-tz-other" });
    const dl = h("datalist", { id: "ts-tzlist" });
    try { (Intl.supportedValuesOf ? Intl.supportedValuesOf("timeZone") : []).forEach((z) => dl.append(h("option", { value: z }))); } catch (_) {}
    E.pick = h("select", { "aria-label": "Which of the two instants", "data-role": "ts-pick" }, h("option", { value: "first", text: "first occurrence" }), h("option", { value: "second", text: "second occurrence" }));
    E.pickRow = h("div", { class: "vm-row", hidden: true }, h("label", null, "That local time happens twice: search the", E.pick));
    E.window = h("input", { type: "number", min: "1", max: "3600", step: "1", value: "30", "aria-label": "Search window in seconds", "data-role": "ts-window" });
    E.tol = h("input", { type: "number", min: "0", max: "3600000", step: "100", value: "2000", "aria-label": "Nearest-frame tolerance in milliseconds", "data-role": "ts-tol" });
    E.latest = h("button", { class: "vm-btn small", type: "button", text: "Use newest stored frame time", onclick: () => { if (TS.info && TS.info.display_frames && TS.info.display_frames.newest_utc_ms) setTime(TS.info.display_frames.newest_utc_ms); } });
    E.run = h("button", { class: "vm-btn primary", type: "button", text: "Search by time", "data-role": "ts-run", onclick: run });
    E.spin = h("span", { class: "vm-spin", hidden: true });
    [E.date, E.time, E.pick, E.tzOther].forEach((el) => { el.addEventListener("input", updateResolved); el.addEventListener("change", updateResolved); });
    E.tz.addEventListener("change", () => { E.tzOther.hidden = E.tz.value !== "other"; updateResolved(); });
    [E.date, E.time, E.window, E.tol].forEach((el) => el.addEventListener("keydown", (e) => { if (e.key === "Enter") run(); }));
    slot.append(h("div", { class: "panel", "data-role": "time-search" },
      h("div", { class: "panel-head" }, h("h2", { text: "Search by time" })),
      h("p", { class: "muted", text: "See what was playing on the BrightSign display at a date and time. This looks up the Proof-of-Play sessions the player reported and the display frames stored for them (the three verification checkpoints and, while history recording is on, about one frame per second). It reads stored timestamps only; no image analysis is run." }),
      E.info,
      h("div", { class: "vm-row" },
        h("label", null, "Date", E.date), h("label", null, "Time", E.time), h("label", null, "Timezone", E.tz, E.tzOther, dl),
        h("label", null, "Window ± seconds", E.window), h("label", null, "Nearest tolerance ± ms", E.tol)),
      E.pickRow, E.resolved,
      h("div", { class: "vm-row" }, E.run, E.latest, E.spin, E.msg),
      E.results));
    if (U.lightbox) U.lightbox.attach(E.results);
  }

  function mount(slot) {
    if (!TS.built) build(slot);
    loadInfo();
  }
  U.timeSearch = { mount, wallToUtc, zoneOffsetMs, fmtZoned };
})();
