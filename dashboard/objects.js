/* Argus dashboard, D4 Object Search: manual region-of-interest selection on a stored frame, "save object to index",
   and object search (Visual Search page, "Object search" mode). Vanilla JS; relies on window.VMUI from vm.js.
   Contract: docs/VISUAL_MEMORY_DASHBOARD_API.md (object endpoints). All API strings are rendered with textContent.
   The coordinate helpers at the top are pure functions (unit-tested with node: tests/js/test_roi.js). */
(() => {
  "use strict";

  // ---- pure geometry: display (CSS px inside the stage) <-> original stored-image pixels ----------------------
  // The image is drawn with object-fit:contain inside a stage of boxW x boxH, so a frame whose aspect ratio differs
  // from the stage is letterboxed; all conversions go through fitRect().
  function fitRect(boxW, boxH, natW, natH) {
    if (!(boxW > 0 && boxH > 0 && natW > 0 && natH > 0)) return null;
    const s = Math.min(boxW / natW, boxH / natH), w = natW * s, h = natH * s;
    return { x: (boxW - w) / 2, y: (boxH - h) / 2, w, h, scale: s };
  }
  const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  // Point in stage coordinates -> original-image pixel coordinates (clamped into the image).
  function displayToImage(px, py, fit, nat) {
    return { x: clamp((px - fit.x) / fit.scale, 0, nat.w), y: clamp((py - fit.y) / fit.scale, 0, nat.h) };
  }
  // Drag from p0 to p1 (stage coordinates) -> integer ROI in image pixels (outward rounding, clamped).
  function roiFromDrag(p0, p1, fit, nat) {
    const a = displayToImage(p0.x, p0.y, fit, nat), b = displayToImage(p1.x, p1.y, fit, nat);
    const x0 = Math.floor(Math.min(a.x, b.x)), y0 = Math.floor(Math.min(a.y, b.y));
    const x1 = Math.ceil(Math.max(a.x, b.x)), y1 = Math.ceil(Math.max(a.y, b.y));
    return { x: x0, y: y0, w: Math.min(x1, nat.w) - x0, h: Math.min(y1, nat.h) - y0 };
  }
  function roiToDisplay(r, fit) { return { x: fit.x + r.x * fit.scale, y: fit.y + r.y * fit.scale, w: r.w * fit.scale, h: r.h * fit.scale }; }
  // "" when valid, else the message shown to the user (same rules as the server's validate_roi()).
  function validateRoi(r, nat, minPx) {
    if (!r || ![r.x, r.y, r.w, r.h].every((v) => Number.isInteger(v))) return "Selection must use whole pixel values.";
    if (r.w < 1 || r.h < 1 || r.x < 0 || r.y < 0) return "Draw a rectangle on the image.";
    if (r.x + r.w > nat.w || r.y + r.h > nat.h) return "The selection leaves the image.";
    if (r.w < minPx || r.h < minPx) return `Selection too small (minimum ${minPx} × ${minPx} px).`;
    return "";
  }
  const geom = { fitRect, displayToImage, roiFromDrag, roiToDisplay, validateRoi };
  if (typeof module !== "undefined" && module.exports) { module.exports = geom; return; }
  if (!window.VMUI) return;

  // ---- UI -----------------------------------------------------------------------------------------------------
  const U = window.VMUI, h = U.h, API = U.API, isNum = U.isNum, DASH = U.DASH, fmtInt = U.fmtInt;
  const MIN_PX = 16;
  const NOTE = "Object search compares a manually selected crop with other manually indexed crops, using the same MobileNetV2 image embedding as Visual Search. " +
    "Similarity scores are not calibrated identification probabilities and this is not object detection or recognition.";
  const objState = () => (U.S.status && U.S.status.objects) || null;
  const objEnabled = () => { const o = objState(); return !!(o && o.enabled); };
  const bboxTxt = (b) => `${b.x}, ${b.y} · ${b.w} × ${b.h}`;
  const cropUrl = (fid, r, max) => `${API}/frames/${fid}/crop?x=${r.x}&y=${r.y}&w=${r.w}&h=${r.h}&max=${max || 240}`;
  const objImgUrl = (rid, max) => `${API}/objects/${rid}/image?max=${max || 240}`;

  function errText(status, data) {
    const code = (data && data.status && typeof data.status === "object" && data.status.code) || (data && data.error) || "";
    const msg = (data && data.status && data.status.message) || (data && data.message) || "";
    const M = {
      invalid_roi: "The selected rectangle is not valid for this frame.",
      not_found: "Frame or object not found (it may have been deleted).",
      image_unavailable: "The source frame's JPEG has expired, so a crop cannot be extracted.",
      busy: "The model is busy with another search or indexing request; try again in a moment.",
      not_enabled: "Object search is not enabled in the active configuration (visual_memory.objects.enabled).",
      provider_failed: "The embedding model could not run on the device.",
      model_mismatch: "The indexed objects were embedded with a different model or preprocessing, so they cannot be compared.",
      timeout: "The search timed out on the server.",
    };
    return (M[code] || U.errorMessage(status, data)) + (msg && !(M[code] || "").includes(msg) ? " " + msg : "");
  }

  // Image of an object crop with expired/missing fallbacks.
  function objThumb(rid, max, big) { return U.loadImg(objImgUrl(rid, max), "object " + rid, "thumb", big); }

  // Source frame with the bounding box drawn as an overlay (percentages of the stored image size: exact at any scale).
  function frameWithBox(frameId, bbox, iw, ih, avail) {
    const wrap = h("div", { class: "vm-roi-frame", style: `aspect-ratio:${iw || 16} / ${ih || 9}` });
    wrap.append(U.frameImage({ id: frameId, image_available: avail && avail.image_available, image_exists: avail && avail.image_exists }, "image", 0, true));
    if (iw && ih && bbox) wrap.append(h("div", { class: "vm-roi-box", "aria-label": "Bounding box " + bboxTxt(bbox),
      style: `left:${bbox.x / iw * 100}%;top:${bbox.y / ih * 100}%;width:${bbox.w / iw * 100}%;height:${bbox.h / ih * 100}%` }));
    return wrap;
  }

  // ---- selector modal ------------------------------------------------------------------------------------------
  function openSelector(frame, parentModal) {
    const fid = frame.id != null ? frame.id : frame.frame_id;
    const nat = { w: frame.width || 0, h: frame.height || 0 };
    if (!nat.w || !nat.h) { U.openModal("Select object", h("div", { class: "vm-banner bad", text: "Frame size unknown." })); return; }
    if (frame.image_available === false || frame.image_exists === false) {
      U.openModal("Select object", h("div", { class: "vm-banner warn", text: "This frame's JPEG is no longer available, so an object cannot be selected." })); return;
    }
    if (parentModal) parentModal.close();
    let roi = null, dragging = null, prevTimer = null;
    const stage = h("div", { class: "vm-roi-stage", tabindex: "0", role: "application",
      "aria-label": "Draw a rectangle around an object. Use the numeric fields below for keyboard input." });
    const img = h("img", { src: `${API}/frames/${fid}/image`, alt: "Frame " + fid, draggable: "false" });
    const rect = h("div", { class: "vm-roi-box live", hidden: true });
    stage.append(img, rect);
    const num = (k) => h("input", { type: "number", min: "0", step: "1", "aria-label": k.toUpperCase(), style: "width:84px" });
    const nx = num("x"), ny = num("y"), nw = num("w"), nh = num("h");
    const label = h("input", { type: "text", maxlength: "64", placeholder: "optional, e.g. coffee cup", "aria-label": "Object label", style: "width:230px" });
    const msg = h("div", { class: "muted", "aria-live": "polite", style: "min-height:20px" });
    const prev = h("div", { class: "vm-roi-prev" }, h("span", { class: "muted", text: "Crop preview appears here" }));
    const find = h("button", { class: "vm-btn primary", type: "button", text: "Find Similar Objects", disabled: true });
    const save = h("button", { class: "vm-btn", type: "button", text: "Save Object to Index", disabled: true });
    const clearBtn = h("button", { class: "vm-btn small", type: "button", text: "Clear" });

    const fit = () => { const r = stage.getBoundingClientRect(); return fitRect(r.width, r.height, nat.w, nat.h); };
    function paint() {
      const f = fit();
      if (!roi || !f || roi.w < 1 || roi.h < 1) { rect.hidden = true; return; }
      const d = roiToDisplay(roi, f);
      rect.hidden = false; rect.style.left = d.x + "px"; rect.style.top = d.y + "px"; rect.style.width = d.w + "px"; rect.style.height = d.h + "px";
    }
    function sync(fromFields) {
      const bad = roi ? validateRoi(roi, nat, MIN_PX) : "Draw a rectangle around an object.";
      if (!fromFields && roi) { nx.value = roi.x; ny.value = roi.y; nw.value = roi.w; nh.value = roi.h; }
      msg.textContent = roi && !bad ? `Selection ${bboxTxt(roi)} (original ${nat.w} × ${nat.h} px)` : bad;
      msg.className = bad ? "muted" : "";
      find.disabled = save.disabled = !!bad;
      paint();
      clearTimeout(prevTimer);
      if (!bad) prevTimer = setTimeout(() => { prev.replaceChildren(h("img", { src: cropUrl(fid, roi, 240), alt: "Selected crop preview" })); }, 120);
      else prev.replaceChildren(h("span", { class: "muted", text: "Crop preview appears here" }));
    }
    const pt = (e) => { const r = stage.getBoundingClientRect(); return { x: e.clientX - r.left, y: e.clientY - r.top }; };
    stage.addEventListener("pointerdown", (e) => {
      if (e.button != null && e.button !== 0) return;
      e.preventDefault(); stage.focus({ preventScroll: true });
      try { stage.setPointerCapture(e.pointerId); } catch (_) {}
      dragging = { id: e.pointerId, p0: pt(e) };
    });
    stage.addEventListener("pointermove", (e) => {
      if (!dragging || e.pointerId !== dragging.id) return;
      const f = fit(); if (!f) return;
      roi = roiFromDrag(dragging.p0, pt(e), f, nat); sync(false);
    });
    const end = (e) => { if (dragging && e.pointerId === dragging.id) { dragging = null; sync(false); } };
    stage.addEventListener("pointerup", end); stage.addEventListener("pointercancel", end);
    [nx, ny, nw, nh].forEach((el) => el.addEventListener("input", () => {
      roi = { x: Number(nx.value), y: Number(ny.value), w: Number(nw.value), h: Number(nh.value) };
      if (![nx, ny, nw, nh].every((q) => q.value !== "")) roi = null;
      sync(true);
    }));
    clearBtn.addEventListener("click", () => { roi = null; nx.value = ny.value = nw.value = nh.value = ""; sync(true); });
    window.addEventListener("resize", paint);

    find.addEventListener("click", () => { m.close(); searchFromRoi({ id: fid, width: nat.w, height: nat.h, capture_utc_ms: frame.capture_utc_ms, source_id: frame.source_id }, Object.assign({}, roi)); });
    save.addEventListener("click", async () => {
      save.disabled = true; msg.textContent = "Extracting crop and computing embedding…";
      const r = await U.api("/objects", { method: "POST", credentials: "same-origin", headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ frame_id: fid, x: roi.x, y: roi.y, w: roi.w, h: roi.h, label: label.value.trim() }) });
      save.disabled = false;
      if (r.status === 200 || r.status === 201) {
        const reg = r.data.region;
        msg.className = ""; msg.textContent = (r.data.status === "duplicate" ? "Already indexed as object #" : "Saved as object #") + reg.region_id + " (" + (isNum(r.data.elapsed_ms) ? r.data.elapsed_ms.toFixed(0) + " ms" : DASH) + "). The original frame is unchanged.";
        U.pollStatus();
      } else { msg.className = ""; msg.textContent = errText(r.status, r.data); }
    });

    const body = h("div", { class: "vm-roi-layout" },
      h("div", null, stage,
        h("p", { class: "muted", style: "margin:6px 0 0", text: "Drag on the image (mouse or touch) to draw a rectangle around one object. The frame is shown scaled; coordinates below are in the stored image's own pixels." })),
      h("div", { class: "vm-stack" },
        h("div", null, h("h3", { text: "Selected crop" }), prev),
        h("div", { class: "vm-row" }, h("label", null, "X", nx), h("label", null, "Y", ny), h("label", null, "Width", nw), h("label", null, "Height", nh), clearBtn),
        h("label", null, "Label (optional)", label),
        msg,
        h("div", { class: "vm-row" }, find, save),
        h("p", { class: "muted", style: "font-size:12px", text: NOTE })));
    const m = U.openModal(`Select object — frame #${fid}`, body, { wide: true, onClose: () => window.removeEventListener("resize", paint) });
    img.addEventListener("load", () => { if (img.naturalWidth && (img.naturalWidth !== nat.w || img.naturalHeight !== nat.h)) { nat.w = img.naturalWidth; nat.h = img.naturalHeight; } paint(); });
    sync(false);
  }

  // ---- object search page -------------------------------------------------------------------------------------
  const Q = { query: null /* {type:"roi",frame,roi} | {type:"region",region} */, ctrl: null };
  const E = {};
  let mounted = false;

  function searchFromRoi(frame, roi) {
    Q.query = { type: "roi", frame, roi };
    U.showSearchMode("objects");
    if (mounted) { renderQuery(); runSearch(); }
  }
  function searchFromRegion(region) {
    Q.query = { type: "region", region };
    U.showSearchMode("objects");
    if (mounted) { renderQuery(); runSearch(); }
  }

  function renderStatusBanner() {
    if (!mounted) return;
    const o = objState(), box = U.clear(E.banner);
    if (!o || !o.enabled) { box.append(h("div", { class: "vm-banner warn" }, h("b", { text: "Object search is disabled. " }), "Set visual_memory.objects.enabled in the Argus configuration (see docs/VISUAL_MEMORY_OBJECT_SEARCH.md).")); }
    else if (o.available === false) box.append(h("div", { class: "vm-banner bad", text: "Object index unavailable: " + (o.reason || "unknown reason") }));
    else if (o.regions === 0) box.append(h("div", { class: "vm-banner" }, h("b", { text: "The object index is empty. " }), "Open a stored frame in Visual Memory, choose “Select Object”, draw a box around an object and choose “Save Object to Index”."));
    else box.append(h("p", { class: "muted", text: `${fmtInt(o.regions)} indexed object${o.regions === 1 ? "" : "s"} · ${U.fmtBytes(o.vector_bytes)} of vectors · limit ${fmtInt(o.max_regions)}` }));
    E.run.disabled = !(o && o.enabled && o.available !== false && Q.query && !Q.ctrl);
    E.pickFrame.disabled = !(o && o.enabled && o.available !== false);
  }

  function renderQuery() {
    const box = U.clear(E.query), q = Q.query;
    if (!q) { box.append(h("p", { class: "muted", text: "No query yet. Select an object in a stored frame, or pick an object that is already indexed." })); renderStatusBanner(); return; }
    if (q.type === "roi") {
      box.append(h("div", { class: "vm-row" }, h("div", { class: "vm-roi-prev small" }, h("img", { src: cropUrl(q.frame.id, q.roi, 200), alt: "Query crop" })),
        h("dl", { class: "vm-dl" }, h("dt", { text: "Query" }), h("dd", { text: "Crop of frame #" + q.frame.id }), h("dt", { text: "Region" }), h("dd", { text: bboxTxt(q.roi) }))));
    } else {
      const r = q.region;
      box.append(h("div", { class: "vm-row" }, h("div", { class: "vm-roi-prev small" }, objThumb(r.region_id, 200)),
        h("dl", { class: "vm-dl" }, h("dt", { text: "Query" }), h("dd", { text: "Indexed object #" + r.region_id + (r.label ? " “" + r.label + "”" : "") }),
          h("dt", { text: "Region" }), h("dd", { text: `frame #${r.frame_id} · ${bboxTxt(r.bbox)}` }))));
    }
    renderStatusBanner();
  }

  async function runSearch() {
    if (!Q.query || Q.ctrl) return;
    const body = {};
    const k = Math.floor(Number(E.topK.value));
    if (!(k >= 1 && k <= 50)) { E.msg.textContent = "Top K must be between 1 and 50."; return; }
    body.top_k = k;
    const t = E.tf.get();
    if (t.start != null) body.start = t.start; if (t.end != null) body.end = t.end;
    if (t.start != null && t.end != null && t.end <= t.start) { E.msg.textContent = "“To” must be after “From”."; return; }
    if (E.src.value) body.source = E.src.value;
    if (E.label.value.trim()) body.label = E.label.value.trim();
    if (E.minOn.checked) { const ms = Number(E.minSim.value); if (!(ms >= 0 && ms <= 1)) { E.msg.textContent = "Minimum similarity must be between 0 and 1."; return; } body.min_similarity = ms; }
    if (E.reqImg.checked) body.require_image = true;
    if (Q.query.type === "roi") Object.assign(body, { frame_id: Q.query.frame.id }, Q.query.roi); else body.region_id = Q.query.region.region_id;
    Q.ctrl = new AbortController();
    E.cancel.hidden = false; E.spin.hidden = false; E.msg.textContent = "Searching…"; U.clear(E.results); renderStatusBanner();
    const r = await U.api("/objects/search", { method: "POST", credentials: "same-origin", cache: "no-store", signal: Q.ctrl.signal, headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
    Q.ctrl = null; E.cancel.hidden = true; E.spin.hidden = true; E.msg.textContent = ""; renderStatusBanner();
    if (r.aborted) { E.results.append(h("div", { class: "vm-banner", text: "Search cancelled." })); return; }
    const d = r.data;
    if (r.status !== 200 || !d || (d.status && d.status.code && d.status.code !== "ok")) {
      E.results.append(h("div", { class: "vm-banner bad" }, h("b", { text: "Search failed. " }), errText(r.status, d)));
      if (r.status === 401) { E.results.append(U.authBox()); U.pollStatus(); }
      return;
    }
    renderResults(d);
  }

  function renderResults(d) {
    const box = U.clear(E.results), res = d.results || [], st = d.status || {};
    box.append(h("div", { class: "vm-label", text: d.label || "" }));
    box.append(h("p", { class: "muted", style: "margin-top:10px", text: `${res.length} result${res.length === 1 ? "" : "s"} · ${isNum(st.elapsed_ms) ? st.elapsed_ms.toFixed(1) + " ms" : DASH} · ${fmtInt(st.candidates_scanned)} indexed objects scanned` }));
    if (!res.length) {
      box.append(h("div", { class: "vm-empty", text: d.status && d.status.empty_index ? "The object index is empty: save some objects first." : "No indexed objects match these options." })); return;
    }
    const grid = h("div", { class: "vm-grid" });
    res.forEach((x) => {
      const avail = x.image_available === false ? h("span", { class: "vm-badge warn", text: "frame JPEG expired" }) : x.image_exists === false ? h("span", { class: "vm-badge bad", text: "frame JPEG missing" }) : h("span", { class: "vm-badge ok", text: "frame JPEG available" });
      grid.append(h("button", { class: "vm-card", type: "button", onclick: () => openCompare(x, d), "aria-label": `Result ${x.rank}, object ${x.region_id}, similarity ${isNum(x.similarity) ? x.similarity.toFixed(4) : "unknown"}` },
        h("div", { class: "vm-thumb" }, h("span", { class: "vm-rank", text: "#" + x.rank }), objThumb(x.region_id, 240)),
        h("div", { class: "meta" },
          h("span", { class: "vm-score", text: isNum(x.similarity) ? x.similarity.toFixed(4) : DASH }),
          h("div", { class: "vm-bar", "aria-hidden": "true" }, isNum(x.similarity) ? h("i", { style: `width:${clamp(x.similarity, 0, 1) * 100}%` }) : null),
          x.label ? h("b", { text: x.label }) : null,
          U.tsEl(x.capture_utc_ms, "t"), h("span", { class: "src", title: x.source_id || "", text: x.source_id || DASH }),
          h("span", { class: "muted", text: `frame #${x.frame_id} · box ${x.bbox.w}×${x.bbox.h}` }), h("div", { class: "vm-badges" }, avail))));
    });
    box.append(grid);
  }

  function openCompare(x, d) {
    const both = (ms) => h("span", null, U.tsEl(ms), " ", h("span", { class: "muted", text: "(" + U.otherZone(ms) + ")" }));
    const q = Q.query;
    const qSide = h("div", null, h("h3", { text: "Query object" }),
      h("div", { class: "vm-roi-prev" }, q && q.type === "roi" ? h("img", { src: cropUrl(q.frame.id, q.roi, 400), alt: "Query crop" }) : q ? objThumb(q.region.region_id, 400, true) : null),
      q && q.type === "roi" ? h("p", { class: "muted", text: `Frame #${q.frame.id}, region ${bboxTxt(q.roi)}` }) : null);
    const mDl = h("dl", { class: "vm-dl" }, h("dt", { text: "Object" }), h("dd", { text: "#" + x.region_id + (x.label ? " “" + x.label + "”" : "") }),
      h("dt", { text: "Similarity" }), h("dd", { text: isNum(x.similarity) ? x.similarity.toFixed(4) + "  (cosine; not a calibrated probability)" : DASH }),
      h("dt", { text: "Captured" }), h("dd", null, both(x.capture_utc_ms)), h("dt", { text: "Source" }), h("dd", { text: x.source_id || DASH }),
      h("dt", { text: "Parent frame" }), h("dd", { text: "#" + x.frame_id }), h("dt", { text: "Bounding box" }), h("dd", { text: `x ${x.bbox.x}, y ${x.bbox.y}, ${x.bbox.w} × ${x.bbox.h} in ${x.image_width} × ${x.image_height}` }));
    const mSide = h("div", null, h("h3", { text: `Match #${x.rank}` }), h("div", { class: "vm-roi-prev" }, objThumb(x.region_id, 400, true)), mDl);
    const src = h("div", { style: "margin-top:14px" }, h("h3", { text: "Source frame with bounding box" }),
      frameWithBox(x.frame_id, x.bbox, x.image_width, x.image_height, x));
    const again = h("button", { class: "vm-btn small", type: "button", text: "Find similar to this object", onclick: () => { modal.close(); searchFromRegion(x); } });
    const detail = h("button", { class: "vm-btn small", type: "button", text: "Open frame detail", onclick: () => U.openDetail(x.frame_id) });
    const body = h("div", null, h("div", { class: "vm-label", style: "margin-bottom:12px", text: d.label || "" }),
      h("div", { class: "vm-cmp" }, qSide, mSide), src, h("div", { class: "vm-row", style: "margin-top:12px" }, again, detail));
    const modal = U.openModal("Compare query object and match", body, { wide: true });
    U.tzSubscribe(() => { if (body.isConnected) U.refreshTs(body); });
  }

  async function loadIndexed() {
    const box = U.clear(E.indexed);
    const r = await U.api("/objects?limit=24");
    if (r.status !== 200 || !r.data) { box.append(h("p", { class: "muted", text: r.status === 503 ? "" : "Could not list indexed objects." })); return; }
    const rows = r.data.objects || [];
    box.append(h("h3", { text: `Indexed objects (${fmtInt(r.data.total)})` }));
    if (!rows.length) { box.append(h("p", { class: "muted", text: "None yet." })); return; }
    const grid = h("div", { class: "vm-grid" });
    rows.forEach((o) => grid.append(h("button", { class: "vm-card", type: "button", title: "Search for objects similar to this one", onclick: () => searchFromRegion(o) },
      h("div", { class: "vm-thumb" }, objThumb(o.region_id, 200)),
      h("div", { class: "meta" }, h("b", { text: o.label || "(no label)" }), U.tsEl(o.capture_utc_ms, "t"), h("span", { class: "muted", text: `#${o.region_id} · frame #${o.frame_id}` })))));
    box.append(grid);
  }

  function pickFrame() {
    const g = U.makeGallery({ onPick: async (f) => {
      m.close();
      const r = await U.api("/frames/" + encodeURIComponent(f.id));
      if (r.status === 200 && r.data) openSelector({ id: r.data.id, width: r.data.width, height: r.data.height, capture_utc_ms: r.data.capture_utc_ms, source_id: r.data.source_id,
        image_available: r.data.image_available, image_exists: r.data.image_exists });
    } });
    const m = U.openModal("Pick a stored frame", h("div", null, h("p", { class: "muted", text: "Choose the frame that contains the object." }), g.el), { wide: true });
    g.load();
  }

  function mountSearch(slot) {
    if (mounted) return; mounted = true;
    E.banner = h("div"); E.query = h("div"); E.results = h("div", { "aria-live": "polite" }); E.indexed = h("div");
    E.topK = h("input", { type: "number", min: "1", max: "50", step: "1", value: "5", "aria-label": "Top K" });
    E.label = h("input", { type: "text", maxlength: "64", placeholder: "any", "aria-label": "Label filter", style: "width:140px" });
    E.tf = U.makeTimeFilter(() => {}); E.src = U.sourceSelect(() => {});
    E.minOn = h("input", { type: "checkbox" }); E.minSim = h("input", { type: "number", min: "0", max: "1", step: "0.01", value: "0.80", "aria-label": "Minimum similarity" });
    E.reqImg = h("input", { type: "checkbox" });
    E.pickFrame = h("button", { class: "vm-btn primary", type: "button", text: "Select object in a stored frame…", onclick: pickFrame });
    E.run = h("button", { class: "vm-btn primary", type: "button", text: "Find Similar Objects", disabled: true, onclick: runSearch });
    E.cancel = h("button", { class: "vm-btn", type: "button", text: "Cancel", hidden: true, onclick: () => { if (Q.ctrl) Q.ctrl.abort(); } });
    E.spin = h("span", { class: "vm-spin", hidden: true }); E.msg = h("span", { class: "muted" });
    slot.append(h("div", { class: "vm-stack" }, h("div", { class: "vm-note", text: NOTE }), E.banner,
      h("div", { class: "vm-search-grid" },
        h("div", { class: "panel" }, h("div", { class: "panel-head" }, h("h2", { text: "1. Query object" })), E.query, h("div", { class: "vm-row", style: "margin-top:10px" }, E.pickFrame)),
        h("div", { class: "panel" }, h("div", { class: "panel-head" }, h("h2", { text: "2. Options" })),
          h("div", { class: "vm-stack" },
            h("div", { class: "vm-row" }, h("label", null, "Top K (1–50)", E.topK), h("label", null, "Label", E.label), h("label", null, "Source", E.src)),
            E.tf.el,
            h("div", { class: "vm-row" }, h("label", { class: "inline" }, E.minOn, "Minimum similarity (off by default)"), E.minSim,
              h("label", { class: "inline" }, E.reqImg, "Require source JPEG")),
            h("div", { class: "vm-row" }, E.run, E.cancel, E.spin, E.msg)))),
      E.results, h("section", { class: "panel" }, E.indexed)));
    U.onStatus(() => { renderStatusBanner(); });
    renderQuery(); loadIndexed();
    U.tzSubscribe(() => U.refreshTs(slot));
  }

  window.VMObjects = { openSelector, mountSearch, searchFromRoi, searchFromRegion, enabled: objEnabled, reload: () => { if (mounted) loadIndexed(); }, _geom: geom };
  U.registerObjects(window.VMObjects);
})();
