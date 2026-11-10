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
  const NOTE = "Object search compares an object crop with the indexed object crops (manually selected, or proposed by the YOLOX detector when automatic indexing is on), using the same MobileNetV2 image embedding as Visual Search. " +
    "It searches only the objects that were indexed, not every stored frame. Similarity scores are not calibrated identification probabilities; detector classes and confidences can be wrong.";
  const pretty = (n) => String(n || "").replace(/_/g, " ");
  const fmtDur = (ms) => { if (!isNum(ms)) return DASH; const s = ms / 1000; return s < 90 ? s.toFixed(1) + " s" : Math.floor(s / 60) + " min " + Math.round(s % 60) + " s"; };
  const A = { data: null, timer: null, msg: "" };           // automatic-indexing status (GET /objects/auto/status)
  const objState = () => (U.S.status && U.S.status.objects) || null;
  const objEnabled = () => { const o = objState(); return !!(o && o.enabled); };
  const bboxTxt = (b) => `${b.x}, ${b.y} · ${b.w} × ${b.h}`;
  // A stored frame is identified by (source_type, kind, id): camera frame ids and Proof-of-Play ids come from different databases.
  const fref = (f) => ({ source_type: f.source_type === "display" ? "display" : "camera", kind: f.source_type === "display" ? (f.kind || f.source_kind || "history") : "", id: f.id != null ? f.id : f.frame_id });
  const frameImgUrl = (ref) => ref.source_type === "display" ? `${API}/display-frames/${ref.kind}/${ref.id}/image` : `${API}/frames/${ref.id}/image`;
  const cropUrl = (ref, r, max) => {
    const q = `x=${r.x}&y=${r.y}&w=${r.w}&h=${r.h}&max=${max || 240}`;
    return ref.source_type === "display" ? `${API}/display-frames/${ref.kind}/${ref.id}/crop?${q}` : `${API}/frames/${ref.id}/crop?${q}`;
  };
  const isDisp = (o) => !!o && o.source_type === "display";
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

  const originBadge = (x) => h("span", { class: "vm-badge " + (x.index_origin === "auto" ? "auto" : "manual"), "data-role": "origin-badge",
    title: x.index_origin === "auto" ? "Found by the YOLOX detector and indexed automatically" : "Selected and indexed by a person",
    text: x.index_origin === "auto" ? "auto-indexed" : "manual" });
  const cropBadge = (x) => x.crop_state === "retained" ? h("span", { class: "vm-badge ok", "data-role": "crop-state", text: x.crop_available === false ? "crop file missing" : "crop retained" })
    : x.crop_state === "expired" ? h("span", { class: "vm-badge warn", "data-role": "crop-state", text: "crop expired" })
    : h("span", { class: "vm-badge", "data-role": "crop-state", title: "Indexed before crop images were stored (schema v1)", text: "no crop stored" });
  const frameBadge = (x) => x.image_available === false ? h("span", { class: "vm-badge warn", "data-role": "frame-state", text: "frame JPEG expired" })
    : x.image_exists === false ? h("span", { class: "vm-badge bad", "data-role": "frame-state", text: "frame JPEG missing" })
    : h("span", { class: "vm-badge ok", "data-role": "frame-state", text: "frame JPEG available" });
  // Proof-of-Play facts of a display object: advertisement (expected vs what the player reported), position in the play, timestamp bound, session link.
  const adText = (x) => `ad ${x.expected_media_id || DASH}` + (x.reported_media_id && x.reported_media_id !== x.expected_media_id ? ` (player reported ${x.reported_media_id})` : x.reported_media_id ? " (player-reported: same)" : " (no player report)");
  const offText = (x) => `${isNum(x.playback_offset_ms) ? "+" + fmtDur(x.playback_offset_ms) + " into the play" : "offset unknown"}${isNum(x.time_uncertainty_ms) ? " · ±" + x.time_uncertainty_ms + " ms" : " · time unbounded"}`;
  const sessionBtn = (x, small) => h("a", { class: "vm-btn" + (small ? " small" : ""), "data-role": "session-link", href: x.session_link || "#pop", text: "Open Proof-of-Play session", title: "Session " + (x.pop_session_id || ""),
    onclick: (e) => { e.stopPropagation(); } });
  const classText = (x) => x.index_origin === "auto" ? `${pretty(x.class_name)} · ${isNum(x.confidence) ? (x.confidence * 100).toFixed(0) + "% detector confidence" : ""}` : "";

  // Source frame with the bounding box drawn as an overlay (percentages of the stored image size: exact at any scale).
  // `x` is a region / hit (camera or display); the parent frame is resolved in ITS OWN store.
  function frameWithBox(x) {
    const iw = x.image_width, ih = x.image_height, bbox = x.bbox;
    const wrap = h("div", { class: "vm-roi-frame", style: `aspect-ratio:${iw || 16} / ${ih || 9}` });
    if (isDisp(x)) {
      if (x.image_available === false || x.image_exists === false) wrap.append(h("div", { class: "vm-empty", "data-role": "frame-gone", text: x.image_available === false ? "The display frame JPEG expired (removed by retention); the object crop, box and timestamp are retained." : "The display frame JPEG is missing." }));
      else wrap.append(U.loadImg(frameImgUrl({ source_type: "display", kind: x.source_kind, id: x.frame_id }), "display frame " + x.frame_id, "image", true));
    } else wrap.append(U.frameImage({ id: x.frame_id, image_available: x.image_available, image_exists: x.image_exists }, "image", 0, true));
    if (iw && ih && bbox && !(isDisp(x) && (x.image_available === false || x.image_exists === false))) wrap.append(h("div", { class: "vm-roi-box", "aria-label": "Bounding box " + bboxTxt(bbox),
      style: `left:${bbox.x / iw * 100}%;top:${bbox.y / ih * 100}%;width:${bbox.w / iw * 100}%;height:${bbox.h / ih * 100}%` }));
    return wrap;
  }

  // ---- selector modal ------------------------------------------------------------------------------------------
  function openSelector(frame, parentModal) {
    const ref = fref(frame), fid = ref.id;
    const nat = { w: frame.width || 0, h: frame.height || 0 };
    if (!nat.w || !nat.h) { U.openModal("Select object", h("div", { class: "vm-banner bad", text: "Frame size unknown." })); return; }
    if (frame.image_available === false || frame.image_exists === false) {
      U.openModal("Select object", h("div", { class: "vm-banner warn", text: "This frame's JPEG is no longer available, so an object cannot be selected." })); return;
    }
    if (parentModal) parentModal.close();
    let roi = null, dragging = null, prevTimer = null;
    const stage = h("div", { class: "vm-roi-stage", tabindex: "0", role: "application",
      "aria-label": "Draw a rectangle around an object. Use the numeric fields below for keyboard input." });
    const img = h("img", { src: frameImgUrl(ref), alt: (ref.source_type === "display" ? "Display frame " : "Frame ") + fid, draggable: "false" });
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
      if (!bad) prevTimer = setTimeout(() => { prev.replaceChildren(h("img", { src: cropUrl(ref, roi, 240), alt: "Selected crop preview" })); }, 120);
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

    find.addEventListener("click", () => { m.close(); searchFromRoi({ id: fid, source_type: ref.source_type, kind: ref.kind, width: nat.w, height: nat.h, capture_utc_ms: frame.capture_utc_ms, source_id: frame.source_id }, Object.assign({}, roi)); });
    save.addEventListener("click", async () => {
      save.disabled = true; msg.textContent = "Extracting crop and computing embedding…";
      const r = await U.api("/objects", { method: "POST", credentials: "same-origin", headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ source_type: ref.source_type, kind: ref.kind, frame_id: fid, x: roi.x, y: roi.y, w: roi.w, h: roi.h, label: label.value.trim() }) });
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
    const m = U.openModal(ref.source_type === "display" ? `Select object — BrightSign display frame #${fid} (${ref.kind})` : `Select object — frame #${fid}`, body, { wide: true, onClose: () => window.removeEventListener("resize", paint) });
    img.addEventListener("load", () => { if (img.naturalWidth && (img.naturalWidth !== nat.w || img.naturalHeight !== nat.h)) { nat.w = img.naturalWidth; nat.h = img.naturalHeight; } paint(); });
    sync(false);
  }

  // ---- object search page -------------------------------------------------------------------------------------
  // Q.source: which indexed objects are searched and shown: "camera" (default) or "display" (BrightSign Weston/VOP2 frames of Proof of Play).
  // The two are never mixed.
  const Q = { query: null /* {type:"roi",frame,roi} | {type:"region",region} */, ctrl: null, source: "camera" };
  const E = {};
  let mounted = false;

  function searchFromRoi(frame, roi) {
    const src = frame.source_type === "display" ? "display" : "camera";
    if (src !== Q.source) U.showSearchSource(src);                       // the page source follows the frame the object was chosen in
    Q.source = src;
    Q.query = { type: "roi", frame, roi };
    U.showSearchMode("objects");
    if (mounted) { renderQuery(); runSearch(); }
  }
  function searchFromRegion(region) {
    const src = region.source_type === "display" ? "display" : "camera";
    if (src !== Q.source) U.showSearchSource(src);
    Q.source = src;
    Q.query = { type: "region", region };
    U.showSearchMode("objects");
    if (mounted) { renderQuery(); runSearch(); }
  }
  // Called by the Visual Search page when the Source switch changes while Object search is on.
  function setSource(src) {
    src = src === "display" ? "display" : "camera";
    if (src === Q.source && mounted && E.root && E.root.dataset.source === src) return;
    Q.source = src; Q.query = null;
    if (!mounted) return;
    if (Q.ctrl) Q.ctrl.abort();
    E.root.dataset.source = src;
    E.src.parentElement.hidden = src === "display";                       // the camera-device filter only applies to camera objects
    E.cls.replaceChildren(h("option", { value: "", text: "any class" })); E.origin.value = ""; E.cls.value = "";
    E.pickFrame.textContent = src === "display" ? "Select object in a stored display frame…" : "Select object in a stored frame…";
    U.clear(E.results); renderQuery(); loadIndexed(); loadAuto();
  }

  // The object totals of the selected source from the VM status: camera = the top-level numbers, display = status.objects.display.
  function srcState(o) {
    if (!o) return null;
    if (Q.source !== "display") return o;
    const d = o.display || {};
    return Object.assign({}, d, { enabled: o.enabled, available: o.available !== false && d.available !== false, vector_bytes: o.vector_bytes, max_regions: o.max_regions,
      reason: d.available === false ? "Proof of Play is not enabled on this player, so there are no display frames" : o.reason });
  }
  function renderStatusBanner() {
    if (!mounted) return;
    const root = objState(), o = srcState(root), box = U.clear(E.banner), disp = Q.source === "display";
    if (!root || !root.enabled) { box.append(h("div", { class: "vm-banner warn" }, h("b", { text: "Object search is disabled. " }), "Set visual_memory.objects.enabled in the Argus configuration (see docs/VISUAL_MEMORY_OBJECT_SEARCH.md).")); }
    else if (!o || o.available === false) box.append(h("div", { class: "vm-banner bad", text: (disp ? "Display objects unavailable: " : "Object index unavailable: ") + ((o && o.reason) || "unknown reason") }));
    else if (o.regions === 0) box.append(h("div", { class: "vm-banner", "data-role": "empty-index" }, h("b", { text: disp ? "No BrightSign display objects are indexed yet. " : "The object index is empty. " }),
      disp ? "Choose “Select object in a stored display frame…”, pick a Proof-of-Play frame and draw a box around an object, or run the display backfill below. Frames exist only for registered Proof-of-Play sessions."
           : "Open a stored frame in Visual Memory, choose “Select Object”, draw a box around an object and choose “Save Object to Index”."));
    else {
      const man = isNum(o.manual_regions) ? o.manual_regions : o.regions, aut = isNum(o.auto_regions) ? o.auto_regions : 0;
      box.append(h("p", { class: "muted", "data-role": "object-totals" },
        h("b", { text: `${fmtInt(o.regions)} indexed ${disp ? "display " : ""}object${o.regions === 1 ? "" : "s"}` }),
        ` (${fmtInt(man)} manual · ${fmtInt(aut)} automatic) · ${isNum(o.frames_processed) ? fmtInt(o.frames_processed) + (disp ? " display frames" : " frames") + " processed by the detector" : "detector off"} · ` +
        `${U.fmtBytes(o.vector_bytes)} of vectors${isNum(o.crop_bytes) ? " · " + U.fmtBytes(o.crop_bytes) + " of crop images (" + fmtInt(o.crops_retained || 0) + " retained, " + fmtInt(o.crops_expired || 0) + " expired)" : ""} · limit ${fmtInt(o.max_regions)}`));
    }
    E.run.disabled = !(root && root.enabled && o && o.available !== false && Q.query && !Q.ctrl);
    E.pickFrame.disabled = !(root && root.enabled && o && o.available !== false);
  }

  function renderQuery() {
    const box = U.clear(E.query), q = Q.query;
    if (!q) { box.append(h("p", { class: "muted", text: Q.source === "display" ? "No query yet. Select an object in a stored BrightSign display frame, or pick a display object that is already indexed." : "No query yet. Select an object in a stored frame, or pick an object that is already indexed." })); renderStatusBanner(); return; }
    if (q.type === "roi") {
      const rf = fref(q.frame);
      box.append(h("div", { class: "vm-row" }, h("div", { class: "vm-roi-prev small" }, h("img", { src: cropUrl(rf, q.roi, 200), alt: "Query crop" })),
        h("dl", { class: "vm-dl" }, h("dt", { text: "Query" }), h("dd", { text: "Crop of " + (rf.source_type === "display" ? "display frame #" + rf.id + " (" + rf.kind + ")" : "frame #" + q.frame.id) }), h("dt", { text: "Region" }), h("dd", { text: bboxTxt(q.roi) }))));
    } else {
      const r = q.region;
      box.append(h("div", { class: "vm-row" }, h("div", { class: "vm-roi-prev small" }, objThumb(r.region_id, 200)),
        h("dl", { class: "vm-dl" }, h("dt", { text: "Query" }), h("dd", { text: "Indexed object #" + r.region_id + (r.label ? " “" + r.label + "”" : "") }),
          h("dt", { text: "Region" }), h("dd", { text: `${isDisp(r) ? "display frame #" + r.frame_id + " (" + r.source_kind + ")" : "frame #" + r.frame_id} · ${bboxTxt(r.bbox)}` }))));
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
    if (E.origin.value) body.origin = E.origin.value;
    if (E.cls.value) body.class = E.cls.value;
    if (E.minOn.checked) { const ms = Number(E.minSim.value); if (!(ms >= 0 && ms <= 1)) { E.msg.textContent = "Minimum similarity must be between 0 and 1."; return; } body.min_similarity = ms; }
    if (E.reqImg.checked) body.require_image = true;                   // keeps objects whose own crop OR source frame is still retained
    body.source_type = Q.source;                                       // never mixes camera and display objects unless "all" is chosen explicitly (not offered here)
    if (Q.query.type === "roi") { const rf = fref(Q.query.frame); Object.assign(body, { frame_source_type: rf.source_type, kind: rf.kind, frame_id: rf.id }, Q.query.roi); }
    else body.region_id = Q.query.region.region_id;
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
    if (d.scope && d.scope.message) box.append(h("div", { class: "vm-banner", "data-role": "search-scope" }, h("b", { text: "Scope: " }), d.scope.message));
    box.append(h("p", { class: "muted", style: "margin-top:10px", text: `${res.length} result${res.length === 1 ? "" : "s"} · ${isNum(st.elapsed_ms) ? st.elapsed_ms.toFixed(1) + " ms" : DASH} · ${fmtInt(st.candidates_scanned)} indexed objects scanned` }));
    if (!res.length) {
      box.append(h("div", { class: "vm-empty", text: d.status && d.status.empty_index ? (Q.source === "display" ? "No display objects are indexed yet: select an object in a display frame and save it, or run the display backfill." : "The object index is empty: save some objects first.") : "No indexed objects match these options." })); return;
    }
    const grid = h("div", { class: "vm-grid", "data-role": "result-grid" });
    res.forEach((x) => {
      grid.append(h("button", { class: "vm-card", type: "button", "data-source": x.source_type, onclick: () => openCompare(x, d), "aria-label": `Result ${x.rank}, object ${x.region_id}, similarity ${isNum(x.similarity) ? x.similarity.toFixed(4) : "unknown"}` },
        h("div", { class: "vm-thumb" }, h("span", { class: "vm-rank", text: "#" + x.rank }), objThumb(x.region_id, 240)),
        h("div", { class: "meta" },
          h("span", { class: "vm-score", text: isNum(x.similarity) ? x.similarity.toFixed(4) : DASH }),
          h("div", { class: "vm-bar", "aria-hidden": "true" }, isNum(x.similarity) ? h("i", { style: `width:${clamp(x.similarity, 0, 1) * 100}%` }) : null),
          x.label ? h("b", { text: pretty(x.label) }) : null,
          x.index_origin === "auto" ? h("span", { class: "muted", "data-role": "class-conf", text: classText(x) }) : null,
          U.tsEl(x.capture_utc_ms, "t"), h("span", { class: "src", title: x.source_id || "", text: x.source_id || DASH }),
          h("span", { class: "muted", text: `${isDisp(x) ? "display frame #" + x.frame_id + " (" + x.source_kind + ")" : "frame #" + x.frame_id} · box ${x.bbox.x},${x.bbox.y} ${x.bbox.w}×${x.bbox.h}` }),
          isDisp(x) ? h("span", { class: "muted", "data-role": "display-facts", text: adText(x) }) : null,
          isDisp(x) ? h("span", { class: "muted", "data-role": "display-offset", text: offText(x) }) : null,
          isDisp(x) && x.pop_session_id ? h("span", { class: "muted", "data-role": "session-id", text: "session " + x.pop_session_id }) : null,
          h("div", { class: "vm-badges" }, originBadge(x), cropBadge(x), frameBadge(x)))));
    });
    box.append(grid);
  }

  function openCompare(x, d) {
    const both = (ms) => h("span", null, U.tsEl(ms), " ", h("span", { class: "muted", text: "(" + U.otherZone(ms) + ")" }));
    const q = Q.query;
    const qref = q && q.type === "roi" ? fref(q.frame) : null;
    const qSide = h("div", null, h("h3", { text: "Query object" }),
      h("div", { class: "vm-roi-prev" }, q && q.type === "roi" ? h("img", { src: cropUrl(qref, q.roi, 400), alt: "Query crop" }) : q ? objThumb(q.region.region_id, 400, true) : null),
      q && q.type === "roi" ? h("p", { class: "muted", text: `${qref.source_type === "display" ? "Display frame #" + qref.id + " (" + qref.kind + ")" : "Frame #" + q.frame.id}, region ${bboxTxt(q.roi)}` }) : null);
    const mDl = h("dl", { class: "vm-dl" }, h("dt", { text: "Object" }), h("dd", { text: "#" + x.region_id + (x.label ? " “" + pretty(x.label) + "”" : "") }),
      h("dt", { text: "Indexed" }), h("dd", null, originBadge(x), " ", cropBadge(x), " ", frameBadge(x)),
      x.index_origin === "auto" ? h("dt", { text: "Detector" }) : null,
      x.index_origin === "auto" ? h("dd", { text: `${pretty(x.class_name)} (class ${x.class_id}), confidence ${isNum(x.confidence) ? x.confidence.toFixed(3) : DASH}, ${x.detector} ${x.detector_version || ""}` }) : null,
      h("dt", { text: "Similarity" }), h("dd", { text: isNum(x.similarity) ? x.similarity.toFixed(4) + "  (cosine; not a calibrated probability)" : DASH }),
      h("dt", { text: "Captured" }), h("dd", null, both(x.capture_utc_ms)), h("dt", { text: "Source" }), h("dd", { text: x.source_id || DASH }),
      h("dt", { text: "Parent frame" }), h("dd", { text: isDisp(x) ? `BrightSign display frame #${x.frame_id} (${x.source_kind}, Weston/VOP2)` : "#" + x.frame_id }),
      isDisp(x) ? h("dt", { text: "Advertisement" }) : null, isDisp(x) ? h("dd", { "data-role": "ad-facts", text: `${adText(x)}` }) : null,
      isDisp(x) ? h("dt", { text: "Playback" }) : null, isDisp(x) ? h("dd", { text: offText(x) + (x.provenance ? " · " + x.provenance : "") }) : null,
      isDisp(x) ? h("dt", { text: "Proof-of-Play session" }) : null, isDisp(x) ? h("dd", { "data-role": "session-id" }, x.pop_session_id || DASH, " ", x.pop_session_id ? sessionBtn(x, true) : null) : null,
      h("dt", { text: "Bounding box" }), h("dd", { text: `x ${x.bbox.x}, y ${x.bbox.y}, ${x.bbox.w} × ${x.bbox.h} in ${x.image_width} × ${x.image_height}` }));
    const mSide = h("div", null, h("h3", { text: `Match #${x.rank}` }), h("div", { class: "vm-roi-prev" }, objThumb(x.region_id, 400, true)), mDl);
    const src = h("div", { style: "margin-top:14px" }, h("h3", { text: "Source frame with bounding box" }),
      frameWithBox(x));
    const again = h("button", { class: "vm-btn small", type: "button", text: "Find similar to this object", onclick: () => { modal.close(); searchFromRegion(x); } });
    const detail = isDisp(x) ? (x.pop_session_id ? sessionBtn(x, true) : null) : h("button", { class: "vm-btn small", type: "button", text: "Open frame detail", onclick: () => U.openDetail(x.frame_id) });
    const body = h("div", null, h("div", { class: "vm-label", style: "margin-bottom:12px", text: d.label || "" }),
      h("div", { class: "vm-cmp" }, qSide, mSide), src, h("div", { class: "vm-row", style: "margin-top:12px" }, again, detail));
    const modal = U.openModal("Compare query object and match", body, { wide: true });
    U.tzSubscribe(() => { if (body.isConnected) U.refreshTs(body); });
  }

  let indexedSeq = 0;
  async function loadIndexed() {
    const seq = ++indexedSeq;
    const box = U.clear(E.indexed);
    const q = "&source_type=" + Q.source + (E.origin && E.origin.value ? "&origin=" + encodeURIComponent(E.origin.value) : "") + (E.cls && E.cls.value ? "&class=" + encodeURIComponent(E.cls.value) : "");
    const r = await U.api("/objects?limit=24" + q);
    if (seq !== indexedSeq) return;                                     // a newer filter change superseded this response
    if (r.status !== 200 || !r.data) { box.append(h("p", { class: "muted", text: r.status === 503 ? "" : "Could not list indexed objects." })); return; }
    const rows = r.data.objects || [];
    box.append(h("h3", { text: `Indexed ${Q.source === "display" ? "BrightSign display " : ""}objects (${fmtInt(r.data.total)})` }));
    if (!rows.length) { box.append(h("p", { class: "muted", text: "None yet." })); return; }
    const grid = h("div", { class: "vm-grid" });
    rows.forEach((o) => grid.append(h("button", { class: "vm-card", type: "button", title: "Search for objects similar to this one", onclick: () => searchFromRegion(o) },
      h("div", { class: "vm-thumb" }, objThumb(o.region_id, 200)),
      h("div", { class: "meta" }, h("b", { text: pretty(o.label) || "(no label)" }),
        o.index_origin === "auto" ? h("span", { class: "muted", text: classText(o) }) : null,
        U.tsEl(o.capture_utc_ms, "t"), h("span", { class: "muted", text: `#${o.region_id} · ${isDisp(o) ? "display frame #" + o.frame_id : "frame #" + o.frame_id}` }),
        isDisp(o) ? h("span", { class: "muted", text: adText(o) }) : null,
        isDisp(o) ? h("span", { class: "muted", text: offText(o) }) : null,
        isDisp(o) && o.pop_session_id ? h("span", { class: "muted", text: "session " + o.pop_session_id }) : null,
        h("div", { class: "vm-badges" }, originBadge(o), cropBadge(o))))));
    box.append(grid);
  }


  // ---- automatic indexing panel: state, progress, classes, backfill, errors ------------------------------------------
  const fmtAgo = (ms) => { if (!isNum(ms) || ms <= 0) return DASH; const s = Math.max(0, Math.round((Date.now() - ms) / 1000)); return s < 90 ? s + " s ago" : s < 5400 ? Math.round(s / 60) + " min ago" : Math.round(s / 3600) + " h ago"; };
  async function loadAuto() {
    if (!mounted) return;
    const want = Q.source;
    const r = await U.api("/objects/auto/status?source=" + want);
    if (want !== Q.source) return;                                        // the source was switched while the request was in flight
    const was = A.data && A.data.backfill && A.data.backfill.state;
    A.data = r.status === 200 ? r.data : null; A.http = r.status;
    renderAuto();
    const bf = A.data && A.data.backfill;
    if (bf && was && (was === "running" || was === "queued") && bf.state !== was) { U.pollStatus(); loadIndexed(); }   // totals in the banner follow the finished backfill
    clearTimeout(A.timer);
    if (mounted && bf && (bf.state === "running" || bf.state === "queued")) A.timer = setTimeout(() => { loadAuto(); loadIndexed(); }, 1500);
  }
  function renderAuto() {
    if (!mounted || !E.auto) return;
    const box = U.clear(E.auto), d = A.data;
    if (!objEnabled()) return;
    const disp = Q.source === "display";
    const head = h("div", { class: "panel-head" }, h("h2", { text: disp ? "Automatic object indexing: BrightSign display" : "Automatic object indexing" }));
    if (!d) { box.append(h("section", { class: "panel" }, head, h("p", { class: "muted", text: A.http === 401 ? "Locked: access token required." : "Status unavailable." }))); return; }
    const body = h("div", { class: "vm-stack" });
    if (!d.enabled) {
      body.append(h("p", { "data-role": "auto-state", class: "muted", text: disp
        ? "Off. Display objects are indexed only when someone selects them (manual). To let the detector find bottles, cups, laptops and similar objects in the Weston/VOP2 frames kept by Proof of Play, enable visual_memory.objects.auto.display in the Argus configuration (Proof of Play must be on)."
        : "Off. Objects are indexed only when someone selects them (manual). To let the detector find bottles, cups, laptops and similar objects in stored frames, enable visual_memory.objects.auto in the Argus configuration." }));
    } else {
      const w = d.worker || {}, pr = d.progress || {}, bf = d.backfill || {}, ob = d.objects || {}, fr = d.frames || {};
      const sc = (d.source_config || {})[Q.source] || {};
      const sw = (d.sources || []).find((x) => x.type === Q.source) || {};
      const otherBusy = (bf.state === "running" || bf.state === "queued") && bf.source && bf.source !== Q.source;
      const stateCls = w.state === "backoff" || w.last_error ? "warn" : "ok";
      body.append(h("div", { class: "vm-row" },
        h("span", { class: "vm-badge " + stateCls, "data-role": "auto-state", text: "worker: " + (w.state || "starting") }),
        h("span", { class: "vm-badge", "data-role": "live-state", text: (sc.live_scan ? "live scan on" : "live scan off (backfill only)") }),
        h("span", { class: "muted", text: `${d.detector_kind === "mock" ? "mock detector (dev)" : "YOLOX-S " + (w.detector_version ? "(" + w.detector_version + ")" : "")} · min confidence ${Number(d.min_confidence).toFixed(2)} · ≤${d.max_objects_per_frame} objects/frame · ${sc.frames_per_minute || d.frames_per_minute} frames/min for this source` })));
      body.append(h("p", { class: "muted", "data-role": "auto-progress" },
        h("b", { text: `${fmtInt(pr.frames_processed || 0)} ${disp ? "display " : ""}frames processed` }),
        ` of ${fmtInt(fr.stored || 0)} stored (${fmtInt(fr.with_retained_jpeg || 0)} still have a JPEG; expired frames cannot be processed${disp ? "; display frames exist only for registered Proof-of-Play sessions" : ""}) · ${fmtInt(pr.frames_with_objects || 0)} with objects · ${fmtInt(pr.frames_failed || 0)} failed or unavailable · ${fmtInt(ob.auto || 0)} automatic objects · last processed ${fmtAgo(pr.last_processed_utc_ms)}` +
        (isNum(w.avg_detect_ms) && w.avg_detect_ms > 0 ? ` · detector ${w.avg_detect_ms.toFixed(0)} ms, indexing ${isNum(w.avg_index_ms) ? w.avg_index_ms.toFixed(0) : DASH} ms per frame` : "") +
        (sw.queue_capacity ? ` · queue ${sw.queue_depth}/${sw.queue_capacity}` : "") + (sw.skipped_stale ? ` · ${fmtInt(sw.skipped_stale)} live frames sampled out` : "")));
      body.append(h("p", { class: "muted", "data-role": "auto-classes" }, "Looks for: " + (d.classes || []).map(pretty).join(", ") + ". People are never indexed automatically."));
      if (w.last_error) body.append(h("div", { class: "vm-banner bad", "data-role": "auto-error" }, h("b", { text: "Last worker error " + fmtAgo(w.last_error_utc_ms) + ": " }), w.last_error));
      const row = h("div", { class: "vm-row" });
      const busy = (bf.state === "running" || bf.state === "queued") && (!bf.source || bf.source === Q.source);
      const btn = h("button", { class: "vm-btn", type: "button", "data-role": "backfill-run", disabled: busy || otherBusy, text: busy ? "Indexing…" : otherBusy ? "Another backfill is running…" : disp ? "Index the 50 most recent retained display frames" : "Index the 50 most recent retained frames",
        title: "Bounded backfill: at most 50 frames that still have a JPEG and were not processed yet", onclick: async () => {
          btn.disabled = true; A.msg = "";
          const r = await U.api("/objects/auto/backfill", { method: "POST", credentials: "same-origin", cache: "no-store", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ source: Q.source, limit: 50 }) });
          if (r.status !== 202) A.msg = errText(r.status, r.data);
          await loadAuto();
        } });
      row.append(btn);
      if (busy) row.append(h("button", { class: "vm-btn", type: "button", text: "Cancel", onclick: async () => { await U.api("/objects/auto/backfill/cancel", { method: "POST", credentials: "same-origin" }); loadAuto(); } }));
      body.append(row);
      if (bf.state && bf.state !== "none" && (!bf.source || bf.source === Q.source)) body.append(h("p", { class: "muted", "data-role": "backfill-state",
        text: `Backfill ${bf.state}: ${bf.done}/${bf.total} frames, ${bf.indexed} objects indexed, ${bf.failed} failed` + (bf.state === "done" && bf.total === 0 ? " (no unprocessed frame with a retained JPEG)" : "") }));
      if (A.msg) body.append(h("div", { class: "vm-banner bad", text: A.msg }));
      // class chips = filters (counts of automatic objects per class)
      const bc = d.by_class || [];
      if (bc.length) {
        const chips = h("div", { class: "vm-badges", "data-role": "class-chips" });
        bc.forEach((c) => chips.append(h("button", { class: "vm-btn small", type: "button", "data-class": c.class_name, text: `${pretty(c.class_name)} ${fmtInt(c.count)}`,
          onclick: () => { E.cls.value = c.class_name; E.origin.value = "auto"; loadIndexed(); } })));
        body.append(chips);
        const have = new Set([...E.cls.options].map((o) => o.value));
        bc.forEach((c) => { if (!have.has(c.class_name)) E.cls.append(h("option", { value: c.class_name, text: pretty(c.class_name) })); });
      }
    }
    box.append(h("section", { class: "panel" }, head, body));
  }

  // Display frames: choose a Proof-of-Play session, then one of its stored Weston/VOP2 frames (history samples and checkpoints).
  function pickDisplayFrame() {
    const grid = h("div", { class: "vm-grid", "data-role": "display-frame-grid" }), msg = h("div", { class: "muted" });
    const sess = h("select", { "aria-label": "Proof-of-Play session", "data-role": "session-filter" }, h("option", { value: "", text: "All sessions (newest frames first)" }));
    let seq = 0;
    async function load() {
      const my = ++seq; U.clear(grid); msg.textContent = "Loading…";
      const r = await U.api("/display-frames?limit=48" + (sess.value ? "&session_id=" + encodeURIComponent(sess.value) : ""));
      if (my !== seq) return;
      if (r.status !== 200 || !r.data) { msg.textContent = r.status === 503 ? ((r.data && r.data.message) || "Display frames are not available.") : "Could not list display frames."; return; }
      const fr = r.data.frames || [];
      msg.textContent = fr.length ? `${fmtInt(r.data.total)} stored Weston/VOP2 frame${r.data.total === 1 ? "" : "s"}${r.data.total > fr.length ? " (newest " + fr.length + " shown; choose a session to narrow)" : ""}. Frames are stored only during registered Proof-of-Play sessions.`
                              : "No display frames are stored: frames exist only for registered Proof-of-Play sessions (run one under Proof of Play).";
      fr.forEach((f) => grid.append(h("button", { class: "vm-card", type: "button", "data-frame-kind": f.kind, "data-frame-id": String(f.id), disabled: !f.image_available,
        title: f.image_available ? "Select an object in this frame" : "JPEG expired", onclick: () => { m.close(); openSelector({ source_type: "display", kind: f.kind, id: f.id, width: f.width, height: f.height, capture_utc_ms: f.capture_utc_ms, source_id: "main_display", image_available: f.image_available, image_exists: f.image_available }); } },
        h("div", { class: "vm-thumb" }, f.image_available ? U.loadImg(f.image_url, "display frame " + f.id, "thumb", false) : h("span", { class: "vm-empty", text: "JPEG expired" })),
        h("div", { class: "meta" }, U.tsEl(f.capture_utc_ms, "t"), h("span", { class: "muted", text: `${f.kind} #${f.id} · session ${f.session_id}` }),
          h("span", { class: "muted", text: `ad ${f.expected_media_id || DASH}${f.reported_media_id && f.reported_media_id !== f.expected_media_id ? " (player reported " + f.reported_media_id + ")" : ""} · ${isNum(f.playback_offset_ms) ? "+" + fmtDur(f.playback_offset_ms) : "offset ?"}` })))));
    }
    sess.addEventListener("change", load);
    (async () => {
      let list = [];
      try { const r = await fetch("/api/pop/sessions?limit=30", { cache: "no-store", credentials: "same-origin" }); const d = r.status === 200 ? await r.json() : null; list = d && Array.isArray(d.sessions) ? d.sessions : []; } catch (_) {}
      list.forEach((x) => sess.append(h("option", { value: x.session_id, text: `${x.session_id} · ${x.expected_media_id || ""} · ${x.state || ""}` })));
    })();
    const m = U.openModal("Pick a BrightSign display frame", h("div", null, h("p", { class: "muted", text: "The BrightSign display output as captured through Weston/VOP2 (approximate, composed). Choose the frame that contains the object." }),
      h("div", { class: "vm-row" }, h("label", null, "Session", sess)), msg, grid), { wide: true });
    load();
  }

  function pickFrame() {
    if (Q.source === "display") { pickDisplayFrame(); return; }
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
    E.root = slot; slot.dataset.source = Q.source;
    E.banner = h("div"); E.query = h("div"); E.results = h("div", { "aria-live": "polite" }); E.indexed = h("div");
    E.topK = h("input", { type: "number", min: "1", max: "50", step: "1", value: "5", "aria-label": "Top K" });
    E.label = h("input", { type: "text", maxlength: "64", placeholder: "any", "aria-label": "Label filter", style: "width:140px" });
    E.tf = U.makeTimeFilter(() => {}); E.src = U.sourceSelect(() => {});
    E.minOn = h("input", { type: "checkbox" }); E.minSim = h("input", { type: "number", min: "0", max: "1", step: "0.01", value: "0.80", "aria-label": "Minimum similarity" });
    E.reqImg = h("input", { type: "checkbox" });
    E.origin = h("select", { "aria-label": "Indexed by", "data-role": "origin-filter", onchange: () => loadIndexed() }, h("option", { value: "", text: "manual + automatic" }), h("option", { value: "manual", text: "manual only" }), h("option", { value: "auto", text: "automatic only" }));
    E.cls = h("select", { "aria-label": "Class", "data-role": "class-filter", onchange: () => loadIndexed() }, h("option", { value: "", text: "any class" }));
    E.auto = h("div", { "data-role": "auto-panel" });
    E.pickFrame = h("button", { class: "vm-btn primary", type: "button", text: "Select object in a stored frame…", onclick: pickFrame });
    E.run = h("button", { class: "vm-btn primary", type: "button", text: "Find Similar Objects", disabled: true, onclick: runSearch });
    E.cancel = h("button", { class: "vm-btn", type: "button", text: "Cancel", hidden: true, onclick: () => { if (Q.ctrl) Q.ctrl.abort(); } });
    E.spin = h("span", { class: "vm-spin", hidden: true }); E.msg = h("span", { class: "muted" });
    slot.append(h("div", { class: "vm-stack" }, h("div", { class: "vm-note", text: NOTE }), E.banner, E.auto,
      h("div", { class: "vm-search-grid" },
        h("div", { class: "panel" }, h("div", { class: "panel-head" }, h("h2", { text: "1. Query object" })), E.query, h("div", { class: "vm-row", style: "margin-top:10px" }, E.pickFrame)),
        h("div", { class: "panel" }, h("div", { class: "panel-head" }, h("h2", { text: "2. Options" })),
          h("div", { class: "vm-stack" },
            h("div", { class: "vm-row" }, h("label", null, "Top K (1–50)", E.topK), h("label", null, "Label", E.label), h("label", null, "Source", E.src), h("label", null, "Indexed by", E.origin), h("label", null, "Class", E.cls)),
            E.tf.el,
            h("div", { class: "vm-row" }, h("label", { class: "inline" }, E.minOn, "Minimum similarity (off by default)"), E.minSim,
              h("label", { class: "inline" }, E.reqImg, "Require source JPEG")),
            h("div", { class: "vm-row" }, E.run, E.cancel, E.spin, E.msg)))),
      E.results, h("section", { class: "panel" }, E.indexed)));
    U.onStatus(() => { renderStatusBanner(); });
    E.src.parentElement.hidden = Q.source === "display";
    renderQuery(); loadIndexed(); loadAuto();
    U.tzSubscribe(() => U.refreshTs(slot));
  }

  window.VMObjects = { openSelector, mountSearch, setSource, searchFromRoi, searchFromRegion, enabled: objEnabled, reload: () => { if (mounted) { loadIndexed(); loadAuto(); } }, _geom: geom };
  U.registerObjects(window.VMObjects);
})();
