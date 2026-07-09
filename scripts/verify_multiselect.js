#!/usr/bin/env node
// Headless regression guard for the Expert-editor multi-select long-press bug.
// Loads the real data/app.js in a vm context with a DOM + timer + clock stub,
// wires the actual initTuneSelection() handlers, and reproduces the reported
// failure: a long-press enters select mode and arms the ghost-click guard, but
// the browser fires `contextmenu` (not `click`), so the trailing click never
// arrives. The NEXT deliberate tap must still register. The guard is armed with
// an expiry, so a tap after the window toggles its cell instead of being eaten.
// selCount.textContent (driven by the real updateSelectionToolbar) is the probe.
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const src = fs.readFileSync(path.join(__dirname, "..", "data", "app.js"), "utf8");

let clock = 1_000_000;
const timeouts = [];

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
    removeEventListener() {},
    _fire(ev, evt) { (handlers[ev] || []).forEach((fn) => fn(evt || {})); },
    querySelectorAll() { return []; },
  };
}

// A lock cell the handlers can resolve via getCellRC (needs the map-cell class
// plus speed/throttle attributes).
function makeCell(speed, throttle) {
  const n = makeNode(`cell-${speed}-${throttle}`);
  n.classList.add("map-cell");
  n.setAttribute("speed", String(speed));
  n.setAttribute("throttle", String(throttle));
  return n;
}

const nodes = {};
function getNode(id) { if (!nodes[id]) nodes[id] = makeNode(id); return nodes[id]; }

const documentStub = {
  addEventListener() {}, removeEventListener() {},
  getElementById: (id) => getNode(id),
  querySelectorAll: () => [],
  querySelector: () => null,
  createElement: () => makeNode("created"),
  body: makeNode("body"),
};

const ctx = {
  document: documentStub,
  window: {}, console,
  setTimeout: (fn) => { timeouts.push(fn); return timeouts.length; },
  clearTimeout: () => {},
  setInterval: () => 0, clearInterval: () => {},
  Date: { now: () => clock },
  fetch: () => Promise.reject(new Error("no network in verify")),
  AbortController: function () { this.signal = {}; this.abort = () => {}; },
  Math, Number, Array, JSON, parseInt, parseFloat, isNaN,
};
vm.createContext(ctx);
vm.runInContext(src, ctx);

let failures = 0;
function check(cond, msg) {
  if (!cond) { failures++; console.error("FAIL:", msg); }
  else console.log("ok  :", msg);
}

const grid = getNode("mapGrid");
const selCount = getNode("selCount");
ctx.initTuneSelection();

function firePendingLongPress() {
  // The only setTimeout the handlers arm is the long-press timer; fire it.
  const fn = timeouts.pop();
  if (fn) fn();
}
function selectedCount() {
  const m = /^(\d+)/.exec(selCount.textContent || "");
  return m ? parseInt(m[1], 10) : 0;
}

// --- Scenario 1: the reported bug (no trailing ghost click) -----------------
// Long-press cellA -> select mode + cellA seeded (1). No click follows (browser
// fired contextmenu). Time passes. A deliberate tap on cellB must select it (2).
const cellA = makeCell(1, 1);
const cellB = makeCell(2, 2);

timeouts.length = 0;
grid._fire("pointerdown", { target: cellA, clientX: 10, clientY: 10 });
firePendingLongPress();
check(selectedCount() === 1, `long-press seeds the held cell (got ${selectedCount()})`);

clock += 500; // ghost click never came; the guard window has lapsed
grid._fire("click", { target: cellB });
check(selectedCount() === 2,
  `next tap after a lapsed guard still selects its cell (got ${selectedCount()}, want 2)`);

// --- Scenario 2: the ghost click IS swallowed when it arrives in-window ------
// A fresh long-press seeds one cell; the browser's trailing ghost click lands
// immediately (same instant) and must be eaten, not toggle a second cell.
ctx.setSelectMode(false); // reset selection/mode
timeouts.length = 0;
grid._fire("pointerdown", { target: cellA, clientX: 10, clientY: 10 });
firePendingLongPress();
check(selectedCount() === 1, `re-seed after reset (got ${selectedCount()})`);

// Ghost click at the same clock tick (within the 400ms window) is swallowed.
grid._fire("click", { target: cellB });
check(selectedCount() === 1,
  `in-window ghost click is swallowed, not a real tap (got ${selectedCount()}, want 1)`);

// A genuine tap right after is then honoured (guard already consumed).
grid._fire("click", { target: cellB });
check(selectedCount() === 2,
  `the tap after the swallowed ghost registers (got ${selectedCount()}, want 2)`);

console.log(failures === 0 ? "\nALL MULTI-SELECT CHECKS PASSED" : `\n${failures} CHECK(S) FAILED`);
process.exit(failures === 0 ? 0 : 1);
