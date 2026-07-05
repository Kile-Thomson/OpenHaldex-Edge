#!/usr/bin/env node
// Headless verification of the Expert tune-map chart. Loads the real data/app.js
// in a vm context with a minimal DOM stub, calls the actual renderTuneChart via
// its initTuneChart + toggle handlers, and asserts the produced SVG is well
// formed (correct line/point counts, all coordinates inside the viewBox) for
// both the speed and throttle views. No browser required.
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const src = fs.readFileSync(path.join(__dirname, "..", "data", "app.js"), "utf8");

// --- minimal DOM stub -------------------------------------------------------
function makeNode(id) {
  const handlers = {};
  return {
    id,
    innerHTML: "",
    textContent: "",
    value: "",
    placeholder: "",
    type: "text",
    disabled: false,
    style: {},
    checked: false,
    dataset: {},
    _attrs: {},
    classList: { add() {}, remove() {}, toggle() {}, contains() { return false; } },
    setAttribute(k, v) { this._attrs[k] = v; },
    getAttribute(k) { return this._attrs[k] ?? null; },
    removeAttribute(k) { delete this._attrs[k]; },
    appendChild() {},
    focus() {},
    select() {},
    addEventListener(ev, fn) { (handlers[ev] = handlers[ev] || []).push(fn); },
    _fire(ev) { (handlers[ev] || []).forEach((fn) => fn({})); },
    querySelectorAll() { return []; },
  };
}

const nodes = {};
function getNode(id) {
  if (!nodes[id]) nodes[id] = makeNode(id);
  return nodes[id];
}

const documentStub = {
  addEventListener() {},
  getElementById: (id) => getNode(id),
  querySelectorAll: () => [],
  querySelector: () => null,
  createElement: () => makeNode("created"),
  body: makeNode("body"),
};

const ctx = {
  document: documentStub,
  window: {},
  console,
  setTimeout,
  clearTimeout,
  setInterval: () => 0,
  clearInterval: () => {},
  fetch: () => Promise.reject(new Error("no network in verify")),
  AbortController: function () { this.signal = {}; this.abort = () => {}; },
  Math,
  Number,
  Array,
  JSON,
  parseInt,
  parseFloat,
  isNaN,
};
vm.createContext(ctx);
vm.runInContext(src, ctx);

// --- assertions -------------------------------------------------------------
let failures = 0;
function check(cond, msg) {
  if (!cond) { failures++; console.error("FAIL:", msg); }
  else console.log("ok  :", msg);
}

function analyzeSvg(html, expectLines, expectPoints, label) {
  const polylines = [...html.matchAll(/<polyline points="([^"]+)"/g)].map((m) => m[1]);
  check(polylines.length === expectLines, `${label}: ${expectLines} polylines (got ${polylines.length})`);
  let allInBounds = true;
  let pointCountOk = true;
  for (const p of polylines) {
    const coords = p.trim().split(/\s+/);
    if (coords.length !== expectPoints) pointCountOk = false;
    for (const c of coords) {
      const [x, y] = c.split(",").map(Number);
      if (!(x >= 0 && x <= 320 && y >= 0 && y <= 200) || Number.isNaN(x) || Number.isNaN(y)) allInBounds = false;
    }
  }
  check(pointCountOk, `${label}: every line has ${expectPoints} points`);
  check(allInBounds, `${label}: all coordinates inside 320x200 viewBox`);
  check(/<text[^>]*>0<\/text>/.test(html) && /<text[^>]*>100<\/text>/.test(html), `${label}: Y axis has 0 and 100 labels`);
}

// speed view is the default. initTuneChart renders it.
ctx.initTuneChart();
const svg = getNode("tuneChartSvg");
analyzeSvg(svg.innerHTML, 7, 7, "speed-view"); // 7 throttle bands, 7 speed points

const legendScale = getNode("chartLegendScale");
check((legendScale.innerHTML.match(/<span>/g) || []).length === 7, "speed-view: legend has 7 series values");
check(typeof getNode("chartLegendBar").style.background === "string" && getNode("chartLegendBar").style.background.includes("linear-gradient"), "speed-view: legend gradient set");
check(getNode("chartLegendCaption").textContent === "Throttle %", "speed-view: legend caption = Throttle %");

// toggle to throttle view via the real button handler
getNode("chartViewThrottle")._fire("click");
analyzeSvg(svg.innerHTML, 7, 7, "throttle-view"); // 7 speed bands, 7 throttle points
check(getNode("chartLegendCaption").textContent === "Speed (km/h)", "throttle-view: legend caption = Speed (km/h)");
check(getNode("chartViewThrottle").getAttribute("aria-selected") === "true", "throttle-view: aria-selected updated");

// non-ascending axis: a mid-edit out-of-order value must still render fully
// in-bounds (the firmware rejects such a tune on upload, but the chart redraws
// live while editing). 7 points so line/point counts are unchanged.
ctx.speedHeader = [0, 90, 30, 120, 60, 180, 160];
getNode("chartViewSpeed")._fire("click");
analyzeSvg(svg.innerHTML, 7, 7, "non-ascending speed axis");

// degenerate guard: a single-column axis must not throw and must clear the svg
ctx.speedHeader = [0];
getNode("chartViewSpeed")._fire("click");
check(svg.innerHTML === "", "degenerate speed axis (1 point): svg cleared, no throw");

console.log(failures === 0 ? "\nALL CHART CHECKS PASSED" : `\n${failures} CHECK(S) FAILED`);
process.exit(failures === 0 ? 0 : 1);
