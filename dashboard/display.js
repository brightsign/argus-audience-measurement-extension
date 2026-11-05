/* Display output preview: shows what the player is actually displaying (OS Resilience rendered frames: weston VOP2 writeback,
   an approximation of the composed output, not a bit-exact HDMI readback), in the Proof of Play view.
   Everything goes through the same-origin dashboard proxy (/api/display/, GET only). The panel stays hidden unless
   visual_memory.display.enabled is true, and nothing is requested while the Proof of Play view or the tab is not visible: the
   frame requests are themselves the "viewer interest" that makes the OS producer capture (it idles a few seconds after the last one). */
(() => {
  "use strict";
  const API = "/api/display";
  const FRAME_MS = 200;            // matches the OS producer cadence (5 FPS)
  const STATUS_MS = 1000;
  const panel = document.getElementById("dispPanel");
  if (!panel) return;
  const img = document.getElementById("dispImg"), empty = document.getElementById("dispEmpty"), meta = document.getElementById("dispMeta");
  const view = document.getElementById("view-pop");
  let enabled = null, seq = 0, running = false, url = null, shown = [], st = null, lastRecv = 0, lastStatus = 0;

  const onScreen = () => !document.hidden && view && !view.hidden;
  const fmtAge = (ms) => (ms == null ? "—" : ms < 1000 ? Math.round(ms) + " ms" : (ms / 1000).toFixed(1) + " s");
  const fmtTs = (ms) => (ms ? new Date(ms).toLocaleTimeString() : "—");

  function row(k, v, cls) {
    const dt = document.createElement("dt"); dt.textContent = k;
    const dd = document.createElement("dd");
    if (cls) { const b = document.createElement("span"); b.className = "disp-badge " + cls; b.textContent = v; dd.appendChild(b); } else dd.textContent = v;
    meta.append(dt, dd);
  }
  function renderMeta() {
    meta.replaceChildren();
    if (!st) return;
    const h = st.health || {}, lf = st.last_frame || null;
    const dh = st.display_health || "unavailable";
    row("Display health", dh === "available" ? "available" : dh === "stale" ? "stale" : dh === "idle" ? "starting" : "unavailable", dh === "available" ? "ok" : dh === "stale" || dh === "idle" ? "warn" : "bad");
    if (st.state && st.state !== "ok" && st.detail) row("Detail", st.detail);
    row("Frame source", "Weston / VOP2 writeback (approximate)");
    row("Capture rate", (st.fps != null ? st.fps.toFixed(1) : "—") + " fps");
    row("Captured", lf ? fmtTs(lf.capture_utc_ms) : "—");
    row("Frame age", lf ? fmtAge(lf.age_ms) : "—", lf && lf.age_ms > (st.config ? st.config.max_age_ms : 1200) ? "warn" : null);
    row("Size", lf ? lf.width + "×" + lf.height + " (" + Math.round(lf.bytes / 1024) + " KB)" : "—");
    row("Time uncertainty", lf ? "≤ " + fmtAge(lf.uncertainty_ms) : "—");
    row("Black screen", h.black_screen ? "black for " + fmtAge(h.black_duration_ms) : "no", h.black_screen ? "bad" : null);
    row("Frozen frame", h.frozen_frame ? "frozen for " + fmtAge(h.frozen_duration_ms) : "no", h.frozen_frame ? "warn" : null);
    const c = st.counters || {};
    if (c.rpc_errors || c.invalid_jpeg || c.ambiguous_discarded) row("Rejected", `${c.rpc_errors || 0} errors · ${c.invalid_jpeg || 0} invalid · ${c.ambiguous_discarded || 0} ambiguous`);
  }

  async function pollStatus() {
    try {
      const r = await fetch(API + "/status", { cache: "no-store", credentials: "same-origin" });
      if (!r.ok) { enabled = null; panel.hidden = true; return; }
      st = await r.json();
      enabled = !!st.enabled;
      panel.hidden = !enabled;
      if (enabled) renderMeta();
    } catch (_) { enabled = null; panel.hidden = true; }
    lastStatus = Date.now();
  }

  async function pullFrame() {
    try {
      const r = await fetch(API + "/frame.jpg?after=" + seq, { cache: "no-store", credentials: "same-origin" });
      if (r.status === 204) return;                                   // nothing newer: cheap answer
      if (!r.ok) { if (!lastRecv || Date.now() - lastRecv > 3000) { img.hidden = true; empty.hidden = false; empty.textContent = "No current display frame"; } return; }
      const n = parseInt(r.headers.get("X-Display-Seq") || "0", 10);
      const blob = await r.blob();
      const u = URL.createObjectURL(blob);
      img.onload = () => { if (url) URL.revokeObjectURL(url); url = u; };
      img.src = u; img.hidden = false; empty.hidden = true;
      seq = n || seq; lastRecv = Date.now();
    } catch (_) { /* network blip: next tick retries */ }
  }

  async function loop() {
    if (running) return;
    running = true;
    try {
      while (onScreen()) {
        const t0 = Date.now();
        if (t0 - lastStatus >= STATUS_MS || enabled === null) await pollStatus();
        if (enabled) await pullFrame();
        else await new Promise((res) => setTimeout(res, 5000));       // disabled: look again rarely
        const wait = FRAME_MS - (Date.now() - t0);
        if (wait > 0) await new Promise((res) => setTimeout(res, wait));
      }
    } finally { running = false; }
  }

  function kick() { if (onScreen() && !running) loop(); }
  document.addEventListener("visibilitychange", kick);
  window.addEventListener("hashchange", () => setTimeout(kick, 50));
  window.addEventListener("load", () => setTimeout(kick, 100));
  if (view) new MutationObserver(kick).observe(view, { attributes: true, attributeFilter: ["hidden"] });
})();
