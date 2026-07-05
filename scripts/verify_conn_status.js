#!/usr/bin/env node
// Headless verification of the dashboard connection-status badge. Loads the real
// data/app.js in a vm context with a DOM stub, drives the actual refreshStatus()
// with a controllable fetch, and asserts the #connStatus badge transitions
// Live -> Reconnecting -> Offline as polls fail and recovers to Live on the next
// good poll. No browser required.
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const src = fs.readFileSync(path.join(__dirname, "..", "data", "app.js"), "utf8");

// --- DOM stub (tracks class state so the badge state is observable) ---------
function makeNode(id) {
  const handlers = {};
  const classes = new Set();
  return {
    id,
    innerHTML: "",
    textContent: "",
    value: "",
    style: {},
    checked: false,
    dataset: {},
    _attrs: {},
    _classes: classes,
    classList: {
      add: (...c) => c.forEach((x) => classes.add(x)),
      remove: (...c) => c.forEach((x) => classes.delete(x)),
      toggle() {},
      contains: (x) => classes.has(x),
    },
    setAttribute(k, v) { this._attrs[k] = v; },
    getAttribute(k) { return this._attrs[k] ?? null; },
    removeAttribute(k) { delete this._attrs[k]; },
    appendChild() {},
    focus() {},
    select() {},
    addEventListener(ev, fn) { (handlers[ev] = handlers[ev] || []).push(fn); },
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

// Controllable fetch: tests set ctx._fetchImpl before each refreshStatus() call.
const ctx = {
  document: documentStub,
  window: {},
  console,
  setTimeout,
  clearTimeout,
  setInterval: () => 0,
  clearInterval: () => {},
  Date,
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

const fail = () => { ctx._fetchImpl = () => Promise.reject(new Error("network down")); };
const ok = (payload) => { ctx._fetchImpl = () => Promise.resolve({ json: () => Promise.resolve(payload) }); };

const badge = getNode("connStatus");

async function poll() { await ctx.refreshStatus(); }

(async () => {
  // A dropped link: first miss warns, and it stays warning until the offline
  // threshold, then flips to Offline.
  fail();
  await poll();
  check(badge.textContent === "Reconnecting…" && badge._classes.has("stale"),
    `first missed poll -> Reconnecting (got "${badge.textContent}")`);

  await poll();
  check(badge.textContent === "Reconnecting…",
    `second missed poll -> still Reconnecting (got "${badge.textContent}")`);

  await poll();
  check(badge.textContent === "Offline" && badge._classes.has("error"),
    `third missed poll -> Offline (got "${badge.textContent}")`);

  // Recovery: a good poll clears the state back to Live.
  ok({ mode: 0, chassisCAN: true, haldexCAN: true });
  await poll();
  check(badge.textContent === "Live" && badge._classes.has("connected") && !badge._classes.has("error"),
    `good poll after outage -> Live (got "${badge.textContent}")`);

  // The miss counter reset, so a single new miss warns again rather than
  // jumping straight back to Offline.
  fail();
  await poll();
  check(badge.textContent === "Reconnecting…",
    `single miss after recovery -> Reconnecting, not Offline (counter reset) (got "${badge.textContent}")`);

  console.log(failures === 0 ? "\nALL CONNECTION-STATUS CHECKS PASSED" : `\n${failures} CHECK(S) FAILED`);
  process.exit(failures === 0 ? 0 : 1);
})();
