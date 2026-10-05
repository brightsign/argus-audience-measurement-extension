/* Argus audience dashboard. Subscribes to analytics/v7.0 over MQTT-WebSockets
   (served by the bundled mosquitto), or falls back to a built-in simulator so the
   dashboard looks right offline. Renders everything on <canvas>. */
(() => {
  "use strict";

  // Validated series colors (see dataviz validation): present=cyan, gazing=amber.
  const C = {
    present: "#2a8fbf", presentGlow: "#4cc3f0",
    gazing: "#c07f1e", gazingGlow: "#f2b24c",
    ink2: "#9fb0c6", line: "#24314a", muted: "#64748b", grid: "#17202f",
  };
  const ENGAGED_DWELL = 3.0;         // seconds, funnel "engaged" threshold
  const WINDOW_MS = 60_000;          // timeline window
  const TRACK_TTL_MS = 1500;         // finalize a track's dwell after this gap

  // ---- State -------------------------------------------------------------
  const timeline = [];               // {t, people, gaze}
  const tracksById = new Map();      // id -> {dwell, lastSeen, bbox, deg, dirConf, zones}
  const completedDwell = [];         // finalized dwell durations
  const funnel = { detected: 0, roi: 0, looked: 0, engaged: 0 }; // EMA-smoothed
  let heat = null, heatW = 48, heatH = 27;
  let peak = 0, latest = null, frameW = 1280, frameH = 720, simMode = false;

  const el = (id) => document.getElementById(id);
  const tooltip = el("tooltip");

  // ---- Connectivity ------------------------------------------------------
  async function start() {
    let cfg = { wsPort: 9001, wsPath: "/", topic: "bs/argus/analytics" };
    try { cfg = Object.assign(cfg, await (await fetch("config.json", { cache: "no-store" })).json()); } catch (_) {}
    const forceSim = new URLSearchParams(location.search).has("sim");
    if (forceSim || typeof mqtt === "undefined") return startSim();

    const url = `ws://${location.hostname}:${cfg.wsPort}${cfg.wsPath}`;
    setFeed("connecting", "connecting…");
    let connected = false;
    try {
      const client = mqtt.connect(url, { reconnectPeriod: 4000, connectTimeout: 4000 });
      const simTimer = setTimeout(() => { if (!connected) { client.end(true); startSim(); } }, 4500);
      client.on("connect", () => { connected = true; clearTimeout(simTimer); setFeed("live", "live"); client.subscribe(cfg.topic); });
      client.on("message", (_t, payload) => { try { handleMessage(JSON.parse(payload.toString())); } catch (_) {} });
      client.on("error", () => {});
      client.on("close", () => { if (connected) setFeed("down", "reconnecting…"); });
    } catch (_) { startSim(); }
  }

  function setFeed(cls, label) {
    el("feedDot").className = "feed-dot " + cls;
    el("feedLabel").textContent = label;
  }

  // ---- Message ingestion -------------------------------------------------
  function handleMessage(msg) {
    if (!msg || typeof msg.people !== "number") return;
    latest = msg;
    frameW = msg.frame_w || frameW; frameH = msg.frame_h || frameH;
    if (msg.device) el("devLabel").textContent = msg.device;

    const now = performance.now();
    timeline.push({ t: now, people: msg.people | 0, gaze: msg.gaze | 0 });
    while (timeline.length && now - timeline[0].t > WINDOW_MS) timeline.shift();
    peak = Math.max(peak, msg.people | 0);

    const tracks = Array.isArray(msg.tracks) ? msg.tracks : [];
    let roi = 0, engaged = 0;
    for (const tr of tracks) {
      const prev = tracksById.get(tr.id);
      tracksById.set(tr.id, { dwell: tr.dwell || 0, lastSeen: now, bbox: tr.bbox, deg: tr.deg || 0, dirConf: tr.dir_conf || 0, zones: tr.zones || [] });
      if ((tr.zones || []).includes("roi")) roi++;
      if ((tr.dwell || 0) >= ENGAGED_DWELL) engaged++;
      if (prev && tr.bbox) accumulateHeat(tr.bbox);
    }
    // Finalize disappeared tracks into the dwell histogram.
    for (const [id, t] of tracksById) {
      if (now - t.lastSeen > TRACK_TTL_MS) {
        if (t.dwell > 0.3) { completedDwell.push(t.dwell); if (completedDwell.length > 2000) completedDwell.shift(); }
        tracksById.delete(id);
      }
    }
    // Smooth the funnel so bars glide rather than jump.
    const a = 0.25;
    funnel.detected += a * ((msg.people | 0) - funnel.detected);
    funnel.roi += a * (roi - funnel.roi);
    funnel.looked += a * ((msg.gaze | 0) - funnel.looked);
    funnel.engaged += a * (engaged - funnel.engaged);
  }

  function accumulateHeat(b) {
    if (!heat) heat = new Float32Array(heatW * heatH);
    const fx = (b[0] + b[2]) / 2 / frameW;     // foot point: bottom-center
    const fy = b[3] / frameH;
    const gx = Math.max(0, Math.min(heatW - 1, Math.floor(fx * heatW)));
    const gy = Math.max(0, Math.min(heatH - 1, Math.floor(fy * heatH)));
    heat[gy * heatW + gx] += 1;
  }

  // ---- Simulator ---------------------------------------------------------
  function startSim() {
    simMode = true;
    setFeed("sim", "simulated");
    el("devLabel").textContent = "demo";
    const N = 6;
    const sims = Array.from({ length: N }, (_, i) => ({
      id: 100 + i, x: Math.random(), y: 0.4 + Math.random() * 0.4,
      vx: (Math.random() - 0.5) * 0.015, dwell: Math.random() * 10,
      looking: Math.random() < 0.4, w: 0.07 + Math.random() * 0.04,
    }));
    setInterval(() => {
      const active = sims.filter(() => Math.random() > 0.08);
      const tracks = active.map((s) => {
        s.x += s.vx; if (s.x < 0.05 || s.x > 0.95) s.vx *= -1;
        s.dwell += 0.1 + Math.random() * 0.05;
        if (Math.random() < 0.03) s.looking = !s.looking;
        const x1 = s.x * frameW, y1 = s.y * frameH, x2 = (s.x + s.w) * frameW, y2 = (s.y + 0.42) * frameH;
        return { id: s.id, state: "Confirmed", bbox: [x1, y1, x2, y2], dwell: +s.dwell.toFixed(1),
                 zones: s.x > 0.12 && s.x < 0.88 ? ["roi"] : [], deg: s.vx > 0 ? 90 : 270, dir_conf: 0.6, speed: Math.abs(s.vx) * 40 };
      });
      const gaze = active.filter((s) => s.looking).length;
      handleMessage({ schema: "analytics/v7.0", device: "demo", frame_w: frameW, frame_h: frameH,
        people: active.length, people_confident: active.length, gaze, fps: 29, npu_load: 70 + Math.random() * 20, tracks });
    }, 100);
  }

  // ---- Rendering ---------------------------------------------------------
  function fit(c) {
    const dpr = window.devicePixelRatio || 1;
    const w = c.clientWidth, h = c.clientHeight;
    if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
      c.width = Math.round(w * dpr); c.height = Math.round(h * dpr);
    }
    const ctx = c.getContext("2d"); ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    return { ctx, w, h };
  }

  function render() {
    drawKPIs();
    drawOverlay();
    drawHistory("timelinePresent", "people", C.present, C.presentGlow, "Present");
    drawHistory("timelineGazing", "gaze", C.gazing, C.gazingGlow, "Gazing");
    drawFunnel();
    drawDwell();
    drawHeatmap();
    if (heat) for (let i = 0; i < heat.length; i++) heat[i] *= 0.992; // slow decay
    requestAnimationFrame(render);
  }

  function drawKPIs() {
    const m = latest || {};
    const people = m.people | 0, gaze = m.gaze | 0;
    el("kPeople").textContent = people;
    el("kGaze").textContent = gaze;
    el("kRate").textContent = people ? Math.round((gaze / people) * 100) + "%" : "0%";
    const confirmed = [...tracksById.values()];
    const avg = confirmed.length ? confirmed.reduce((s, t) => s + t.dwell, 0) / confirmed.length : 0;
    el("kDwell").textContent = avg.toFixed(0) + "s";
    el("kPeak").textContent = peak;
    el("kNpu").textContent = Math.round(m.npu_load || 0) + "%";
    el("fpsHint").textContent = (m.fps || 0) + " fps";
  }

  // Single-series history chart (area + line) over the rolling window. Used for
  // both "Present History" (key=people) and "Gazing History" (key=gaze).
  function drawHistory(canvasId, key, color, glow, label) {
    const c = el(canvasId); const { ctx, w, h } = fit(c);
    ctx.clearRect(0, 0, w, h);
    const padL = 28, padB = 16, padT = 8, plotW = w - padL - 6, plotH = h - padB - padT;
    const max = Math.max(4, ...timeline.map((d) => d[key]));
    ctx.font = "11px system-ui"; ctx.textBaseline = "middle";
    for (let i = 0; i <= 4; i++) {
      const y = padT + (plotH * i) / 4, v = Math.round(max * (1 - i / 4));
      ctx.strokeStyle = C.grid; ctx.beginPath(); ctx.moveTo(padL, y); ctx.lineTo(w - 6, y); ctx.stroke();
      ctx.fillStyle = C.muted; ctx.textAlign = "right"; ctx.fillText(v, padL - 6, y);
    }
    if (timeline.length < 2) { c._hit = null; return; }
    const now = performance.now();
    // Span the actual data range (oldest sample -> now), capped at the window, so
    // the series fills the width from the start instead of bunching at the right;
    // once WINDOW_MS of data exists it becomes a true rolling window.
    const span = Math.max(1000, Math.min(WINDOW_MS, now - timeline[0].t));
    const X = (t) => padL + (1 - (now - t) / span) * plotW;
    const Y = (v) => padT + plotH - (v / max) * plotH;
    area(ctx, timeline, X, Y, (d) => d[key], color, 0.28, padT + plotH);
    line(ctx, timeline, X, Y, (d) => d[key], glow, 2);
    const last = timeline[timeline.length - 1];
    ctx.textAlign = "left"; ctx.fillStyle = glow; ctx.fillText(last[key], w - 22, Y(last[key]));
    c._hit = { X, key, label };
  }

  function area(ctx, data, X, Y, f, color, alpha, base) {
    ctx.beginPath(); ctx.moveTo(X(data[0].t), base);
    data.forEach((d) => ctx.lineTo(X(d.t), Y(f(d))));
    ctx.lineTo(X(data[data.length - 1].t), base); ctx.closePath();
    ctx.fillStyle = hexA(color, alpha); ctx.fill();
  }
  function line(ctx, data, X, Y, f, color, lw) {
    ctx.beginPath(); data.forEach((d, i) => { const x = X(d.t), y = Y(f(d)); i ? ctx.lineTo(x, y) : ctx.moveTo(x, y); });
    ctx.strokeStyle = color; ctx.lineWidth = lw; ctx.lineJoin = "round"; ctx.stroke();
  }

  function drawFunnel() {
    const c = el("funnel"); const { ctx, w, h } = fit(c);
    ctx.clearRect(0, 0, w, h);
    const stages = [
      { k: "Detected", v: funnel.detected }, { k: "In ROI", v: funnel.roi },
      { k: "Looked", v: funnel.looked }, { k: "Engaged >3s", v: funnel.engaged },
    ];
    const max = Math.max(1, stages[0].v, ...stages.map((s) => s.v));
    const padL = 92, barH = Math.min(30, (h - 12) / stages.length - 8), gap = ((h - 12) - barH * stages.length) / (stages.length - 1 || 1);
    // sequential cyan ramp, light->dark down the funnel
    const ramp = ["#5cc2ea", "#3aa6d6", "#2a8fbf", "#1f6f96"];
    ctx.font = "12px system-ui"; ctx.textBaseline = "middle";
    const hits = [];
    stages.forEach((s, i) => {
      const y = 6 + i * (barH + gap), bw = ((w - padL - 56) * s.v) / max;
      ctx.fillStyle = C.ink2; ctx.textAlign = "right"; ctx.fillText(s.k, padL - 10, y + barH / 2);
      ctx.fillStyle = C.grid; roundRect(ctx, padL, y, w - padL - 56, barH, 5); ctx.fill();
      ctx.fillStyle = ramp[i]; roundRect(ctx, padL, y, Math.max(3, bw), barH, 5); ctx.fill();
      ctx.fillStyle = "#e8eef7"; ctx.textAlign = "left"; ctx.fillText(Math.round(s.v), padL + bw + 8, y + barH / 2);
      const conv = i === 0 || stages[i - 1].v < 0.5 ? "" : Math.round((s.v / stages[i - 1].v) * 100) + "%";
      if (conv) { ctx.fillStyle = C.muted; ctx.textAlign = "right"; ctx.fillText(conv, w - 6, y + barH / 2); }
      hits.push({ y, h: barH, s });
    });
    c._hit = { padL, w, hits };
  }

  function drawDwell() {
    const c = el("dwell"); const { ctx, w, h } = fit(c);
    ctx.clearRect(0, 0, w, h);
    const edges = [0, 2, 5, 10, 20, 40, Infinity], labels = ["0-2", "2-5", "5-10", "10-20", "20-40", "40+"];
    const bins = new Array(labels.length).fill(0);
    for (const d of completedDwell) for (let i = 0; i < edges.length - 1; i++) if (d >= edges[i] && d < edges[i + 1]) { bins[i]++; break; }
    el("dwellN").textContent = completedDwell.length + " visits";
    const max = Math.max(1, ...bins);
    const padB = 18, padT = 6, plotH = h - padB - padT, n = bins.length, bw = (w - 8) / n;
    ctx.font = "11px system-ui";
    const hits = [];
    bins.forEach((v, i) => {
      const x = 4 + i * bw, bh = (v / max) * plotH, y = padT + plotH - bh;
      ctx.fillStyle = C.present; roundRect(ctx, x + 4, y, bw - 8, bh, 4); ctx.fill();
      ctx.fillStyle = C.muted; ctx.textAlign = "center"; ctx.textBaseline = "top";
      ctx.fillText(labels[i] + "s", x + bw / 2, padT + plotH + 4);
      if (v) { ctx.fillStyle = C.ink2; ctx.textBaseline = "bottom"; ctx.fillText(v, x + bw / 2, y - 2); }
      hits.push({ x: x + 4, w: bw - 8, label: labels[i], v });
    });
    c._hit = { hits };
  }

  function drawHeatmap() {
    const c = el("heatmap"); const { ctx, w, h } = fit(c);
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = "#0a0e16"; ctx.fillRect(0, 0, w, h);
    if (!heat) return;
    const max = Math.max(1, ...heat);
    const cw = w / heatW, ch = h / heatH;
    for (let y = 0; y < heatH; y++) for (let x = 0; x < heatW; x++) {
      const v = heat[y * heatW + x] / max; if (v < 0.02) continue;
      ctx.fillStyle = heatColor(v); ctx.globalAlpha = Math.min(1, 0.15 + v);
      ctx.fillRect(x * cw, y * ch, cw + 0.5, ch + 0.5);
    }
    ctx.globalAlpha = 1;
  }

  // heat ramp: dark -> cyan -> amber -> white (perceptual magnitude)
  function heatColor(v) {
    const stops = [[11,15,23],[42,143,191],[192,127,30],[245,240,220]];
    const p = Math.min(0.999, Math.max(0, v)) * (stops.length - 1);
    const i = Math.floor(p), f = p - i, a = stops[i], b = stops[i + 1] || a;
    return `rgb(${Math.round(a[0]+(b[0]-a[0])*f)},${Math.round(a[1]+(b[1]-a[1])*f)},${Math.round(a[2]+(b[2]-a[2])*f)})`;
  }

  function drawOverlay() {
    const c = el("overlay"); const { ctx, w, h } = fit(c);
    ctx.clearRect(0, 0, w, h);
    // LIVE: the /video frame already has attention_demo's detection boxes baked in
    // at full frame rate. Drawing our own boxes here (fed by 1 Hz MQTT) just makes a
    // second, lagging box -- so the overlay only draws in SIMULATOR mode, where the
    // video is blank and the sim boxes are the only visualization.
    if (!simMode) return;
    const scale = Math.min(w / frameW, h / frameH);
    const ox = (w - frameW * scale) / 2, oy = (h - frameH * scale) / 2;
    for (const [, t] of tracksById) {
      if (!t.bbox) continue;
      const [x1, y1, x2, y2] = t.bbox;
      const rx = ox + x1 * scale, ry = oy + y1 * scale, rw = (x2 - x1) * scale, rh = (y2 - y1) * scale;
      ctx.setLineDash([6, 5]); ctx.strokeStyle = "rgba(159,176,198,0.55)"; ctx.lineWidth = 1.5;
      ctx.strokeRect(rx, ry, rw, rh); ctx.setLineDash([]);
    }
    {
      const bw = 320, bh = 26, bx = (w - bw) / 2;
      ctx.fillStyle = "rgba(192,127,30,0.92)"; roundRect(ctx, bx, 8, bw, bh, 6); ctx.fill();
      ctx.fillStyle = "#0b0f17"; ctx.font = "600 13px system-ui";
      ctx.textAlign = "center"; ctx.textBaseline = "middle";
      ctx.fillText("SIMULATED DATA — no live feed", w / 2, 8 + bh / 2);
      ctx.textAlign = "left";
    }
  }

  // ---- helpers -----------------------------------------------------------
  function roundRect(ctx, x, y, w, h, r) { r = Math.min(r, h / 2, w / 2); ctx.beginPath(); ctx.moveTo(x + r, y); ctx.arcTo(x + w, y, x + w, y + h, r); ctx.arcTo(x + w, y + h, x, y + h, r); ctx.arcTo(x, y + h, x, y, r); ctx.arcTo(x, y, x + w, y, r); ctx.closePath(); }
  function hexA(hex, a) { const n = parseInt(hex.slice(1), 16); return `rgba(${(n>>16)&255},${(n>>8)&255},${n&255},${a})`; }

  // ---- tooltips ----------------------------------------------------------
  function showTip(x, y, html) { tooltip.innerHTML = html; tooltip.style.left = x + "px"; tooltip.style.top = y + "px"; tooltip.hidden = false; }
  function hideTip() { tooltip.hidden = true; }

  function historyHover(canvasId) {
    el(canvasId).addEventListener("mousemove", (e) => {
      const c = el(canvasId), hit = c._hit; if (!hit || timeline.length < 2) return hideTip();
      const r = c.getBoundingClientRect(), mx = e.clientX - r.left;
      let best = timeline[0], bd = Infinity;
      for (const d of timeline) { const dx = Math.abs(hit.X(d.t) - mx); if (dx < bd) { bd = dx; best = d; } }
      showTip(e.clientX, e.clientY, `${hit.label}: <b>${best[hit.key]}</b>`);
    });
    el(canvasId).addEventListener("mouseleave", hideTip);
  }
  historyHover("timelinePresent");
  historyHover("timelineGazing");

  el("funnel").addEventListener("mousemove", (e) => {
    const c = el("funnel"), hit = c._hit; if (!hit) return hideTip();
    const r = c.getBoundingClientRect(), my = e.clientY - r.top;
    const b = hit.hits.find((b) => my >= b.y && my <= b.y + b.h);
    b ? showTip(e.clientX, e.clientY, `${b.s.k}: <b>${Math.round(b.s.v)}</b>`) : hideTip();
  });
  el("funnel").addEventListener("mouseleave", hideTip);

  el("dwell").addEventListener("mousemove", (e) => {
    const c = el("dwell"), hit = c._hit; if (!hit) return hideTip();
    const r = c.getBoundingClientRect(), mx = e.clientX - r.left;
    const b = hit.hits.find((b) => mx >= b.x && mx <= b.x + b.w);
    b ? showTip(e.clientX, e.clientY, `${b.label}s: <b>${b.v}</b> visits`) : hideTip();
  });
  el("dwell").addEventListener("mouseleave", hideTip);

  start();
  requestAnimationFrame(render);
})();
