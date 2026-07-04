#!/usr/bin/env node
// Headless verification of the dashboard lock-response strip chart. Loads the
// real data/app.js in a vm context with a DOM stub and a controllable clock,
// drives the actual pushLockSample()/renderLockTrace()/refreshStatus(), and
// asserts the #lockTraceSvg content: the actual line maps lock% to the right Y,
// the target dashed line appears only where a target is present, the buffer is
// pruned to the rolling window, and a dropped link clears the trace. No browser.
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const src = fs.readFileSync(path.join(__dirname, "..", "data", "app.js"), "utf8");

// --- DOM stub ---------------------------------------------------------------
function makeNode(id) {
  const handlers = {};
  const classes = new Set();
  return {
    id, innerHTML: "", textContent: "", value: "", style: {}, checked: false,
    dataset: {}, _attrs: {}, _classes: classes,
    classList: {
      add: (...c) => c.forEach((x) => classes.add(x)),
      remove: (...c) => c.forEach((x) => classes.delete(x)),
      toggle() {}, contains: (x) => classes.has(x),
    },
    setAttribute(k, v) { this._attrs[k] = v; },
    getAttribute(k) { return this._attrs[k] ?? null; },
    removeAttribute(k) { delete this._attrs[k]; },
    appendChild() {}, focus() {}, select() {},
    addEventListener(ev, fn) { (handlers[ev] = handlers[ev] || []).push(fn); },
    querySelectorAll() { return []; },
  };
}
const nodes = {};
function getNode(id) { if (!nodes[id]) nodes[id] = makeNode(id); return nodes[id]; }

const documentStub = {
  addEventListener() {},
  getElementById: (id) => getNode(id),
  querySelectorAll: () => [],
  querySelector: () => null,
  createElement: () => makeNode("created"),
  body: makeNode("body"),
};

// Controllable clock: renderLockTrace/pushLockSample take now explicitly, but
// refreshStatus reads Date.now() internally, so stub it to our fake clock.
let clock = 1_000_000;
const ctx = {
  document: documentStub,
  window: {}, console, setTimeout, clearTimeout,
  setInterval: () => 0, clearInterval: () => {},
  Date: { now: () => clock },
  fetch: (...args) => ctx._fetchImpl(...args),
  _fetchImpl: () => Promise.reject(new Error("no impl set")),
  AbortController: function () { this.signal = {}; this.abort = () => {}; },
  Math, Number, Array, JSON, parseInt, parseFloat, isNaN,
};
vm.createContext(ctx);
vm.runInContext(src, ctx);

// --- assertions -------------------------------------------------------------
let failures = 0;
function check(cond, msg) {
  if (!cond) { failures++; console.error("FAIL:", msg); }
  else console.log("ok  :", msg);
}

const svg = getNode("lockTraceSvg");

// Geometry mirrors renderLockTrace (viewBox 320x160).
const padT = 8, padB = 18, H = 160;
const plotH = H - padT - padB;              // 134
const yOf = (lock) => padT + (1 - lock / 100) * plotH;

// Parse the points= list of the Nth element whose opening tag matches `tag`.
function pointsOf(tag, idx = 0) {
  const re = new RegExp(`<${tag}\\b[^>]*\\bpoints="([^"]*)"`, "g");
  let m, i = 0;
  while ((m = re.exec(svg.innerHTML))) { if (i++ === idx) return m[1].trim(); }
  return null;
}
function vertices(ptsStr) {
  return ptsStr ? ptsStr.split(/\s+/).filter(Boolean).map((p) => p.split(",").map(Number)) : [];
}
function count(tag) {
  return (svg.innerHTML.match(new RegExp(`<${tag}\\b`, "g")) || []).length;
}

// 1) A run of samples with target present: actual polyline + a dashed target line.
ctx.resetLockTrace();
const seq = [
  { target: 0,   actual: 0 },
  { target: 100, actual: 20 },
  { target: 100, actual: 60 },
  { target: 100, actual: 100 },
];
seq.forEach((s) => { clock += 500; ctx.pushLockSample(s.target, s.actual, clock); });
ctx.renderLockTrace(clock);

const actualPts = vertices(pointsOf("polyline", 0));
check(actualPts.length === seq.length,
  `actual line has one vertex per sample (got ${actualPts.length}/${seq.length})`);
check(Math.abs(actualPts[actualPts.length - 1][1] - yOf(100)) < 0.6,
  `last actual=100% maps to top of plot (y≈${yOf(100).toFixed(1)}, got ${actualPts[actualPts.length - 1][1]})`);
check(Math.abs(actualPts[0][1] - yOf(0)) < 0.6,
  `first actual=0% maps to chart floor (y≈${yOf(0).toFixed(1)}, got ${actualPts[0][1]})`);
check(/stroke-dasharray/.test(svg.innerHTML), "target series drawn as a dashed line");
check(count("polygon") === 1, "actual area fill drawn under the line");

// 2) A missing actual reading is skipped, not plotted as a phantom zero.
ctx.resetLockTrace();
clock += 500; ctx.pushLockSample(50, 50, clock);
clock += 500; ctx.pushLockSample(50, null, clock);   // dropped engagement
clock += 500; ctx.pushLockSample(50, 55, clock);
ctx.renderLockTrace(clock);
check(vertices(pointsOf("polyline", 0)).length === 2,
  `null actual is not plotted (2 real points, got ${vertices(pointsOf("polyline", 0)).length})`);

// 3) A gap in the target (no CAN) breaks the dashed line into separate segments
//    rather than bridging the gap.
ctx.resetLockTrace();
clock += 500; ctx.pushLockSample(40, 10, clock);
clock += 500; ctx.pushLockSample(null, 12, clock);   // target absent
clock += 500; ctx.pushLockSample(40, 14, clock);
ctx.renderLockTrace(clock);
check(count("polyline") >= 3,
  `target gap splits into separate dashed segments (>=3 polylines incl. actual, got ${count("polyline")})`);

// 4) Old samples scroll off: pushing 50s of data at 500ms keeps only ~window.
ctx.resetLockTrace();
for (let i = 0; i < 100; i++) { clock += 500; ctx.pushLockSample(50, 50, clock); }
ctx.renderLockTrace(clock);
const kept = vertices(pointsOf("polyline", 0)).length;
// 15s window / 0.5s = 30 in-window + 1 older kept for the left edge.
check(kept >= 28 && kept <= 33,
  `buffer pruned to the rolling window (kept ${kept}, expected ~31)`);

// 5) A dropped link clears the trace: three failed polls -> Offline -> no data lines.
(async () => {
  ctx.resetLockTrace();
  clock += 500; ctx.pushLockSample(50, 50, clock);
  clock += 500; ctx.pushLockSample(50, 60, clock);
  ctx.renderLockTrace(clock);
  check(count("polyline") >= 1, "trace has data before the outage");

  ctx._fetchImpl = () => Promise.reject(new Error("network down"));
  for (let i = 0; i < 3; i++) { clock += 500; await ctx.refreshStatus(); }
  check(count("polyline") === 0 && count("polygon") === 0,
    "trace cleared after the link is declared Offline");

  console.log(failures === 0 ? "\nALL LOCK-TRACE CHECKS PASSED" : `\n${failures} CHECK(S) FAILED`);
  process.exit(failures === 0 ? 0 : 1);
})();
