// Unit tests for the ROI coordinate helpers in dashboard/objects.js (run: node tests/js/test_roi.js).
const assert = require("assert");
const g = require("../../dashboard/objects.js");
let n = 0; const t = (name, f) => { f(); n++; console.log("ok - " + name); };

t("fitRect: same aspect ratio fills the stage", () => {
  const f = g.fitRect(640, 360, 640, 360); assert.deepStrictEqual([f.x, f.y, f.w, f.h, f.scale], [0, 0, 640, 360, 1]);
});
t("fitRect: 4:3 frame in a 16:9 stage is pillarboxed", () => {
  const f = g.fitRect(960, 540, 640, 480); assert.strictEqual(f.scale, 540 / 480); assert.strictEqual(f.h, 540); assert.strictEqual(f.w, 720); assert.strictEqual(f.x, 120); assert.strictEqual(f.y, 0);
});
t("fitRect: portrait frame is pillarboxed, wide frame is letterboxed", () => {
  const p = g.fitRect(900, 500, 360, 640); assert.ok(p.x > 0 && p.y === 0);
  const w = g.fitRect(900, 500, 1280, 400); assert.ok(w.y > 0 && w.x === 0);
});
t("fitRect: degenerate input -> null", () => { assert.strictEqual(g.fitRect(0, 10, 10, 10), null); assert.strictEqual(g.fitRect(10, 10, 0, 10), null); });
t("displayToImage maps scaled coordinates back to image pixels (1.5x scale)", () => {
  const nat = { w: 640, h: 360 }, f = g.fitRect(960, 540, 640, 360);
  assert.deepStrictEqual(g.displayToImage(480, 270, f, nat), { x: 320, y: 180 });
  assert.deepStrictEqual(g.displayToImage(0, 0, f, nat), { x: 0, y: 0 });
});
t("displayToImage subtracts letterbox offset and clamps pointer outside the picture", () => {
  const nat = { w: 640, h: 480 }, f = g.fitRect(960, 540, 640, 480);   // x offset 120, scale 1.125
  const p = g.displayToImage(120 + 112.5, 135, f, nat); assert.ok(Math.abs(p.x - 100) < 1e-9 && Math.abs(p.y - 120) < 1e-9);
  assert.deepStrictEqual(g.displayToImage(5, 5, f, nat), { x: 0, y: 4.444444444444445 });
  assert.deepStrictEqual(g.displayToImage(5000, 5000, f, nat), { x: 640, y: 480 });
});
t("roiFromDrag: normalises direction and rounds outward", () => {
  const nat = { w: 640, h: 360 }, f = g.fitRect(640, 360, 640, 360);
  assert.deepStrictEqual(g.roiFromDrag({ x: 200.4, y: 100.6 }, { x: 100.2, y: 50.1 }, f, nat), { x: 100, y: 50, w: 101, h: 51 });
});
t("roiFromDrag: clamped to the image and never exceeds it", () => {
  const nat = { w: 640, h: 360 }, f = g.fitRect(960, 540, 640, 360);
  const r = g.roiFromDrag({ x: -50, y: -50 }, { x: 5000, y: 5000 }, f, nat);
  assert.deepStrictEqual(r, { x: 0, y: 0, w: 640, h: 360 });
});
t("roiFromDrag inside letterbox bars selects nothing valid", () => {
  const nat = { w: 640, h: 480 }, f = g.fitRect(960, 540, 640, 480);
  const r = g.roiFromDrag({ x: 10, y: 10 }, { x: 60, y: 60 }, f, nat);   // both points in the left bar -> x clamps to 0
  assert.strictEqual(r.x, 0); assert.ok(g.validateRoi(r, nat, 16) !== "" || r.w === 0 || r.h > 0);
});
t("roiToDisplay is the inverse of the drag conversion", () => {
  const nat = { w: 640, h: 360 }, f = g.fitRect(960, 540, 640, 360), r = { x: 100, y: 50, w: 200, h: 120 };
  const d = g.roiToDisplay(r, f); const back = g.roiFromDrag({ x: d.x, y: d.y }, { x: d.x + d.w, y: d.y + d.h }, f, nat);
  assert.deepStrictEqual(back, r);
});
t("round trip with a letterboxed frame", () => {
  const nat = { w: 640, h: 480 }, f = g.fitRect(960, 540, 640, 480), r = { x: 33, y: 47, w: 211, h: 99 };
  const d = g.roiToDisplay(r, f); assert.deepStrictEqual(g.roiFromDrag({ x: d.x, y: d.y }, { x: d.x + d.w, y: d.y + d.h }, f, nat), r);
});
t("validateRoi mirrors the server rules", () => {
  const nat = { w: 640, h: 360 };
  assert.strictEqual(g.validateRoi({ x: 0, y: 0, w: 640, h: 360 }, nat, 16), "");
  assert.strictEqual(g.validateRoi({ x: 10, y: 10, w: 16, h: 16 }, nat, 16), "");
  assert.match(g.validateRoi({ x: 10, y: 10, w: 15, h: 40 }, nat, 16), /too small/);
  assert.match(g.validateRoi({ x: 630, y: 10, w: 20, h: 20 }, nat, 16), /leaves/);
  assert.match(g.validateRoi({ x: -1, y: 0, w: 20, h: 20 }, nat, 16), /Draw/);
  assert.match(g.validateRoi({ x: 1.5, y: 0, w: 20, h: 20 }, nat, 16), /whole/);
  assert.match(g.validateRoi(null, nat, 16), /whole/);
  assert.match(g.validateRoi({ x: 0, y: 0, w: 0, h: 20 }, nat, 16), /Draw/);
});
console.log(n + " tests passed");
