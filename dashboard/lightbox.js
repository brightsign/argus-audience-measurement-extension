/* Argus dashboard: click-to-enlarge image viewer shared by Proof of Play and Visual Search.
   Any <img data-zoom="1" data-title="..." data-meta="line1|line2"> inside a root passed to VMUI.lightbox.attach(root) opens full size on click,
   with previous/next (arrow keys), a link to the original JPEG and "Search with this image", which hands the picture to Visual Search
   (BrightSign Display source) as the reference. Server strings are rendered with textContent only. */
(() => {
  "use strict";
  if (!window.VMUI) return;
  const U = window.VMUI, h = U.h;
  let bg = null, st = null;

  function close() {
    if (bg) { bg.remove(); bg = null; }
    document.removeEventListener("keydown", onKey, true);
    st = null;
  }
  function onKey(e) {
    if (!st) return;
    if (e.key === "Escape") { e.stopPropagation(); close(); }
    else if (e.key === "ArrowLeft") { e.preventDefault(); step(-1); }
    else if (e.key === "ArrowRight") { e.preventDefault(); step(1); }
  }
  function step(d) { if (!st || st.items.length < 2) return; st.i = (st.i + d + st.items.length) % st.items.length; render(); }

  async function searchWith(it) {
    const msg = bg && bg.querySelector("[data-role=lb-msg]");
    try {
      const r = await fetch(it.url, { credentials: "same-origin", cache: "no-store" });
      if (!r.ok) throw new Error("HTTP " + r.status);
      const b = await r.blob();
      if (!b.size || (b.type && b.type !== "image/jpeg")) throw new Error("not a JPEG");
      close();
      document.querySelectorAll(".vm-modal-head button").forEach((x) => x.click());       // close an open session window
      if (U.useDisplayReference) U.useDisplayReference(new File([b], "stored-frame.jpg", { type: "image/jpeg" }));
    } catch (e) { if (msg) msg.textContent = "Could not use this image as a search reference: " + e.message; }
  }

  function render() {
    const it = st.items[st.i];
    bg.replaceChildren(
      h("div", { class: "lb-box", role: "dialog", "aria-modal": "true", "aria-label": "Image viewer" },
        h("div", { class: "lb-top" },
          h("b", { text: it.title || "Image" }), h("span", { class: "muted", text: st.items.length > 1 ? `${st.i + 1} / ${st.items.length}` : "" }),
          h("button", { class: "vm-btn small", type: "button", text: "Close ✕", onclick: close })),
        h("div", { class: "lb-stage" },
          st.items.length > 1 ? h("button", { class: "vm-btn lb-nav", type: "button", "aria-label": "Previous image", text: "‹", onclick: () => step(-1) }) : null,
          h("img", { src: it.url, alt: it.title || "Stored frame" }),
          st.items.length > 1 ? h("button", { class: "vm-btn lb-nav", type: "button", "aria-label": "Next image", text: "›", onclick: () => step(1) }) : null),
        h("div", { class: "lb-meta" }, ...(it.meta || []).map((m) => h("div", { class: "muted", text: m }))),
        h("div", { class: "lb-actions" },
          h("a", { class: "vm-btn small", href: it.url, target: "_blank", rel: "noopener", text: "Open original JPEG" }),
          it.searchable === false ? null : h("button", { class: "vm-btn small primary", type: "button", "data-role": "lb-search", text: "Search with this image", onclick: () => searchWith(it) }),
          h("span", { class: "muted", "data-role": "lb-msg" }))));
  }

  function open(items, index) {
    if (!items || !items.length) return;
    close();
    st = { items, i: Math.max(0, Math.min(items.length - 1, index | 0)) };
    bg = h("div", { class: "lb-bg" });
    bg.addEventListener("click", (e) => { if (e.target === bg) close(); });
    document.body.append(bg);
    document.addEventListener("keydown", onKey, true);
    render();
  }

  function attach(root) {
    if (!root || root._lbAttached) return;
    root._lbAttached = true;
    root.addEventListener("click", (e) => {
      const img = e.target && e.target.closest ? e.target.closest("img[data-zoom]") : null;
      if (!img || !root.contains(img)) return;
      e.preventDefault();
      const all = [...root.querySelectorAll("img[data-zoom]")];
      open(all.map((x) => ({ url: x.getAttribute("src"), title: x.dataset.title || x.alt || "Image", meta: (x.dataset.meta || "").split("|").filter(Boolean),
        searchable: x.dataset.searchable !== "0" })), all.indexOf(img));
    });
  }
  U.lightbox = { open, close, attach };
})();
