#!/usr/bin/env node
// Headless verification of the Learn Haldex calibration chart. Loads the real
// data/app.js in a vm context with a DOM stub, drives the actual
// renderLearnChart(), and asserts the #learnChartSvg content: one learned curve
// with a vertex per table entry, a dashed 1:1 reference diagonal, all coordinates
// inside the viewBox, Y-axis labels, and that an empty/short table hides the
// chart wrapper instead of drawing a broken axis. No browser required.
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

const ctx = {
  document: documentStub,
  window: {}, console, setTimeout, clearTimeout,
  setInterval: () => 0, clearInterval: () => {},
  Date: { now: () => 1_000_000 },
  fetch: () => Promise.reject(new Error("no network in verify")),
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

const svg = getNode("learnChartSvg");
const wrap = getNode("learnChartWrap");

// Geometry mirrors renderLearnChart (viewBox 320x200).
const W = 320, H = 200, padL = 26, padR = 8, padT = 8, padB = 20;
const plotW = W - padL - padR, plotH = H - padT - padB;
const xOf = (cf) => padL + (cf / 100) * plotW;
const yOf = (eng) => padT + (1 - eng / 100) * plotH;

function curveVerts() {
  const m = /<polyline\b[^>]*\bpoints="([^"]*)"/.exec(svg.innerHTML);
  if (!m) return [];
  return m[1].trim().split(/\s+/).filter(Boolean).map((p) => p.split(",").map(Number));
}
function count(tag) {
  return (svg.innerHTML.match(new RegExp(`<${tag}\\b`, "g")) || []).length;
}

// 1) A full 101-point 1:1 table (measured == commanded) draws the curve on the
//    reference diagonal, one vertex per entry, all in-bounds.
const linearTable = Array.from({ length: 101 }, (_, i) => i);
ctx.renderLearnChart(linearTable);

check(wrap.style.display === "", "valid table shows the chart wrapper");
check(count("polyline") === 1, `one learned-curve polyline (got ${count("polyline")})`);

const verts = curveVerts();
check(verts.length === 101, `curve has one vertex per table entry (got ${verts.length}/101)`);

let inBounds = true;
for (const [x, y] of verts) {
  if (!(x >= 0 && x <= W && y >= 0 && y <= H) || Number.isNaN(x) || Number.isNaN(y)) inBounds = false;
}
check(inBounds, "all curve coordinates inside the 320x200 viewBox");

check(Math.abs(verts[0][0] - xOf(0)) < 0.6 && Math.abs(verts[0][1] - yOf(0)) < 0.6,
  `CF 0 -> engagement 0 maps to bottom-left (got ${verts[0]})`);
check(Math.abs(verts[100][0] - xOf(100)) < 0.6 && Math.abs(verts[100][1] - yOf(100)) < 0.6,
  `CF 100 -> engagement 100 maps to top-right (got ${verts[100]})`);
// Mid point on a 1:1 table sits on the diagonal.
check(Math.abs(verts[50][1] - yOf(50)) < 0.6,
  `CF 50 -> engagement 50 on the diagonal (y≈${yOf(50).toFixed(1)}, got ${verts[50][1]})`);

check(/stroke-dasharray/.test(svg.innerHTML), "1:1 reference diagonal drawn as a dashed line");
check(/<text[^>]*>0<\/text>/.test(svg.innerHTML) && /<text[^>]*>100<\/text>/.test(svg.innerHTML),
  "axis has 0 and 100 tick labels");

// 2) An under-responding Haldex (measured = commanded/2) plots below the diagonal:
//    at CF 100 the curve sits lower on screen (larger y) than the diagonal end.
const underTable = Array.from({ length: 101 }, (_, i) => Math.round(i / 2));
ctx.renderLearnChart(underTable);
const underVerts = curveVerts();
check(underVerts[100][1] > yOf(100) + 5,
  `under-responding curve plots below the 1:1 line at full CF (y ${underVerts[100][1]} > ${yOf(100)})`);

// 3) An empty table hides the chart wrapper and clears the svg rather than
//    drawing a broken single-point axis.
ctx.renderLearnChart([]);
check(wrap.style.display === "none" && svg.innerHTML === "",
  "empty table hides the wrapper and clears the svg");

// 4) A degenerate single-point table is treated the same (needs >=2 points).
ctx.renderLearnChart([42]);
check(wrap.style.display === "none" && svg.innerHTML === "",
  "single-point table hides the chart (no throw)");

// 5) Non-array / missing table is tolerated (no throw, chart hidden).
ctx.renderLearnChart(undefined);
check(wrap.style.display === "none", "undefined table hides the chart without throwing");

console.log(failures === 0 ? "\nALL LEARN-CHART CHECKS PASSED" : `\n${failures} CHECK(S) FAILED`);
process.exit(failures === 0 ? 0 : 1);
