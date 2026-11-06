/* Argus dashboard, Visual Search source "BrightSign Display (Weston/VOP2)": searches the persisted Proof-of-Play display evidence with a
   reference JPEG and shows each match next to the reference, with its capture time, advertisement, playback session and a link to that session.
   Vanilla JS; relies on window.VMUI from vm.js. All server strings are rendered with textContent.
   Contract: docs/VISUAL_SEARCH_DISPLAY_SOURCE.md. Only stored evidence is searched: an empty result says "No matching stored evidence" and never
   claims the content was not played. */
(() => {
  "use strict";
  if (!window.VMUI) return;
  const U = window.VMUI, h = U.h, isNum = U.isNum, DASH = U.DASH, fmtInt = U.fmtInt, fmtBytes = U.fmtBytes;
  const POP = "/api/pop";
  const MAX_UPLOAD = 8 * 1024 * 1024;
  const NOTE = "BrightSign Display search looks only at frames that Proof of Play stored as evidence of what the player's screen showed " +
    "(Weston/VOP2 output frames captured during registered advertisement plays: the three verification checkpoints of each play and, when display-history recording is on, about one more frame per second). It uses the same MobileNetV2 image embedding as Proof of Play. " +
    "Similarity scores are not probabilities, and a match is an observation that captured display content resembled the reference, not independent proof that the complete advertisement played. Content that was shown outside a registered advertisement session was not stored and cannot be found here.";
  const METHODS = [["resilience_grpc", "Weston/VOP2 output frames (OS service)"], ["brightscript_screenshot", "BrightScript screenshots"], ["any", "All stored display captures"]];
  const GEOMS = [["as_uploaded", "As uploaded (for 640×360 frames from the display path)"], ["display", "Apply display geometry (for a full-frame 16:9 picture or video frame)"]];
  const STATUS_CLASS = { playback_corroborated: "ok", visual_match: "ok", mismatch: "bad", incomplete: "warn", capture_unsupported: "warn", playback_reported: "info" };

  const E = {};
  const SD = { ref: null, ctrl: null, built: false, coverage: null };

  async function papi(path, opts) {
    try {
      const r = await fetch(POP + path, Object.assign({ cache: "no-store", credentials: "same-origin" }, opts));
      let data = null; try { data = await r.json(); } catch (_) {}
      return { status: r.status, data, aborted: false };
    } catch (e) { return { status: 0, data: null, aborted: !!(e && e.name === "AbortError") }; }
  }
  function errText(r) {
    const d = r.data || {};
    const code = d.error || (d.status && d.status.code) || "";
    const m = {
      not_enabled: "Proof of Play is not enabled on this player, so there is no stored display evidence to search.",
      provider_unavailable: "The embedding model could not be loaded on the player.",
      busy: "The embedding model is busy with another search or capture; try again in a moment.",
      timeout: "The search timed out on the player.",
      invalid_argument: "The request was rejected as invalid.",
      too_large: "The image is larger than 8 MiB.",
      unauthorized: "Access token required.",
      forbidden: "The request was refused (cross-origin).",
    }[code] || (r.status === 0 ? "Network error: could not reach the dashboard server." : "Search failed (HTTP " + r.status + ").");
    return m + (d.message && !m.includes(d.message) ? " Server says: " + d.message : "");
  }
  const when = (ms) => h("span", null, U.tsEl(ms), " ", h("span", { class: "muted", text: "(" + U.otherZone(ms) + ")" }));
  const fmtOff = (ms) => (isNum(ms) && ms >= 0 ? (ms / 1000).toFixed(1) + " s into the play" : DASH);

  async function sniffJpeg(file) {
    const b = new Uint8Array(await file.slice(0, 3).arrayBuffer());
    return b.length === 3 && b[0] === 0xff && b[1] === 0xd8 && b[2] === 0xff;
  }
  async function setUpload(file) {
    clearChildren(E.refMsg);
    if (!file) return;
    if (file.size > MAX_UPLOAD) { E.refMsg.append(h("div", { class: "vm-banner bad", text: `Image is ${fmtBytes(file.size)}; the limit is 8 MiB.` })); return; }
    if ((file.type && file.type !== "image/jpeg") || !(await sniffJpeg(file))) { E.refMsg.append(h("div", { class: "vm-banner bad", text: "Only JPEG images (image/jpeg) are accepted." })); return; }
    if (SD.ref && SD.ref.blobUrl) URL.revokeObjectURL(SD.ref.blobUrl);
    SD.ref = { file, blobUrl: URL.createObjectURL(file) };
    renderRef();
  }
  const clearChildren = (el) => { el.replaceChildren(); return el; };
  function renderRef() {
    const box = clearChildren(E.refPreview);
    if (!SD.ref) { box.append(h("p", { class: "muted", text: "No reference selected." })); updateRun(); return; }
    box.append(h("div", { class: "vm-ref" }, h("div", { class: "vm-thumb" }, h("img", { src: SD.ref.blobUrl, alt: "Reference upload" })),
      h("div", null, h("div", { text: "Uploaded reference" }), h("div", { class: "muted", text: `${SD.ref.file.name || "pasted image"} · ${fmtBytes(SD.ref.file.size)}` }),
        h("div", { class: "muted", text: "Embedded in memory by the player; never stored." }))));
    const hint = E.fs && E.fs.querySelector('[data-role="geom-hint"]');
    if (hint) {
      const im = box.querySelector("img");
      const upd = () => {
        const w = im.naturalWidth, hh = im.naturalHeight, disp = E.geom.value === "display";
        hint.textContent = disp ? `Display geometry will be applied: ${w}×${hh} → stretched to 1920×1080, top-left 1280×800, letterboxed to 640×360 (the stored Weston/VOP2 frame layout). Use it for a full-frame picture of the advertisement.`
          : (w === 640 && hh === 360 ? "The image is 640×360, the display-frame size, and is embedded as uploaded."
            : `The image is ${w}×${hh}, not the 640×360 letterboxed display-frame layout. Scores may be lower; choose “Apply display geometry” for a full-frame picture.`);
      };
      if (im.complete && im.naturalWidth) upd(); else im.addEventListener("load", upd);
    }
    updateRun();
  }
  function updateRun() {
    if (!E.run) return;
    const ok = !!(SD.coverage ? SD.coverage.available : true);
    E.run.disabled = !ok || !SD.ref || !!SD.ctrl;
    E.minSim.disabled = !E.minOn.checked;
  }

  function renderCoverage() {
    const box = clearChildren(E.coverage), c = SD.coverage;
    if (!c) { box.append(h("p", { class: "muted", text: "Loading evidence coverage…" })); return; }
    if (!c.available) { box.append(h("div", { class: "vm-banner warn" }, h("b", { text: "Display search is unavailable. " }), c.why)); return; }
    if (!c.searchable_rows) {
      box.append(h("div", { class: "vm-banner warn" }, h("b", { text: "No stored display evidence yet. " }),
        `The player holds ${fmtInt(c.evidence_rows)} evidence row${c.evidence_rows === 1 ? "" : "s"}, none of them embedded display frames from the selected capture method. Run a Proof of Play session first.`));
      return;
    }
    box.append(h("p", { class: "muted" }, `Searchable stored evidence: ${fmtInt(c.searchable_rows)} frame${c.searchable_rows === 1 ? "" : "s"} from ${fmtInt(c.sessions)} playback session${c.sessions === 1 ? "" : "s"}, `,
      c.oldest_utc_ms ? ["captured between ", when(c.oldest_utc_ms), " and ", when(c.newest_utc_ms)] : "", `. JPEG still retained for ${fmtInt(c.images_retained)} of them.`));
  }
  async function loadCoverage() {
    const method = E.method ? E.method.value : "resilience_grpc";
    const r = await papi("/search/display?method=" + encodeURIComponent(method));
    if (r.status === 200 && r.data) SD.coverage = Object.assign({ available: true }, r.data);
    else SD.coverage = { available: false, why: r.status === 503 ? "Proof of Play is not enabled on this player (visual_memory.pop.enabled)." : r.status === 0 ? "The dashboard server is unreachable." : errText(r) };
    renderCoverage(); updateRun();
  }

  // ---- display history panel (recording state, per-session capture, storage, embedding) ---------------------------------------------------------------
  const fmtMB = (b) => (isNum(b) ? (b / 1048576).toFixed(b < 10485760 ? 2 : 1) + " MB" : DASH);
  function renderHistory() {
    const box = clearChildren(E.hist), d = SD.history;
    if (!d) { box.append(h("p", { class: "muted", text: "Loading display-history status…" })); return; }
    if (d.unavailable) { box.append(h("p", { class: "muted", text: "Display-history status is not available on this player (older package)." })); return; }
    const c = d.counters || {}, s = d.store || {}, l = d.limits || {}, act = (d.active_sessions || [])[0];
    const failed = (c.write_failed || 0) + (c.sqlite_failed || 0) + (c.embed_failed || 0) + (c.rejected_invalid || 0);
    const dropped = (c.dropped_queue_full || 0) + (c.backlog_refused || 0) + (c.paused_low_space || 0) + (c.session_limit || 0);
    const cov = SD.coverage || {};
    const row = (k, v) => [h("dt", { text: k }), h("dd", null, v)];
    box.append(h("div", { class: "panel", "data-role": "history-panel" },
      h("div", { class: "panel-head" }, h("h2", { text: "Display history" }),
        h("span", { class: "pop-chip " + (d.enabled ? "ok" : ""), text: d.enabled ? "Recording enabled" : "Recording off" })),
      h("p", { class: "muted", text: d.enabled
        ? `About ${d.frames_per_second_target} frame per second is stored while a registered Proof-of-Play session plays (never during the idle presentation). The three verification checkpoints per session are separate and unchanged.`
        : "Recording is off, so only the three verification checkpoints of each session are searchable. History frames recorded earlier (if any) remain searchable." }),
      h("dl", { class: "dh-panel" },
        ...row("Capture frequency", d.enabled ? `${(1000 / d.interval_ms).toFixed(2)} FPS target` : DASH),
        ...row("Frames captured in the current session", act ? `${fmtInt(act.queued_for_storage + act.staged)} of about ${fmtInt(act.expected_total)} (session ${act.session_id})` : "no session recording now"),
        ...row("Searchable display frames", cov.searchable_rows != null ? `${fmtInt(cov.searchable_rows)} (${fmtInt(cov.checkpoint_rows)} checkpoints · ${fmtInt(cov.history_rows)} history)` : DASH),
        ...row("Latest captured display frame", d.last_commit_utc_ms ? when(d.last_commit_utc_ms) : (s.newest_utc_ms ? when(s.newest_utc_ms) : DASH)),
        ...row("Dropped / failed", `${fmtInt(dropped)} dropped · ${fmtInt(failed)} failed · ${fmtInt((c.missed_no_frame || 0) + (c.duplicate_frames || 0))} seconds without a usable frame`),
        ...row("Storage", `${fmtMB(s.image_bytes)} of ${fmtMB(l.max_bytes)} quota · JPEGs kept ${l.jpeg_retention_days || "∞"} d, metadata ${l.metadata_retention_days || "∞"} d`),
        ...row("Embedding", `${fmtInt(s.embedded)} embedded · ${fmtInt(s.pending_embed)} pending · ${fmtInt(s.embed_failed_rows)} failed`),
        ...row("Stored history", `${fmtInt(s.rows)} frames from ${fmtInt(s.sessions)} sessions · ${fmtInt(s.images_retained)} JPEGs retained`)),
      d.last_error ? h("p", { class: "muted", text: "Last history error: " + d.last_error }) : null));
  }
  async function loadHistory() {
    const r = await papi("/history/status");
    SD.history = r.status === 200 && r.data ? r.data : { unavailable: true };
    if (E.hist) renderHistory();
  }

  async function loadAds() {
    const r = await papi("/media");
    const sel = E.media; if (!sel) return;
    const keep = sel.value;
    sel.replaceChildren(h("option", { value: "", text: "Any advertisement" }));
    if (r.status === 200 && r.data) (r.data.media || []).forEach((m) => sel.append(h("option", { value: m.media_id, text: (m.display_name || m.media_id) + " (" + m.media_id + ")" })));
    sel.value = keep;
  }

  function build(slot) {
    SD.built = true;
    E.coverage = h("div"); E.hist = h("div"); E.refMsg = h("div"); E.refPreview = h("div"); E.results = h("div", { "aria-live": "polite" });
    const fileIn = h("input", { type: "file", accept: "image/jpeg", style: "display:none", "aria-label": "Choose a JPEG reference image for display search" });
    fileIn.addEventListener("change", () => { if (fileIn.files[0]) setUpload(fileIn.files[0]); fileIn.value = ""; });
    const drop = h("div", { class: "vm-drop", tabindex: "0", role: "button", "aria-label": "Upload reference JPEG: click or drop a file" },
      h("div", null, h("b", { text: "Drop a JPEG here or click to choose" })), h("div", { class: "muted", text: "JPEG only, up to 8 MiB" }));
    drop.addEventListener("click", () => fileIn.click());
    drop.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); fileIn.click(); } });
    ["dragenter", "dragover"].forEach((ev) => drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.add("over"); }));
    ["dragleave", "drop"].forEach((ev) => drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.remove("over"); }));
    drop.addEventListener("drop", (e) => { const f = e.dataTransfer && e.dataTransfer.files[0]; if (f) setUpload(f); });

    E.topK = h("input", { type: "number", min: "1", max: "50", step: "1", value: "10", "aria-label": "Top K" });
    E.tf = U.makeTimeFilter(() => {});
    E.method = h("select", { "aria-label": "Display capture method" }, ...METHODS.map(([v, t]) => h("option", { value: v, text: t })));
    E.method.addEventListener("change", loadCoverage);
    E.geom = h("select", { "aria-label": "Reference geometry" }, ...GEOMS.map(([v, t]) => h("option", { value: v, text: t })));
    E.geom.addEventListener("change", renderRef);
    E.media = h("select", { "aria-label": "Advertisement" }, h("option", { value: "", text: "Any advertisement" }));
    E.minOn = h("input", { type: "checkbox" }); E.minOn.addEventListener("change", updateRun);
    E.minSim = h("input", { type: "number", min: "0", max: "1", step: "0.01", value: "0.80", disabled: true, "aria-label": "Minimum similarity" });
    E.reqImg = h("input", { type: "checkbox" });
    E.role = h("select", { "aria-label": "Frame role" }, h("option", { value: "any", text: "All frames (checkpoints + history)" }),
      h("option", { value: "history_sample", text: "Display-history samples only" }), h("option", { value: "verification_checkpoint", text: "Verification checkpoints only" }));
    E.sessId = h("input", { type: "text", maxlength: "64", placeholder: "any session", "aria-label": "Playback session id", style: "min-width:14em" });
    E.run = h("button", { class: "vm-btn primary", type: "button", text: "Search display evidence", onclick: runSearch, disabled: true });
    E.cancel = h("button", { class: "vm-btn", type: "button", text: "Cancel", hidden: true, onclick: () => { if (SD.ctrl) SD.ctrl.abort(); } });
    E.spin = h("span", { class: "vm-spin", hidden: true });
    E.runMsg = h("span", { class: "muted" });

    E.fs = h("fieldset", { class: "vm-fs" }, h("div", { class: "vm-search-grid" },
      h("div", { class: "panel" }, h("div", { class: "panel-head" }, h("h2", { text: "1. Reference" })), drop, fileIn, E.refMsg, E.refPreview),
      h("div", { class: "panel" }, h("div", { class: "panel-head" }, h("h2", { text: "2. Options" })),
        h("div", { class: "vm-stack" },
          h("div", { class: "vm-row" }, h("label", null, "Top K (1–50)", E.topK), h("label", null, "Advertisement", E.media), h("label", null, "Capture method", E.method)),
          h("div", { class: "vm-row" }, h("label", null, "Reference geometry", E.geom)),
          h("p", { class: "muted", "data-role": "geom-hint" }),
          h("div", { class: "vm-row" }, h("label", null, "Frame role", E.role), h("label", null, "Playback session", E.sessId)),
          h("div", { class: "vm-row" }, h("label", { class: "inline" }, E.reqImg, "Require JPEG still available")),
          E.tf.el,
          h("div", { class: "vm-row" }, h("label", { class: "inline" }, E.minOn, "Override minimum similarity (default: the Proof-of-Play match threshold)"), E.minSim),
          h("div", { class: "vm-row" }, E.run, E.cancel, E.spin, E.runMsg)))));
    slot.append(h("div", { class: "vm-stack" }, h("div", { class: "vm-note", text: NOTE }), E.hist, E.coverage, E.fs, E.results));
    if (U.lightbox) U.lightbox.attach(E.results);
    renderRef(); renderCoverage(); renderHistory();
  }

  async function runSearch() {
    if (!SD.ref || SD.ctrl) return;
    const k = Math.floor(Number(E.topK.value));
    if (!(k >= 1 && k <= 50)) { E.runMsg.textContent = "Top K must be between 1 and 50."; return; }
    const q = new URLSearchParams({ top_k: k, method: E.method.value, geometry: E.geom.value });
    const t = E.tf.get();
    if (t.start != null) q.set("start", t.start);
    if (t.end != null) q.set("end", t.end);
    if (t.start != null && t.end != null && t.end <= t.start) { E.runMsg.textContent = "“To” must be after “From”."; return; }
    if (E.media.value) q.set("media_id", E.media.value);
    if (E.minOn.checked) {
      const ms = Number(E.minSim.value);
      if (!(ms >= 0 && ms <= 1) || E.minSim.value === "") { E.runMsg.textContent = "Minimum similarity must be between 0 and 1."; return; }
      q.set("min_similarity", ms);
    }
    if (E.reqImg.checked) q.set("require_image", "1");
    if (E.role.value !== "any") q.set("role", E.role.value);
    if (E.sessId.value.trim()) q.set("session_id", E.sessId.value.trim());
    const ref = SD.ref;
    SD.ctrl = new AbortController();
    E.cancel.hidden = false; E.spin.hidden = false; E.runMsg.textContent = "Searching…";
    clearChildren(E.results); updateRun();
    const r = await papi("/search/display?" + q, { method: "POST", headers: { "Content-Type": "image/jpeg" }, body: ref.file, signal: SD.ctrl.signal });
    SD.ctrl = null; E.cancel.hidden = true; E.spin.hidden = true; E.runMsg.textContent = ""; updateRun();
    if (r.aborted) { E.results.append(h("div", { class: "vm-banner" }, "Search cancelled.")); return; }
    if (r.status !== 200 || !r.data || !r.data.results) {
      E.results.append(h("div", { class: "vm-banner bad" }, h("b", { text: "Search failed. " }), errText(r)));
      if (r.status === 401) U.pollStatus();
      return;
    }
    renderResults(r.data, ref);
  }

  function evImg(x, alt) {
    if (!x.image_url) return h("div", { class: "vm-empty", text: "JPEG expired (retention removed the file; the score and time are kept)" });
    const img = h("img", { src: x.image_url, alt, loading: "lazy", "data-zoom": "1", "data-title": (x.role === "history_sample" ? "History sample" : "Verification checkpoint") + " · " + (x.advertisement_name || x.advertisement_id || "") + (isNum(x.offset_in_play_ms) ? " · " + (x.offset_in_play_ms / 1000).toFixed(1) + " s into the play" : ""),
      "data-meta": [isNum(x.similarity) ? "similarity " + x.similarity.toFixed(4) : "", x.capture_utc_ms ? "captured " + new Date(x.capture_utc_ms).toLocaleString() : "", "session " + x.session_id,
        "frame " + (x.role === "history_sample" ? "history #" + x.history_id : "checkpoint #" + x.evidence_id) + " · " + fmtInt(x.width) + "×" + fmtInt(x.height)].filter(Boolean).join("|") });
    img.addEventListener("error", () => img.replaceWith(h("div", { class: "vm-empty", text: "JPEG no longer available" })));
    return img;
  }
  // A stored frame (or any same-origin JPEG) becomes the reference of the next display search.
  async function reuseFrame(url) {
    try {
      const r = await fetch(url, { credentials: "same-origin", cache: "no-store" });
      if (!r.ok) throw new Error("HTTP " + r.status);
      const b = await r.blob();
      if (!b.size || (b.type && b.type !== "image/jpeg")) throw new Error("not a JPEG");
      await setUpload(new File([b], "stored-frame.jpg", { type: "image/jpeg" }));
      if (E.geom) { E.geom.value = "as_uploaded"; renderRef(); }
      window.scrollTo({ top: 0, behavior: "smooth" });
    } catch (e) { E.runMsg.textContent = "Could not use this frame as a reference: " + e.message; }
  }
  function consumePending() {
    if (!SD.pendingRef) return;
    const f = SD.pendingRef; SD.pendingRef = null;
    setUpload(f).then(() => { if (E.geom) { E.geom.value = "as_uploaded"; renderRef(); } window.scrollTo({ top: 0 }); });
  }
  U.useDisplayReference = (file) => {
    SD.pendingRef = file;
    const target = "#search?source=display", same = location.hash === target;
    location.hash = target;
    if (same && SD.built) consumePending();
  };
  function hitCard(x, ref, closest) {
    const s = isNum(x.similarity) ? x.similarity : null;
    const chipCls = STATUS_CLASS[x.session_status] || "";
    const link = h("a", { class: "vm-btn small", href: x.session_link, text: "Open playback session " + x.session_id, "data-session-link": x.session_id });
    return h("div", { class: "panel ds-hit" + (closest ? " ds-closest" : ""), "data-evidence": String(x.role === "history_sample" ? "h" + x.history_id : x.evidence_id), "data-role-kind": x.role || "" },
      h("div", { class: "vm-cmp" },
        h("div", null, h("h3", { text: "Reference" }), h("div", { class: "vm-thumb ds-img" }, h("img", { src: ref.blobUrl, alt: "Reference", "data-zoom": "1", "data-title": "Reference image", "data-meta": "the image you searched with" }))),
        h("div", null, h("h3", null, closest ? "Closest stored frame" : "Match #" + x.rank,
          h("span", { class: "dh-role " + (x.role === "history_sample" ? "hist" : "ck"), "data-role": "frame-role", text: x.role === "history_sample" ? "History sample" : "Verification checkpoint" })),
          h("div", { class: "vm-thumb ds-img" }, evImg(x, "Stored display frame")))),
      h("div", { class: "ds-meta" },
        h("span", { class: "vm-score", text: s != null ? "similarity " + s.toFixed(4) : DASH }),
        h("div", { class: "vm-bar", "aria-hidden": "true" }, s != null ? h("i", { style: `width:${Math.max(0, Math.min(1, s)) * 100}%` }) : null),
        h("dl", { class: "vm-dl" },
          h("dt", { text: "Captured" }), h("dd", null, when(x.capture_utc_ms)),
          h("dt", { text: "Within the play" }), h("dd", { text: fmtOff(x.offset_in_play_ms) }),
          h("dt", { text: "Advertisement" }), h("dd", { text: (x.advertisement_name ? x.advertisement_name + " · " : "") + (x.advertisement_id || DASH) }),
          x.expected_media_id && x.played_media_id && x.expected_media_id !== x.played_media_id ? [h("dt", { text: "Expected" }), h("dd", { text: x.expected_media_id })] : null,
          h("dt", { text: "Session" }), h("dd", null, h("span", { class: "pop-chip " + chipCls, text: x.session_status_label || DASH }), " ", h("span", { class: "muted", text: x.session_id })),
          h("dt", { text: "Source" }), h("dd", { text: (x.capture_method === "resilience_grpc" ? "Weston/VOP2 frame" : x.capture_method === "brightscript_screenshot" ? "BrightScript screenshot" : x.capture_method) + (isNum(x.time_uncertainty_ms) ? " · time ±" + x.time_uncertainty_ms + " ms" : "") }),
          h("dt", { text: "Frame" }), h("dd", { text: (x.role === "history_sample" ? `history frame #${x.history_id} (sample ${x.sec_idx})` : `checkpoint evidence #${x.evidence_id}`) + ` · ${fmtInt(x.width)} × ${fmtInt(x.height)}` })),
        h("div", { class: "vm-row", style: "gap:8px" }, link,
          x.image_url ? h("button", { class: "vm-btn small", type: "button", "data-role": "search-with-frame", text: "Search with this frame", onclick: () => reuseFrame(x.image_url) }) : null)));
  }
  function renderResults(d, ref) {
    const box = clearChildren(E.results), res = d.results || [], st = d.status || {}, cov = d.coverage || {}, m = d.model || {};
    box.append(h("div", { class: "vm-label", text: d.label || "" }));
    if (d.query && d.query.embedded_jpeg_b64) {
      ref = Object.assign({}, ref, { blobUrl: "data:image/jpeg;base64," + d.query.embedded_jpeg_b64 });
      box.append(h("p", { class: "muted", "data-role": "geom-note", text: `Reference shown as embedded (${d.query.uploaded_size} upload → ${d.query.embedded_size} display geometry). ${d.query.geometry_note}` }));
    } else if (d.query && d.query.geometry_note) box.append(h("p", { class: "muted", "data-role": "geom-note", text: d.query.geometry_note }));
    box.append(h("p", { class: "muted", style: "margin-top:10px", text:
      `${res.length} result${res.length === 1 ? "" : "s"} · ${isNum(st.elapsed_ms) ? st.elapsed_ms.toFixed(1) + " ms" : DASH} · ${fmtInt(st.candidates_scanned)} stored frames scanned (${fmtInt(cov.checkpoint_rows)} checkpoints, ${fmtInt(cov.history_rows)} history) · ` +
      `minimum similarity ${isNum(d.options && d.options.min_similarity) ? d.options.min_similarity.toFixed(2) : DASH}${d.options && d.options.min_similarity_default ? " (Proof-of-Play match threshold)" : ""} · ` +
      `model ${m.name || DASH} ${m.preprocessing || ""} dim ${fmtInt(m.dimension)}` }));
    if (cov.other_model_rows) box.append(h("div", { class: "vm-banner warn", text: `${fmtInt(cov.other_model_rows)} stored evidence frame(s) were produced by a different embedding model and cannot be compared.` }));
    if (!res.length) {
      box.append(h("div", { class: "vm-empty ds-nomatch", role: "status" }, h("b", { text: d.message || "No matching stored evidence" }), h("p", { class: "muted", text: d.message_detail || "" })));
      if (d.closest_below_minimum) {
        box.append(h("h3", { text: "Closest stored frame (below the minimum, so not a match)", style: "margin-top:12px" }));
        box.append(hitCard(d.closest_below_minimum, ref, true));
      }
      return;
    }
    res.forEach((x) => box.append(hitCard(x, ref, false)));
  }

  function mount(slot) {
    if (!SD.built) build(slot);
    loadCoverage(); loadAds(); loadHistory(); consumePending();
    if (!SD.timer) SD.timer = setInterval(() => { if (document.getElementById('view-search') && !document.getElementById('view-search').hidden && E.hist && E.hist.offsetParent !== null) loadHistory(); }, 5000);
  }
  if (U.registerDisplaySearch) U.registerDisplaySearch({ mount });
})();
