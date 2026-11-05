/* Argus dashboard: Visual Memory status pill, Visual Memory page (status + gallery + frame detail),
   Visual Search page, hash routing. Vanilla JS, no dependencies. All API-provided strings are rendered
   through textContent (see h()). Contract: docs/VISUAL_MEMORY_DASHBOARD_API.md */
(() => {
  "use strict";

  const API = "/api/vm";
  const PAGE_SIZE = 24;
  const MAX_UPLOAD = 8 * 1024 * 1024;
  const POLL_MS = 5000;
  const DASH = "—";
  const SEARCH_NOTE = "Image/scene similarity from a whole-frame MobileNetV2 embedding. It does not detect objects and does not identify people.";
  const EXPIRED_TXT = "JPEG expired — metadata and embedding retained";
  const MISSING_TXT = "JPEG file missing";

  // ---- tiny DOM helpers ---------------------------------------------------
  function h(tag, attrs, ...kids) {
    const e = document.createElement(tag);
    if (attrs) for (const [k, v] of Object.entries(attrs)) {
      if (v == null || v === false) continue;
      if (k === "class") e.className = v;
      else if (k === "text") e.textContent = v;
      else if (k.startsWith("on")) e.addEventListener(k.slice(2), v);
      else if (v === true) e.setAttribute(k, "");
      else e.setAttribute(k, String(v));
    }
    for (const c of kids.flat()) if (c != null && c !== false) e.append(c.nodeType ? c : document.createTextNode(String(c)));
    return e;
  }
  const $ = (id) => document.getElementById(id);
  const clear = (e) => { while (e.firstChild) e.removeChild(e.firstChild); return e; };
  const isNum = (v) => typeof v === "number" && isFinite(v);

  // ---- formatting -----------------------------------------------------------
  const fmtInt = (v) => (isNum(v) ? v.toLocaleString() : DASH);
  function fmtBytes(v) {
    if (!isNum(v)) return DASH;
    const u = ["B", "KB", "MB", "GB", "TB"]; let i = 0, x = v;
    while (x >= 1000 && i < u.length - 1) { x /= 1000; i++; }
    return (i === 0 ? x.toFixed(0) : x.toFixed(x >= 100 ? 0 : x >= 10 ? 1 : 2)) + " " + u[i];
  }
  function fmtDur(ms) {
    if (!isNum(ms)) return DASH;
    if (ms < 1000) return Math.round(ms) + " ms";
    const s = ms / 1000;
    if (s < 60) return s.toFixed(1) + " s";
    if (s < 3600) return Math.floor(s / 60) + " min " + Math.round(s % 60) + " s";
    return (s / 3600).toFixed(1) + " h";
  }
  const p2 = (n) => String(n).padStart(2, "0");
  const LOCAL_TZ = (() => { try { return Intl.DateTimeFormat().resolvedOptions().timeZone || "local"; } catch (_) { return "local"; } })();

  // Display zone shared by every widget: "local" | "utc"
  const tz = { mode: "local", subs: [] };
  try { if (localStorage.getItem("vm_tz") === "utc") tz.mode = "utc"; } catch (_) {}
  function setTz(m) {
    if (m === tz.mode) return;
    tz.mode = m;
    try { localStorage.setItem("vm_tz", m); } catch (_) {}
    tz.subs.forEach((f) => f());
  }
  function fmtZone(ms, mode) {
    if (!isNum(ms) || ms <= 0) return DASH;
    const d = new Date(ms);
    if (mode === "utc") return `${d.getUTCFullYear()}-${p2(d.getUTCMonth() + 1)}-${p2(d.getUTCDate())} ${p2(d.getUTCHours())}:${p2(d.getUTCMinutes())}:${p2(d.getUTCSeconds())} UTC`;
    return `${d.getFullYear()}-${p2(d.getMonth() + 1)}-${p2(d.getDate())} ${p2(d.getHours())}:${p2(d.getMinutes())}:${p2(d.getSeconds())}`;
  }
  const fmtTs = (ms) => fmtZone(ms, tz.mode);
  // The zone that is NOT currently displayed, labelled (for secondary lines).
  const otherZone = (ms) => (tz.mode === "utc" ? "local " + fmtZone(ms, "local") : fmtZone(ms, "utc"));
  const tsTitle = (ms) => (isNum(ms) && ms > 0 ? `UTC: ${fmtZone(ms, "utc")}\nLocal (${LOCAL_TZ}): ${fmtZone(ms, "local")}` : "");
  function tsEl(ms, cls) {
    const e = h("span", { class: cls || "", title: tsTitle(ms), text: fmtTs(ms) });
    e._ms = ms; e.dataset.vmts = "1";
    return e;
  }
  function refreshTs(root) { root.querySelectorAll("[data-vmts]").forEach((e) => { e.textContent = fmtTs(e._ms); }); }
  const toInputVal = (ms, mode) => {
    const d = new Date(ms);
    return mode === "utc"
      ? `${d.getUTCFullYear()}-${p2(d.getUTCMonth() + 1)}-${p2(d.getUTCDate())}T${p2(d.getUTCHours())}:${p2(d.getUTCMinutes())}`
      : `${d.getFullYear()}-${p2(d.getMonth() + 1)}-${p2(d.getDate())}T${p2(d.getHours())}:${p2(d.getMinutes())}`;
  };
  function fromInputVal(v, mode) {
    const m = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})/.exec(v || "");
    if (!m) return null;
    const a = m.slice(1, 6).map(Number);
    return mode === "utc" ? Date.UTC(a[0], a[1] - 1, a[2], a[3], a[4]) : new Date(a[0], a[1] - 1, a[2], a[3], a[4]).getTime();
  }

  // ---- API ------------------------------------------------------------------
  // Returns {status, data, net}. status 0 => network failure/abort.
  async function api(path, opts) {
    try {
      const r = await fetch(API + path, Object.assign({ cache: "no-store", credentials: "same-origin" }, opts));
      let data = null;
      try { data = await r.json(); } catch (_) {}
      return { status: r.status, data };
    } catch (e) {
      return { status: 0, data: null, aborted: e && e.name === "AbortError" };
    }
  }

  // ---- shared state -----------------------------------------------------------
  const S = {
    status: null,          // last good /status body
    apiState: "unknown",   // unknown | ok | down | auth
    apiDetail: "",
    subs: [],
  };
  const onStatus = (f) => S.subs.push(f);

  // ---- status polling ---------------------------------------------------------
  let pollTimer = null, polling = false;
  async function pollStatus() {
    if (polling) return;
    polling = true;
    try {
      const r = await api("/status");
      if (r.status === 200 && r.data) { S.status = r.data; S.apiState = "ok"; S.apiDetail = ""; }
      else if (r.status === 401) { S.apiState = "auth"; S.apiDetail = ""; }
      else { S.apiState = "down"; S.apiDetail = r.status ? "HTTP " + r.status : "network error"; }
    } finally { polling = false; }
    S.subs.forEach((f) => { try { f(); } catch (e) { console.error(e); } });
  }
  function schedulePoll() {
    clearTimeout(pollTimer);
    pollTimer = setTimeout(async () => {
      if (!document.hidden) await pollStatus();
      schedulePoll();
    }, POLL_MS);
  }
  document.addEventListener("visibilitychange", () => { if (!document.hidden) { pollStatus(); schedulePoll(); } });

  // ---- health evaluation (pill) -------------------------------------------------
  const HARD_FAIL = [["stage1", "jpeg_write_failures", "JPEG write failures"], ["stage1", "database_failures", "Stage-1 DB failures"],
    ["stage2", "inference_failures", "Inference failures"], ["stage2", "permanent_failures", "Permanent embed failures"],
    ["stage2", "decode_failures", "Decode failures"], ["stage2", "db_failures", "Stage-2 DB failures"], ["stage2", "invalid_vectors", "Invalid vectors"]];
  const DROPS = [["stage1", "dropped_queue_full", "Stage-1 queue drops"], ["stage1", "dropped_invalid", "Invalid frames dropped"],
    ["stage2", "dropped_queue_full", "Stage-2 queue drops"]];
  const cnt = (st, grp, k) => { const g = st && st.runtime && st.runtime[grp]; const v = g && g[k]; return isNum(v) ? v : null; };

  function evaluate() {
    if (S.apiState === "down") return { cls: "grey", text: "VM: API offline", reasons: ["Visual Memory API not running (" + S.apiDetail + ")"] };
    if (S.apiState === "auth") return { cls: "amber", text: "VM: locked", reasons: ["Access token required"] };
    const st = S.status;
    if (!st) return { cls: "grey", text: "VM: …", reasons: [] };
    if (!st.configured) return { cls: "grey", text: "VM: not configured", reasons: ["visual_memory.enabled is false in the active config"] };
    const red = [], amber = [];
    if (st.store && st.store.available === false) red.push("Store unavailable");
    for (const [g, k, l] of HARD_FAIL) { const v = cnt(st, g, k); if (v) red.push(`${l}: ${v}`); }
    if (st.store && st.store.embeddings_failed > 0) red.push(`Failed embeddings stored: ${st.store.embeddings_failed}`);
    if (st.viewer) { /* dataset viewer: no live Argus writer by design */ }
    else if (!st.runtime) amber.push("No runtime snapshot");
    else {
      if (st.runtime.running === false) amber.push("Pipeline not running");
      if (st.runtime.stale) amber.push("Runtime snapshot stale");
    }
    const ing = st.ingest;
    if (ing && ing.state === "stalled") red.push("Frame ingestion stalled: " + (ing.message || "no new frames"));
    if (st.embedding_configured === false) amber.push("Embeddings not configured");
    else if (st.runtime && st.runtime.stage2 && st.runtime.stage2.status && st.runtime.stage2.status !== "active") amber.push("Stage-2 status: " + st.runtime.stage2.status);
    for (const [g, k, l] of DROPS) { const v = cnt(st, g, k); if (v) amber.push(`${l}: ${v}`); }
    if (Array.isArray(st.errors) && st.errors.length) amber.push(...st.errors.filter((e) => !(ing && ing.state === "stalled" && e === ing.message)));
    if (red.length) return { cls: "red", text: "VM: errors", reasons: red.concat(amber) };
    if (amber.length) return { cls: "amber", text: "VM: degraded", reasons: amber };
    if (st.viewer) return { cls: "green", text: "VM: dataset viewer", reasons: ["Read-only view of a stored dataset (no live capture)"] };
    return { cls: "green", text: "VM: active", reasons: ["Active and healthy"] };
  }
  function renderPill() {
    const ev = evaluate();
    const p = $("vmPill");
    p.className = "vm-pill vm-" + ev.cls;
    $("vmPillText").textContent = ev.text;
    p.title = "Visual Memory: " + ev.reasons.join("\n");
  }

  // ---- generic bits -------------------------------------------------------------
  function placeholder(txt, big) {
    return h("div", { class: "vm-ph" + (big ? " big" : ""), role: "img", "aria-label": txt }, h("span", { class: "ic", "aria-hidden": "true", text: "∅" }), h("span", { text: txt }));
  }
  // Image of a stored frame with expired/missing handling. kind: "thumb" | "image"
  function frameImage(f, kind, w, big) {
    if (f.image_available === false) return placeholder(EXPIRED_TXT, big);
    if (f.image_exists === false) return placeholder(MISSING_TXT, big);
    const src = kind === "thumb" ? `${API}/frames/${f.id != null ? f.id : f.frame_id}/thumb?w=${w || 240}` : `${API}/frames/${f.id != null ? f.id : f.frame_id}/image`;
    return loadImg(src, f.id != null ? f.id : f.frame_id, kind, big);
  }
  function loadImg(src, id, kind, big) {
    const holder = h("span", { style: "display:contents" });
    const img = h("img", { src, alt: "Frame " + id, loading: "lazy", decoding: "async" });
    img.addEventListener("error", async () => {
      let txt = "Image unavailable";
      try {
        const r = await fetch(src, { cache: "no-store" });
        if (r.status === 410) txt = EXPIRED_TXT; else if (r.status === 404) txt = MISSING_TXT;
        else if (r.status === 401) txt = "Access token required";
      } catch (_) {}
      holder.replaceChildren(placeholder(txt, big));
    });
    holder.append(img);
    return holder;
  }
  function copyText(t, btn) {
    const done = () => { const o = btn.textContent; btn.textContent = "Copied"; setTimeout(() => { btn.textContent = o; }, 1200); };
    if (navigator.clipboard && window.isSecureContext) navigator.clipboard.writeText(t).then(done, () => fallback());
    else fallback();
    function fallback() {
      const ta = h("textarea", { style: "position:fixed;opacity:0" }); ta.value = t; document.body.append(ta); ta.select();
      try { document.execCommand("copy"); done(); } catch (_) {} ta.remove();
    }
  }

  // Modal
  const modals = [];
  function openModal(title, body, opts) {
    const prevFocus = document.activeElement;
    const close = () => {
      const i = modals.indexOf(m); if (i >= 0) modals.splice(i, 1);
      bg.remove();
      if (opts && opts.onClose) opts.onClose();
      if (prevFocus && prevFocus.focus) try { prevFocus.focus(); } catch (_) {}
    };
    const closeBtn = h("button", { class: "vm-btn small", type: "button", "aria-label": "Close", onclick: close, text: "Close ✕" });
    const box = h("div", { class: "vm-modal" + (opts && opts.wide ? " wide" : ""), role: "dialog", "aria-modal": "true", "aria-label": title },
      h("div", { class: "vm-modal-head" }, h("h2", { text: title }), closeBtn), body);
    const bg = h("div", { class: "vm-modal-bg", onmousedown: (e) => { if (e.target === bg) close(); } }, box);
    document.body.append(bg);
    const m = { close };
    modals.push(m);
    closeBtn.focus();
    return m;
  }
  document.addEventListener("keydown", (e) => { if (e.key === "Escape" && modals.length) modals[modals.length - 1].close(); });

  // Auth box shown when the API says 401
  function authBox() {
    const inp = h("input", { type: "password", placeholder: "access token", autocomplete: "off", "aria-label": "Visual Memory access token" });
    const msg = h("span", { class: "muted" });
    const go = async () => {
      msg.textContent = "…";
      const r = await api("/auth", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ token: inp.value }) });
      if (r.status === 200) { inp.value = ""; msg.textContent = "OK"; pollStatus(); document.dispatchEvent(new CustomEvent("vm-authed")); }
      else msg.textContent = r.status === 401 ? "Wrong token" : "Failed (" + (r.status || "network") + ")";
    };
    inp.addEventListener("keydown", (e) => { if (e.key === "Enter") go(); });
    return h("div", { class: "vm-banner warn" }, h("p", null, h("b", { text: "Access token required." }), " The Visual Memory API is protected by dashboard.vm_token."),
      h("div", { class: "vm-row" }, inp, h("button", { class: "vm-btn primary", type: "button", onclick: go, text: "Unlock" }), msg));
  }
  function apiDownBox() {
    return h("div", { class: "vm-banner bad" }, h("p", null, h("b", { text: "Visual Memory API not running." }), " " + (S.apiDetail ? `(${S.apiDetail}) ` : "") +
      "The dashboard could not reach vm_api_server. Live audience analytics are unaffected."));
  }

  // ---- time/source filter widget -------------------------------------------------
  function makeTimeFilter(onChange) {
    let startMs = null, endMs = null;
    const from = h("input", { type: "datetime-local", "aria-label": "From" });
    const to = h("input", { type: "datetime-local", "aria-label": "To" });
    const segBtn = (m, t) => h("button", { type: "button", "data-m": m, onclick: () => setTz(m), text: t });
    const seg = h("div", { class: "vm-seg", role: "group", "aria-label": "Time zone for filters and display" }, segBtn("local", "Local"), segBtn("utc", "UTC"));
    const sync = () => {
      seg.querySelectorAll("button").forEach((b) => b.classList.toggle("on", b.dataset.m === tz.mode));
      from.value = startMs != null ? toInputVal(startMs, tz.mode) : "";
      to.value = endMs != null ? toInputVal(endMs, tz.mode) : "";
    };
    const read = () => { startMs = fromInputVal(from.value, tz.mode); endMs = fromInputVal(to.value, tz.mode); };
    from.addEventListener("change", () => { read(); onChange(); });
    to.addEventListener("change", () => { read(); onChange(); });
    const quick = (label, ms) => h("button", { class: "vm-btn small", type: "button", text: label, onclick: () => {
      if (ms == null) { startMs = endMs = null; } else { endMs = null; startMs = Date.now() - ms; }
      sync(); onChange();
    } });
    tz.subs.push(sync);
    sync();
    const el = h("div", { class: "vm-filters", style: "margin:0" },
      h("label", null, "Zone", seg), h("label", null, "From", from), h("label", null, "To", to),
      h("div", { class: "vm-row" }, quick("Last hour", 3600e3), quick("Last 24 h", 86400e3), quick("All", null)));
    return { el, get: () => ({ start: startMs, end: endMs }), set(s, e) { startMs = s; endMs = e; sync(); } };
  }
  function sourceSelect(onChange) {
    const sel = h("select", { "aria-label": "Source" }, h("option", { value: "", text: "All sources" }));
    sel.addEventListener("change", onChange);
    const upd = () => {
      const srcs = (S.status && S.status.store && Array.isArray(S.status.store.sources)) ? S.status.store.sources : [];
      const cur = sel.value;
      const have = Array.from(sel.options).slice(1).map((o) => o.value);
      if (have.join("\n") === srcs.join("\n")) return;
      clear(sel).append(h("option", { value: "", text: "All sources" }), ...srcs.map((s) => h("option", { value: s, text: s })));
      sel.value = srcs.includes(cur) ? cur : "";
    };
    onStatus(upd); upd();
    return sel;
  }

  // ---- gallery component (main page + picker) ---------------------------------------
  function makeGallery(opts) {
    const st = { offset: 0, total: null, seq: 0, selected: opts.selected || null, loadedNewest: null };
    const grid = h("div", { class: "vm-grid", "aria-live": "polite" });
    const live = h("div", { class: "vm-live" });          // "new frames" / "ingestion stalled" notices (main gallery only)
    const msg = h("div");
    const info = h("span", { class: "muted" });
    const prev = h("button", { class: "vm-btn small", type: "button", text: "← Prev", onclick: () => { st.offset = Math.max(0, st.offset - PAGE_SIZE); load(); } });
    const next = h("button", { class: "vm-btn small", type: "button", text: "Next →", onclick: () => { st.offset += PAGE_SIZE; load(); } });
    const tf = makeTimeFilter(() => { st.offset = 0; load(); });
    const src = sourceSelect(() => { st.offset = 0; load(); });
    const order = h("select", { "aria-label": "Order" }, h("option", { value: "desc", text: "Newest first" }), h("option", { value: "asc", text: "Oldest first" }));
    order.addEventListener("change", () => { st.offset = 0; load(); });
    const refresh = h("button", { class: "vm-btn small", type: "button", text: "Refresh", onclick: () => load() });
    const root = h("div", null,
      h("div", { class: "vm-filters" }, tf.el, h("label", null, "Source", src), h("label", null, "Order", order), refresh),
      live, msg, grid, h("div", { class: "vm-pager" }, prev, next, info));
    tz.subs.push(() => refreshTs(grid));

    // Live behaviour (main gallery only; the picker inside Visual Search must not move under the user).
    // "Following" = page 1, newest first, no end-time filter: new frames are then merged in place on the next
    // status poll (existing cards, scroll position, selection and open dialogs are untouched). Otherwise the user
    // keeps exactly what they are looking at and gets a notice with a button instead.
    const newestKnown = () => { const sto = S.status && S.status.store; return sto && isNum(sto.newest_utc_ms) ? sto.newest_utc_ms : null; };
    const following = () => opts.main && st.offset === 0 && order.value === "desc" && tf.get().end == null;
    function renderLive() {
      clear(live);
      if (!opts.main) return;
      const ing = S.status && S.status.ingest;
      if (ing && ing.state === "stalled")
        live.append(h("div", { class: "vm-banner bad", role: "alert" }, "Frame ingestion has stopped: " + (ing.message || "no new frames are being stored") + ". The gallery shows the newest frames that were stored."));
      const n = newestKnown();
      if (n != null && st.loadedNewest != null && n > st.loadedNewest && !following())
        live.append(h("div", { class: "vm-banner info", role: "status" }, "Newer frames have been stored. ",
          h("button", { class: "vm-btn small", type: "button", text: "Show newest", onclick: () => { tf.set(null, null); order.value = "desc"; st.offset = 0; load(); } })));
    }
    function onStatusTick() {
      if (!opts.main || !root.isConnected) return;
      const n = newestKnown();
      if (n != null && st.loadedNewest == null) st.loadedNewest = n;
      else if (n != null && n !== st.loadedNewest && following() && !st.busy) load({ silent: true });
      renderLive();
    }
    onStatus(onStatusTick);

    function card(f) {
      const hasEmb = f.has_embedding;
      const b = h("button", { class: "vm-card" + (st.selected === f.id ? " selected" : ""), type: "button", "data-id": f.id,
        "aria-label": `Frame ${f.id}, ${fmtZone(f.capture_utc_ms, tz.mode)}`, onclick: () => opts.onPick(f) },
        h("div", { class: "vm-thumb" }, frameImage(f, "thumb", 240)),
        h("div", { class: "meta" },
          tsEl(f.capture_utc_ms, "t"),
          h("span", { class: "src", title: f.source_id || "", text: f.source_id || DASH }),
          h("div", { class: "vm-badges" },
            h("span", { class: "vm-badge", text: "#" + f.id }),
            f.image_available === false ? h("span", { class: "vm-badge warn", text: "JPEG expired" }) : f.image_exists === false ? h("span", { class: "vm-badge bad", text: "JPEG missing" }) : null,
            hasEmb ? h("span", { class: "vm-badge ok", text: "embedding" }) : h("span", { class: "vm-badge", text: "no embedding" }))));
      return b;
    }
    async function load(o) {
      const silent = !!(o && o.silent);
      if (S.apiState === "auth") { clear(grid); clear(msg); if (!opts.main) msg.append(authBox()); info.textContent = ""; prev.disabled = next.disabled = true; return; }
      if (S.apiState === "down") { clear(grid); clear(msg); if (!opts.main) msg.append(apiDownBox()); info.textContent = ""; prev.disabled = next.disabled = true; return; }
      if (S.status && !S.status.configured) { clear(grid); clear(msg).append(h("div", { class: "vm-empty", text: "Visual Memory is not configured, so there are no frames." })); info.textContent = ""; return; }
      const seq = ++st.seq;
      const q = new URLSearchParams({ limit: PAGE_SIZE, offset: st.offset, order: order.value });
      const t = tf.get();
      if (t.start != null) q.set("start", t.start);
      if (t.end != null) q.set("end", t.end);
      if (src.value) q.set("source", src.value);
      const newestAtRequest = newestKnown();           // frames committed after this are picked up by the next tick
      st.busy = true;
      let r;
      try { r = await api("/frames?" + q); } finally { st.busy = false; }
      if (seq !== st.seq) return;
      if (silent && (r.status !== 200 || !r.data)) return;      // a background refresh never replaces the grid with an error
      if (!silent) { clear(msg); clear(grid); }
      if (r.status === 200 && r.data && st.offset === 0 && order.value === "desc") st.loadedNewest = newestAtRequest;
      if (r.status !== 200 || !r.data) {
        const m = r.status === 401 ? authBox() : r.status === 404 || r.status === 502 || r.status === 0 ? apiDownBox()
          : h("div", { class: "vm-banner bad" }, "Could not load frames: " + ((r.data && (r.data.message || r.data.error)) || "HTTP " + r.status));
        msg.append(m); info.textContent = ""; prev.disabled = next.disabled = true; return;
      }
      const d = r.data, frames = Array.isArray(d.frames) ? d.frames : [];
      st.total = isNum(d.total) ? d.total : frames.length;
      if (silent) {
        // merge in place: reuse the cards of frames that are still on the page, so images do not reload or flicker
        const have = new Map(Array.from(grid.children).filter((c) => c.dataset && c.dataset.id).map((c) => [c.dataset.id, c]));
        const nodes = frames.map((f) => have.get(String(f.id)) || card(f));
        nodes.forEach((n) => n.classList && n.classList.toggle("selected", st.selected != null && String(st.selected) === n.dataset.id));
        if (!nodes.length) nodes.push(h("div", { class: "vm-empty", style: "grid-column:1/-1", text: "No frames match these filters." }));
        grid.replaceChildren(...nodes);
      } else {
        if (!frames.length) {
          if (st.offset > 0 && st.total > 0) { st.offset = Math.max(0, (Math.ceil(st.total / PAGE_SIZE) - 1) * PAGE_SIZE); return load(); }
          grid.append(h("div", { class: "vm-empty", style: "grid-column:1/-1", text: "No frames match these filters." }));
        }
        frames.forEach((f) => grid.append(card(f)));
      }
      const a = frames.length ? st.offset + 1 : 0, b = st.offset + frames.length;
      info.textContent = `${fmtInt(a)}–${fmtInt(b)} of ${fmtInt(st.total)}`;
      prev.disabled = st.offset <= 0; next.disabled = b >= st.total;
      renderLive();
    }
    document.addEventListener("vm-authed", () => { if (root.isConnected) load(); });
    return { el: root, load, setSelected(id) { st.selected = id; } };
  }

  // ---- Visual Memory page --------------------------------------------------------------
  let memInit = false, memGallery = null;
  const memStatusBox = h("div", { class: "vm-stack" });
  function tile(label, value, sub, cls, small) {
    return h("div", { class: "vm-tile " + (cls || "") }, h("div", { class: "l", text: label }), h("div", { class: "v" + (small ? " sm" : ""), text: value }), sub ? h("div", { class: "s", text: sub }) : null);
  }
  function renderMemStatus() {
    const box = clear(memStatusBox);
    if (S.apiState === "auth") { box.append(authBox()); return; }
    if (S.apiState === "down") { box.append(apiDownBox()); return; }
    const st = S.status;
    if (!st) { box.append(h("div", { class: "vm-empty", text: "Loading status…" })); return; }
    const ev = evaluate();
    const rt = st.runtime || null, s1 = rt && rt.stage1, s2 = rt && rt.stage2, sto = st.store, mdl = st.model;
    const cfgTxt = st.configured ? (st.viewer ? "Dataset viewer" : rt && rt.running ? (st.ingest && st.ingest.state === "stalled" ? "Stalled" : "Active") : "Configured") : "Not configured";
    const tiles = h("div", { class: "vm-tiles" });
    tiles.append(
      tile("Visual Memory", cfgTxt, st.viewer ? "read-only stored dataset · no live capture" : (st.config_path || null), ev.cls === "green" ? "good" : ev.cls === "red" ? "bad" : ev.cls === "amber" ? "warn" : ""),
      tile("Frames", fmtInt(sto && sto.frames), sto && isNum(sto.schema_version) ? "schema v" + sto.schema_version : null),
      tile("With image", fmtInt(sto && sto.frames_with_image), sto && isNum(sto.frames) && isNum(sto.frames_with_image) ? fmtInt(sto.frames - sto.frames_with_image) + " JPEG expired" : null),
      tile("Embeddings", fmtInt(sto && sto.embeddings_ok), sto && isNum(sto.embeddings_failed) ? fmtInt(sto.embeddings_failed) + " failed" : null, sto && sto.embeddings_failed > 0 ? "warn" : ""),
      tile("Stage-1 queue", fmtInt(s1 && s1.queue_depth), "max " + fmtInt(s1 && s1.max_queue_depth)),
      tile("Stage-2 queue", fmtInt(s2 && s2.queue_depth), "max " + fmtInt(s2 && s2.max_queue_depth)),
      tile("Retention", sto && isNum(sto.retention_hours) ? sto.retention_hours + " h" : DASH, "embeddings: " + (sto && isNum(sto.embedding_retention_days) ? sto.embedding_retention_days + " days" : DASH)),
      tile("Oldest frame", sto && sto.oldest_utc_ms ? fmtTs(sto.oldest_utc_ms) : DASH, sto && sto.oldest_utc_ms ? otherZone(sto.oldest_utc_ms) : null, "", true),
      tile("Newest frame", sto && sto.newest_utc_ms ? fmtTs(sto.newest_utc_ms) : DASH, sto && sto.newest_utc_ms ? otherZone(sto.newest_utc_ms) : null, "", true),
      tile("Snapshot age", st.viewer ? "n/a" : rt ? fmtDur(rt.snapshot_age_ms) : DASH, st.viewer ? "dataset viewer" : rt ? (rt.stale ? "STALE" : rt.running ? "running" : "not running") : "no runtime snapshot", !st.viewer && rt && (rt.stale || rt.running === false) ? "warn" : ""));
    // storage
    const used = sto ? [sto.jpeg_bytes, sto.vector_bytes, sto.db_bytes].reduce((a, v) => a + (isNum(v) ? v : 0), 0) : null;
    const quota = sto && isNum(sto.quota_bytes) ? sto.quota_bytes : null;
    const pct = used != null && quota ? Math.min(100, (used / quota) * 100) : null;
    const stor = h("div", { class: "vm-tile wide" + (pct != null && pct > 90 ? " warn" : "") },
      h("div", { class: "l", text: "Storage used (jpeg + vectors + db)" }),
      h("div", { class: "v", text: fmtBytes(used) + (quota ? " / " + fmtBytes(quota) : "") }),
      h("div", { class: "vm-bar", role: "progressbar", "aria-valuemin": "0", "aria-valuemax": "100", "aria-valuenow": pct != null ? Math.round(pct) : null },
        pct != null ? h("i", { class: pct > 90 ? "hot" : "", style: `width:${pct.toFixed(1)}%` }) : null),
      h("div", { class: "s", text: sto ? `jpeg ${fmtBytes(sto.jpeg_bytes)} · vectors ${fmtBytes(sto.vector_bytes)} · db ${fmtBytes(sto.db_bytes)} · disk free ${fmtBytes(sto.disk_free_bytes)}` : DASH }));
    tiles.insertBefore(stor, tiles.children[4]);
    // model
    const modelTile = h("div", { class: "vm-tile wide" }, h("div", { class: "l", text: "Model" }),
      h("div", { class: "v sm", text: mdl ? `${mdl.name || DASH} ${mdl.version || ""}` : DASH }),
      h("div", { class: "s", text: mdl ? `preprocessing ${mdl.preprocessing || DASH} · dim ${fmtInt(mdl.dimension)} · ${mdl.dtype || DASH} · ${mdl.metric || DASH}` : "no model information" }),
      h("div", { class: "s", text: `embedder status: ${(s2 && s2.status) || DASH} · vector backend: ${(s2 && s2.backend) || (sto && sto.vec_backend) || (st.vector_search_configured === false ? "not configured" : DASH)}` }),
      mdl && mdl.sha256 ? h("div", { class: "s vm-mono", title: "model SHA-256", text: "sha256 " + mdl.sha256 }) : null);
    tiles.insertBefore(modelTile, tiles.children[6]);
    // ingestion + embedding health, storage root (answers "is it actually storing the latest images, where, and with embeddings?")
    const ing = st.ingest || null;
    const ingCls = !ing ? "" : ing.state === "stalled" ? "bad" : ing.state === "ok" ? "good" : "warn";
    const ageTxt = ing && isNum(ing.age_ms) ? fmtDur(ing.age_ms) + " ago" : null;
    tiles.append(
      tile("Latest JPEG committed", ing && ing.last_commit_utc_ms ? fmtTs(ing.last_commit_utc_ms) : (ing && ing.newest_frame_utc_ms ? fmtTs(ing.newest_frame_utc_ms) : DASH),
        ing ? [ageTxt, "ingestion: " + ing.state].filter(Boolean).join(" · ") : null, ingCls, true),
      tile("Latest embedding", sto && sto.latest_embedding_utc_ms ? fmtTs(sto.latest_embedding_utc_ms) : DASH,
        sto && sto.latest_embedding_utc_ms ? otherZone(sto.latest_embedding_utc_ms) : (st.embedding_configured ? "none stored yet" : "embeddings disabled"), st.embedding_configured === false ? "warn" : "", true),
      h("div", { class: "vm-tile wide" }, h("div", { class: "l", text: "Storage root" }),
        h("div", { class: "v sm vm-mono", text: st.storage_root || DASH }),
        h("div", { class: "s", text: "config: " + (st.config_path || DASH) })));
    box.append(tiles);

    // notices: ingestion stopped / embeddings off / retention catch-up
    const notices = [];
    if (ing && ing.state === "stalled") notices.push(h("div", { class: "vm-banner bad", role: "alert" }, h("b", { text: "Frame ingestion stalled. " }), ing.message || ""));
    else if (ing && ing.retention_pending) notices.push(h("div", { class: "vm-banner info" }, "Clearing expired frames from an earlier run in the background. New frames are still being stored."));
    if (st.configured && !st.viewer && st.embedding_configured === false)
      notices.push(h("div", { class: "vm-banner warn" }, h("b", { text: "Stage 2 is disabled. " }),
        "New frames are stored without embeddings and image search is unavailable. Set visual_memory.vector_search_enabled and visual_memory.embedding.enabled to true in the active config and restart Argus."));
    else if (st.embedding_configured && !st.viewer && rt && !rt.stale && !s2)
      notices.push(h("div", { class: "vm-banner warn" }, h("b", { text: "Embeddings are configured but not running. " }), "The model may have failed to load; see the Argus log."));
    if (notices.length) box.insertBefore(h("div", { class: "vm-stack" }, notices), tiles);

    // sampling & processing (existing runtime counters; nothing new is counted)
    {
      const smp = st.sampling, cap = st.capture;
      const sum = (list) => { let t = 0, any = false; for (const [g, k] of list) { const v = cnt(st, g, k); if (v != null) { t += v; any = true; } } return any ? t : null; };
      const dropped = sum(DROPS), failed = sum(HARD_FAIL);
      const failedAll = (failed == null && !(sto && isNum(sto.embeddings_failed))) ? null : (failed || 0);
      const intv = smp && isNum(smp.interval_ms) ? (smp.interval_ms % 1000 === 0 ? smp.interval_ms / 1000 + " s" : smp.interval_ms + " ms") : DASH;
      const pt = h("div", { class: "vm-tiles" });
      pt.append(
        tile("Sampling interval", intv, smp ? `stored ${fmtInt(smp.stored_width)}×${fmtInt(smp.stored_height)} · JPEG q${fmtInt(smp.jpeg_quality)}` : "not configured"),
        tile("Capture FPS", cap && isNum(cap.fps) ? (Math.round(cap.fps * 10) / 10).toString() : DASH,
          cap && isNum(cap.fps) ? `camera ${fmtInt(cap.width)}×${fmtInt(cap.height)} · ${fmtInt(cap.dropped_frames)} frames dropped (Argus)` : "Argus metrics unavailable", cap && isNum(cap.fps) && cap.fps < 25 ? "warn" : ""),
        tile("Frames sampled", fmtInt(s1 && s1.samples_seen), "offered to Visual Memory"),
        tile("Accepted", fmtInt(s1 && s1.accepted), s1 && isNum(s1.encoded) ? fmtInt(s1.encoded) + " JPEG-encoded" : null),
        tile("Embedded", fmtInt(s2 && s2.embedded), s2 ? "stage 2 · " + (s2.status || DASH) : st.viewer ? "dataset viewer" : "stage 2 not running"),
        tile("Queue-full drops", fmtInt(cnt(st, "stage1", "dropped_queue_full")), "writer could not keep up (or was busy)", cnt(st, "stage1", "dropped_queue_full") > 0 ? "warn" : ""),
        tile("Invalid frames", fmtInt(cnt(st, "stage1", "dropped_invalid")), "rejected before queueing", cnt(st, "stage1", "dropped_invalid") > 0 ? "warn" : ""),
        tile("Failed", fmtInt(failedAll), "write / DB / embedding failures", failedAll > 0 ? "bad" : ""),
        tile("Embedding model", mdl ? (mdl.name || DASH) : DASH, mdl ? `${mdl.version || ""} · dim ${fmtInt(mdl.dimension)} · ${mdl.preprocessing || DASH}` : "no model information", "", true));
      box.append(h("section", { class: "panel" }, h("div", { class: "panel-head" }, h("h2", { text: "Sampling & processing" }),
        h("span", { class: "hint", text: st.viewer ? "dataset viewer: live counters are not available" : "counters since the Argus process started" })), pt));
    }

    // counters
    const chips = h("div", { class: "vm-chips" });
    const add = (l, v, hard) => chips.append(h("span", { class: "vm-chip" + (isNum(v) && v > 0 ? (hard ? " bad" : " warn") : "") }, l, h("b", { text: fmtInt(v) })));
    for (const [g, k, l] of DROPS) add(l, cnt(st, g, k), false);
    for (const [g, k, l] of HARD_FAIL) add(l, cnt(st, g, k), true);
    add("Failed embeddings (stored)", sto ? sto.embeddings_failed : null, true);
    box.append(h("section", { class: "panel" }, h("div", { class: "panel-head" }, h("h2", { text: "Drops & failures" }), h("span", { class: "hint", text: "counters since the Argus process started" })), chips));

    if (Array.isArray(st.errors) && st.errors.length)
      box.append(h("section", { class: "panel" }, h("div", { class: "panel-head" }, h("h2", { text: "Reported problems" })), h("ul", { class: "vm-errors" }, st.errors.map((e) => h("li", { text: String(e) })))));

    box.append(h("section", { class: "panel" }, h("div", { class: "panel-head" }, h("h2", { text: "Storage & retention" })),
      h("p", { text: "Visual Memory keeps a rolling window of JPEG frames and, separately, a compact embedding vector per frame. JPEG files are deleted when they reach the JPEG retention age (" +
        (sto && isNum(sto.retention_hours) ? sto.retention_hours + " h" : "unknown") + ") or when the storage quota is reached; the frame row and its embedding are kept for the embedding retention period (" +
        (sto && isNum(sto.embedding_retention_days) ? sto.embedding_retention_days + " days" : "unknown") + "). Expired frames therefore still appear in the gallery and in search results, with a placeholder instead of an image." }),
      h("p", { class: "muted", text: "Server time " + (st.server_time_utc_ms ? fmtZone(st.server_time_utc_ms, "utc") : DASH) + " · sources: " + ((sto && Array.isArray(sto.sources) && sto.sources.join(", ")) || DASH) })));
  }

  function openDetail(id) {
    const body = h("div", null, h("div", { class: "vm-empty" }, h("span", { class: "vm-spin" }), " Loading…"));
    const m = openModal("Frame #" + id, body);
    (async () => {
      const r = await api("/frames/" + encodeURIComponent(id));
      clear(body);
      if (r.status !== 200 || !r.data) {
        body.append(r.status === 401 ? authBox() : h("div", { class: "vm-banner bad", text: r.status === 404 ? "Frame not found (it may have been deleted)." : "Could not load frame: " + ((r.data && (r.data.message || r.data.error)) || "HTTP " + (r.status || "network")) }));
        return;
      }
      const f = r.data;
      const copyBtn = h("button", { class: "vm-btn small", type: "button", text: "Copy", onclick: (e) => copyText(f.sha256 || "", e.currentTarget) });
      const dd = (k, v) => [h("dt", { text: k }), h("dd", null, v)];
      const both = (ms) => h("span", null, tsEl(ms), h("br"), h("span", { class: "muted", text: otherZone(ms) }));
      const dl = h("dl", { class: "vm-dl" },
        dd("ID", String(f.id)), dd("Captured", both(f.capture_utc_ms)), dd("Stored", both(f.created_utc_ms)),
        dd("Source", f.source_id || DASH), dd("Source type", f.source_type || DASH), dd("Device", f.device_id || DASH),
        dd("Size", `${fmtInt(f.width)} × ${fmtInt(f.height)} px · ${fmtBytes(f.jpeg_bytes)}`), dd("Sequence", fmtInt(f.sequence)),
        dd("SHA-256", h("span", null, h("span", { class: "vm-mono", text: f.sha256 || DASH }), " ", f.sha256 ? copyBtn : null)),
        dd("JPEG", f.image_available === false ? "expired (metadata retained)" : f.image_exists === false ? "file missing" : "available"),
        dd("JPEG expires", f.retention && f.retention.jpeg_expires_utc_ms ? both(f.retention.jpeg_expires_utc_ms) : DASH),
        dd("Embedding expires", f.retention && f.retention.embedding_expires_utc_ms ? both(f.retention.embedding_expires_utc_ms) : DASH));
      const embs = Array.isArray(f.embeddings) ? f.embeddings : [];
      const tbl = embs.length ? h("div", { class: "vm-table-wrap" }, h("table", { class: "vm-table" },
        h("thead", null, h("tr", null, ["Model", "Version", "Preproc.", "Dim", "Status", "Created", "Error"].map((x) => h("th", { text: x })))),
        h("tbody", null, embs.map((e) => h("tr", null, h("td", { text: e.model_name || DASH }), h("td", { text: e.model_version || DASH }), h("td", { text: e.preprocessing || DASH }),
          h("td", { text: fmtInt(e.dimension) }), h("td", null, h("span", { class: "vm-badge " + (e.status === "ok" ? "ok" : "bad"), text: e.status || DASH })),
          h("td", null, tsEl(e.created_utc_ms)), h("td", { text: e.error || "" })))))) : h("p", { class: "muted", text: "No embedding records for this frame." });
      const find = h("button", { class: "vm-btn primary", type: "button", text: "Find similar", onclick: () => { m.close(); location.hash = "#search?frame=" + f.id; } });
      const objOn = !!(VMUI.objects && S.status && S.status.objects && S.status.objects.enabled) && f.image_available !== false && f.image_exists !== false;
      const selObj = objOn ? h("button", { class: "vm-btn", type: "button", text: "Select Object", title: "Draw a box around an object in this frame to search for or index it",
        onclick: () => VMUI.objects.openSelector({ id: f.id, width: f.width, height: f.height, capture_utc_ms: f.capture_utc_ms, source_id: f.source_id, image_available: f.image_available, image_exists: f.image_exists }, m) }) : null;
      body.append(h("div", { class: "vm-detail" },
        h("div", null, h("div", { class: "vm-bigimg" }, frameImage(f, "image", 0, true)),
          h("div", { class: "vm-row", style: "margin-top:10px" }, find, selObj, h("a", { class: "vm-btn", href: `${API}/frames/${f.id}/image`, target: "_blank", rel: "noopener", style: f.image_available === false || f.image_exists === false ? "display:none" : "text-decoration:none", text: "Open full image" }))),
        h("div", null, dl)),
        h("h3", { style: "margin-top:16px", text: "Embedding records" }), tbl);
    })();
    tz.subs.push(() => { if (body.isConnected) refreshTs(body); });
  }

  function initMemory() {
    if (memInit) return; memInit = true;
    memGallery = makeGallery({ main: true, onPick: (f) => openDetail(f.id) });
    $("view-memory").append(h("div", { class: "vm-stack" },
      h("div", { class: "vm-banner", style: "border-left-color:var(--present)" }, h("p", null, h("b", { text: "Visual Memory" }), " keeps a rolling set of JPEG frames from the audience camera with image embeddings, for scene similarity search. It is not used for identity matching or proof of play.")),
      memStatusBox,
      h("section", { class: "panel" }, h("div", { class: "panel-head" }, h("h2", { text: "Gallery" }), h("span", { class: "hint", text: "times shown in the selected zone; hover for UTC + local" })), memGallery.el)));
    onStatus(renderMemStatus);
    tz.subs.push(renderMemStatus);
    renderMemStatus();
  }

  // ---- Visual Search page --------------------------------------------------------------------
  const SR = { ref: null /* {type:"upload"|"frame", blobUrl?, file?, frame?} */, ctrl: null, built: false };
  const searchEls = {};

  function errorMessage(httpStatus, data) {
    const code = (data && data.status && data.status.code) || (data && data.error) || "";
    const server = (data && data.status && data.status.message) || (data && data.message) || "";
    const M = {
      busy: "Another search is already running. Visual Memory runs one search at a time; try again in a moment.",
      timeout: "The search timed out on the server. Narrow the time window or source and try again.",
      not_enabled: "Visual search is not enabled (Visual Memory or embeddings are disabled in the active config).",
      provider_failed: "The embedding model could not be loaded or failed to run on the device.",
      model_mismatch: "The stored vectors were produced by a different model than the one currently loaded, so they cannot be compared.",
      dimension_mismatch: "The query vector size does not match the stored vectors.",
      invalid_vector: "The query produced an invalid embedding vector.",
      invalid_argument: "The request was rejected as invalid.",
      database_error: "The Visual Memory database reported an error.",
      cancelled: "The search was cancelled.",
      unauthorized: "Access token required.",
      too_large: "The image is larger than 8 MiB.",
      forbidden: "The request was refused (cross-origin).",
      not_found: "Frame not found; it may have been deleted.",
      vm_api_unavailable: "The Visual Memory API is not running.",
    };
    let m = M[code];
    if (!m) {
      if (httpStatus === 429) m = M.busy; else if (httpStatus === 504) m = M.timeout; else if (httpStatus === 503) m = M.not_enabled;
      else if (httpStatus === 401) m = M.unauthorized; else if (httpStatus === 413) m = M.too_large; else if (httpStatus === 403) m = M.forbidden;
      else if (httpStatus === 404 || httpStatus === 502) m = "The Visual Memory API is not running (HTTP " + httpStatus + ").";
      else if (httpStatus === 0) m = "Network error: could not reach the dashboard server.";
      else m = "Search failed (HTTP " + httpStatus + ").";
    }
    return m + (server && !m.includes(server) ? " Server says: " + server : "") + (code ? "  [" + code + "]" : "");
  }

  async function sniffJpeg(file) {
    const b = new Uint8Array(await file.slice(0, 3).arrayBuffer());
    return b.length === 3 && b[0] === 0xff && b[1] === 0xd8 && b[2] === 0xff;
  }
  async function setUpload(file) {
    const E = searchEls;
    clear(E.refMsg);
    if (!file) return;
    if (file.size > MAX_UPLOAD) { E.refMsg.append(h("div", { class: "vm-banner bad", text: `Image is ${fmtBytes(file.size)}; the limit is 8 MiB.` })); return; }
    if ((file.type && file.type !== "image/jpeg") || !(await sniffJpeg(file))) { E.refMsg.append(h("div", { class: "vm-banner bad", text: "Only JPEG images (image/jpeg) are accepted." })); return; }
    clearRef();
    SR.ref = { type: "upload", file, blobUrl: URL.createObjectURL(file) };
    renderRef();
  }
  function clearRef() { if (SR.ref && SR.ref.blobUrl) URL.revokeObjectURL(SR.ref.blobUrl); SR.ref = null; }
  async function setFrameRef(idOrFrame) {
    const id = typeof idOrFrame === "object" ? idOrFrame.id : Number(idOrFrame);
    if (!isFinite(id)) return;
    let f = typeof idOrFrame === "object" ? idOrFrame : null;
    if (!f || f.image_available === undefined) { const r = await api("/frames/" + id); if (r.status === 200 && r.data) f = r.data; else f = f || { id, image_available: false }; }
    clearRef(); clear(searchEls.refMsg);
    SR.ref = { type: "frame", frame: f };
    renderRef();
  }
  function renderRef() {
    const E = searchEls, box = clear(E.refPreview);
    if (!SR.ref) { box.append(h("p", { class: "muted", text: "No reference selected." })); updateRunBtn(); return; }
    if (SR.ref.type === "upload") {
      box.append(h("div", { class: "vm-ref" }, h("div", { class: "vm-thumb" }, h("img", { src: SR.ref.blobUrl, alt: "Reference upload" })),
        h("div", null, h("div", { text: "Uploaded reference" }), h("div", { class: "muted", text: `${SR.ref.file.name || "pasted image"} · ${fmtBytes(SR.ref.file.size)}` }),
          h("div", { class: "muted", text: "Processed in memory by the device; never stored." }))));
    } else {
      const f = SR.ref.frame;
      box.append(h("div", { class: "vm-ref" }, h("div", { class: "vm-thumb" }, frameImage(f, "thumb", 240)),
        h("div", null, h("div", { text: "Stored frame #" + f.id }), f.capture_utc_ms ? h("div", { class: "muted" }, tsEl(f.capture_utc_ms)) : null, f.source_id ? h("div", { class: "muted", text: f.source_id }) : null,
          h("div", { class: "muted", text: "The frame itself is excluded from results." }))));
    }
    updateRunBtn();
  }
  function searchAvailability() {
    if (S.apiState === "down") return { ok: false, why: "Visual Memory API not running." };
    if (S.apiState === "auth") return { ok: false, why: "Access token required." };
    const st = S.status;
    if (!st) return { ok: false, why: "Loading status…" };
    if (!st.search || st.search.available === false) return { ok: false, why: (st.search && st.search.reason) || (st.configured ? "Search is not available." : "Visual Memory is not configured.") };
    return { ok: true };
  }
  function updateRunBtn() {
    const E = searchEls; if (!E.run) return;
    const av = searchAvailability();
    E.fs.disabled = !av.ok;
    clear(E.avail);
    if (!av.ok) E.avail.append(S.apiState === "auth" ? authBox() : S.apiState === "down" ? apiDownBox() : h("div", { class: "vm-banner warn" }, h("b", { text: "Search is unavailable. " }), av.why));
    E.run.disabled = !av.ok || !SR.ref || !!SR.ctrl;
    E.minSim.disabled = !E.minOn.checked;
  }

  function buildSearch() {
    if (SR.built) return; SR.built = true;
    const E = searchEls, root = $("view-search");
    E.avail = h("div"); E.refMsg = h("div"); E.refPreview = h("div"); E.results = h("div", { "aria-live": "polite" });
    const fileIn = h("input", { type: "file", accept: "image/jpeg", style: "display:none", "aria-label": "Choose a JPEG reference image" });
    fileIn.addEventListener("change", () => { if (fileIn.files[0]) setUpload(fileIn.files[0]); fileIn.value = ""; });
    const drop = h("div", { class: "vm-drop", tabindex: "0", role: "button", "aria-label": "Upload reference JPEG: click, drop a file, or paste an image" },
      h("div", null, h("b", { text: "Drop a JPEG here, click to choose, or paste an image" })), h("div", { class: "muted", text: "JPEG only, up to 8 MiB" }));
    drop.addEventListener("click", () => fileIn.click());
    drop.addEventListener("keydown", (e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); fileIn.click(); } });
    ["dragenter", "dragover"].forEach((ev) => drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.add("over"); }));
    ["dragleave", "drop"].forEach((ev) => drop.addEventListener(ev, (e) => { e.preventDefault(); drop.classList.remove("over"); }));
    drop.addEventListener("drop", (e) => { const f = e.dataTransfer && e.dataTransfer.files[0]; if (f) setUpload(f); });
    document.addEventListener("paste", (e) => {
      if (currentView !== "search" || !e.clipboardData) return;
      for (const it of e.clipboardData.items || []) if (it.kind === "file") { const f = it.getAsFile(); if (f) { setUpload(f); e.preventDefault(); return; } }
    });
    const pick = h("button", { class: "vm-btn", type: "button", text: "Pick a stored frame…", onclick: openPicker });

    E.topK = h("input", { type: "number", min: "1", max: "50", step: "1", value: "5", "aria-label": "Top K" });
    E.tf = makeTimeFilter(() => {});
    E.src = sourceSelect(() => {});
    E.minOn = h("input", { type: "checkbox" }); E.minOn.addEventListener("change", updateRunBtn);
    E.minSim = h("input", { type: "number", min: "0", max: "1", step: "0.01", value: "0.80", disabled: true, "aria-label": "Minimum similarity" });
    E.reqImg = h("input", { type: "checkbox" });
    E.run = h("button", { class: "vm-btn primary", type: "button", text: "Search", onclick: runSearch, disabled: true });
    E.cancel = h("button", { class: "vm-btn", type: "button", text: "Cancel", hidden: true, onclick: () => { if (SR.ctrl) SR.ctrl.abort(); } });
    E.spin = h("span", { class: "vm-spin", hidden: true });
    E.runMsg = h("span", { class: "muted" });

    E.fs = h("fieldset", { class: "vm-fs" }, h("div", { class: "vm-search-grid" },
      h("div", { class: "panel" }, h("div", { class: "panel-head" }, h("h2", { text: "1. Reference" })), drop, fileIn,
        h("div", { class: "vm-row", style: "margin-top:10px" }, pick), E.refMsg, E.refPreview),
      h("div", { class: "panel" }, h("div", { class: "panel-head" }, h("h2", { text: "2. Options" })),
        h("div", { class: "vm-stack" },
          h("div", { class: "vm-row" }, h("label", null, "Top K (1–50)", E.topK), h("label", null, "Source", E.src),
            h("label", { class: "inline", style: "margin-top:16px" }, E.reqImg, "Require JPEG still available")),
          E.tf.el,
          h("div", { class: "vm-row" }, h("label", { class: "inline" }, E.minOn, "Minimum similarity (off by default)"), E.minSim),
          h("div", { class: "vm-row" }, E.run, E.cancel, E.spin, E.runMsg)))));

    E.full = h("div", { class: "vm-stack" }, h("div", { class: "vm-note", text: SEARCH_NOTE }), E.avail, E.fs, E.results);
    E.objSlot = h("div", { hidden: true });
    E.modeFull = h("button", { class: "vm-btn on", type: "button", text: "Full frame search", onclick: () => setSearchMode("frame") });
    E.modeObj = h("button", { class: "vm-btn", type: "button", text: "Object search", onclick: () => setSearchMode("objects") });
    root.append(h("div", { class: "vm-stack" }, h("div", { class: "vm-row", role: "group", "aria-label": "Search mode" }, E.modeFull, E.modeObj), E.full, E.objSlot));
    renderRef();
    onStatus(() => { updateRunBtn(); });
    tz.subs.push(() => refreshTs(root));
    document.addEventListener("vm-authed", updateRunBtn);
  }

  let searchMode = "frame";
  function setSearchMode(mode) {
    searchMode = mode;                                   // the requested mode survives until objects.js has registered
    if (currentView !== "search") { location.hash = "#search"; }
    buildSearch();
    const E = searchEls, obj = mode === "objects" && !!VMUI.objects;
    E.full.hidden = obj; E.objSlot.hidden = !obj;
    E.modeFull.classList.toggle("on", !obj); E.modeObj.classList.toggle("on", obj);
    E.modeFull.setAttribute("aria-pressed", String(!obj)); E.modeObj.setAttribute("aria-pressed", String(obj));
    if (obj) VMUI.objects.mountSearch(E.objSlot);
  }

  function openPicker() {
    const g = makeGallery({ selected: SR.ref && SR.ref.type === "frame" ? SR.ref.frame.id : null, onPick: (f) => { m.close(); setFrameRef(f); } });
    const m = openModal("Pick a stored frame", h("div", null, h("p", { class: "muted", text: "Choose a frame to use as the search reference." }), g.el), { wide: true });
    g.load();
  }

  async function runSearch() {
    const E = searchEls;
    if (!SR.ref || SR.ctrl) return;
    const k = Math.floor(Number(E.topK.value));
    if (!(k >= 1 && k <= 50)) { E.runMsg.textContent = "Top K must be between 1 and 50."; return; }
    const q = new URLSearchParams({ top_k: k });
    const t = E.tf.get();
    if (t.start != null) q.set("start", t.start);
    if (t.end != null) q.set("end", t.end);
    if (t.start != null && t.end != null && t.end <= t.start) { E.runMsg.textContent = "“To” must be after “From”."; return; }
    if (E.src.value) q.set("source", E.src.value);
    if (E.minOn.checked) {
      const ms = Number(E.minSim.value);
      if (!(ms >= 0 && ms <= 1) || E.minSim.value === "") { E.runMsg.textContent = "Minimum similarity must be between 0 and 1."; return; }
      q.set("min_similarity", ms);
    }
    if (E.reqImg.checked) q.set("require_image", "1");
    const ref = SR.ref;
    let url, init = { method: "POST", signal: null, credentials: "same-origin", cache: "no-store" };
    if (ref.type === "upload") { url = "/search/image?" + q; init.body = ref.file; init.headers = { "Content-Type": "image/jpeg" }; }
    else { q.set("frame_id", ref.frame.id); url = "/search/frame?" + q; }
    SR.ctrl = new AbortController(); init.signal = SR.ctrl.signal;
    E.cancel.hidden = false; E.spin.hidden = false; E.runMsg.textContent = "Searching…";
    clear(E.results); updateRunBtn();
    const r = await api(url, init);
    const aborted = r.aborted;
    SR.ctrl = null; E.cancel.hidden = true; E.spin.hidden = true; E.runMsg.textContent = "";
    updateRunBtn();
    if (aborted) { E.results.append(h("div", { class: "vm-banner" }, "Search cancelled.")); return; }
    const d = r.data;
    if (r.status !== 200 || !d || (d.status && d.status.code && d.status.code !== "ok")) {
      E.results.append(h("div", { class: "vm-banner bad" }, h("b", { text: "Search failed. " }), errorMessage(r.status, d)));
      if (r.status === 401) { E.results.append(authBox()); pollStatus(); }
      return;
    }
    renderResults(d, ref);
  }

  function renderResults(d, ref) {
    const E = searchEls, box = clear(E.results);
    const res = Array.isArray(d.results) ? d.results : [];
    const stt = d.status || {};
    const mdl = d.model || {}, qy = d.query || {};
    box.append(h("div", { class: "vm-label", text: d.label || "" }));
    box.append(h("p", { class: "muted", style: "margin-top:10px", text:
      `${res.length} result${res.length === 1 ? "" : "s"} · ${isNum(stt.elapsed_ms) ? stt.elapsed_ms.toFixed(1) + " ms" : DASH} · ${fmtInt(stt.candidates_scanned)} candidates scanned` +
      ` · query: ${qy.type || DASH}${qy.embedded_with ? " (" + qy.embedded_with + ")" : ""} · model ${mdl.name || DASH} ${mdl.version || ""} ${mdl.preprocessing || ""} dim ${fmtInt(mdl.dimension)}` }));
    if (!res.length) { box.append(h("div", { class: "vm-empty", text: "No similar frames found for these options." })); return; }
    const grid = h("div", { class: "vm-grid" });
    res.forEach((x) => {
      const sim = isNum(x.similarity) ? x.similarity : null;
      const avail = x.image_available === false ? h("span", { class: "vm-badge warn", text: "JPEG expired" }) : x.image_exists === false ? h("span", { class: "vm-badge bad", text: "JPEG missing" }) : h("span", { class: "vm-badge ok", text: "JPEG available" });
      grid.append(h("button", { class: "vm-card", type: "button", onclick: () => openCompare(ref, x, d), "aria-label": `Result ${x.rank}, frame ${x.frame_id}, similarity ${sim != null ? sim.toFixed(4) : "unknown"}` },
        h("div", { class: "vm-thumb" }, h("span", { class: "vm-rank", text: "#" + x.rank }), frameImage({ id: x.frame_id, image_available: x.image_available, image_exists: x.image_exists }, "thumb", 240)),
        h("div", { class: "meta" },
          h("span", { class: "vm-score", text: sim != null ? sim.toFixed(4) : DASH }),
          h("div", { class: "vm-bar", "aria-hidden": "true" }, sim != null ? h("i", { style: `width:${Math.max(0, Math.min(1, sim)) * 100}%` }) : null),
          tsEl(x.capture_utc_ms, "t"), h("span", { class: "src", title: x.source_id || "", text: x.source_id || DASH }),
          h("div", { class: "vm-badges" }, h("span", { class: "vm-badge", text: "#" + x.frame_id }), avail))));
    });
    box.append(grid);
  }

  function openCompare(ref, x, d) {
    let refSide;
    if (ref.type === "upload") refSide = h("div", { class: "vm-bigimg" }, h("img", { src: ref.blobUrl, alt: "Reference" }));
    else refSide = h("div", { class: "vm-bigimg" }, frameImage(ref.frame, "image", 0, true));
    const matchSide = h("div", { class: "vm-bigimg" }, frameImage({ id: x.frame_id, image_available: x.image_available, image_exists: x.image_exists }, "image", 0, true));
    const both = (ms) => h("span", null, tsEl(ms), " ", h("span", { class: "muted", text: "(" + otherZone(ms) + ")" }));
    const refDl = h("dl", { class: "vm-dl" }, h("dt", { text: "Type" }), h("dd", { text: ref.type === "upload" ? "Uploaded file (local preview)" : "Stored frame #" + ref.frame.id }),
      ref.type === "frame" && ref.frame.capture_utc_ms ? [h("dt", { text: "Captured" }), h("dd", null, both(ref.frame.capture_utc_ms))] : null,
      ref.type === "frame" && ref.frame.source_id ? [h("dt", { text: "Source" }), h("dd", { text: ref.frame.source_id })] : null);
    const mDl = h("dl", { class: "vm-dl" }, h("dt", { text: "Frame" }), h("dd", { text: "#" + x.frame_id }), h("dt", { text: "Captured" }), h("dd", null, both(x.capture_utc_ms)),
      h("dt", { text: "Source" }), h("dd", { text: x.source_id || DASH }), h("dt", { text: "Size" }), h("dd", { text: `${fmtInt(x.width)} × ${fmtInt(x.height)}` }),
      h("dt", { text: "Similarity" }), h("dd", { text: isNum(x.similarity) ? x.similarity.toFixed(4) : DASH }), h("dt", { text: "Distance" }), h("dd", { text: isNum(x.distance) ? x.distance.toFixed(4) : DASH }));
    const open = h("button", { class: "vm-btn small", type: "button", text: "Open frame detail", onclick: () => { openDetail(x.frame_id); } });
    const similar = h("button", { class: "vm-btn small", type: "button", text: "Find similar to this match", onclick: () => { modal.close(); setFrameRef(x.frame_id); } });
    const body = h("div", null, h("div", { class: "vm-label", style: "margin-bottom:12px", text: d.label || "" }),
      h("div", { class: "vm-cmp" }, h("div", null, h("h3", { text: "Reference" }), refSide, refDl),
        h("div", null, h("h3", { text: `Match #${x.rank} — similarity ${isNum(x.similarity) ? x.similarity.toFixed(4) : DASH}` }), matchSide, mDl)),
      h("div", { class: "vm-row", style: "margin-top:12px" }, open, similar));
    const modal = openModal("Compare reference and match", body, { wide: true });
  }

  function initSearch() {
    buildSearch();
    updateRunBtn();
  }

  // ---- routing -----------------------------------------------------------------------------
  const VIEWS = ["audience", "memory", "search", "pop"];
  let currentView = "audience";
  function route() {
    const raw = location.hash.replace(/^#/, "");
    const [name, qs] = raw.split("?");
    const view = VIEWS.includes(name) ? name : "audience";
    currentView = view;
    document.body.dataset.view = view;
    document.querySelectorAll(".view").forEach((v) => { v.hidden = v.dataset.view !== view; });
    document.querySelectorAll("#tabs a").forEach((a) => {
      const on = a.dataset.tab === view; a.classList.toggle("active", on);
      if (on) a.setAttribute("aria-current", "page"); else a.removeAttribute("aria-current");
    });
    const titles = { audience: "Audience Measurement", memory: "Visual Memory", search: "Visual Search", pop: "Proof of Play" };
    document.title = "Argus — " + titles[view];
    if (view === "memory") { initMemory(); memGallery.load(); renderMemStatus(); }
    if (view === "search") {
      initSearch();
      const prm = new URLSearchParams(qs || "");
      const fid = prm.get("frame");
      if (fid) { setSearchMode("frame"); setFrameRef(Number(fid)); history.replaceState(null, "", "#search"); }
      else if (prm.get("mode") === "objects") { setSearchMode("objects"); history.replaceState(null, "", "#search"); }
      else setSearchMode(searchMode);
    }
    if (view === "pop" && window.VMUI && window.VMUI.pop) window.VMUI.pop();
    if (view === "audience") window.dispatchEvent(new Event("resize"));
    window.scrollTo(0, 0);
  }
  const VMUI = window.VMUI = {
    h, api, openModal, frameImage, loadImg, tsEl, otherZone, fmtInt, fmtBytes, isNum, DASH, API, S, clear, errorMessage, authBox,
    makeTimeFilter, sourceSelect, makeGallery, openDetail, onStatus, pollStatus, refreshTs, tzSubscribe: (f) => tz.subs.push(f),
    showSearchMode: (m) => setSearchMode(m), objects: null,
    registerObjects(o) { VMUI.objects = o; if (SR.built) setSearchMode(searchMode); },
    pop: null,
    registerPop(show) { VMUI.pop = show; if (currentView === "pop") show(); },
  };
  window.addEventListener("hashchange", route);
  onStatus(renderPill);
  tz.subs.push(() => { });
  route();
  pollStatus();
  schedulePoll();
})();
