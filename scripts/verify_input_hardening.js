#!/usr/bin/env node
// Headless regression guard for the dashboard input-hardening batch:
//   1. Mode dirty-tracking - a stale in-flight poll must not snap the mode
//      highlight back after the user just picked a mode; the device echo (or a
//      timeout) releases the hold. External changes still apply when nothing is
//      pending. (Reported: "didn't respect which drive mode".)
//   2. Scroll-tap guard - a click on a .toggle that was preceded by a finger
//      move beyond tolerance is swallowed (preventDefault), so a scroll-flick
//      can't flip a switch. A stationary tap is left alone.
//   3. Confirm-on-enable - switching a high-consequence toggle INTO its
//      disruptive state prompts; a declined confirm reverts the checkbox and
//      does not persist; an accepted confirm persists.
// Loads the real data/app.js in a vm context with a DOM + clock stub and wires
// the actual handlers - no logic is re-implemented here.
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const src = fs.readFileSync(path.join(__dirname, "..", "data", "app.js"), "utf8");

let clock = 1_000_000;

function makeNode(id) {
  const handlers = {};
  const classes = new Set();
  return {
    id, innerHTML: "", textContent: "", value: "", style: {}, checked: false,
    disabled: false, dataset: {}, _attrs: {}, _classes: classes,
    classList: {
      add: (...c) => c.forEach((x) => classes.add(x)),
      remove: (...c) => c.forEach((x) => classes.delete(x)),
      toggle(cls, on) {
        const want = on === undefined ? !classes.has(cls) : !!on;
        if (want) classes.add(cls); else classes.delete(cls);
        return want;
      },
      contains: (x) => classes.has(x),
    },
    setAttribute(k, v) { this._attrs[k] = v; },
    getAttribute(k) { return this._attrs[k] ?? null; },
    removeAttribute(k) { delete this._attrs[k]; },
    appendChild() {}, focus() {}, select() {},
    addEventListener(ev, fn) { (handlers[ev] = handlers[ev] || []).push(fn); },
    removeEventListener() {},
    _fire(ev, evt) {
      const e = evt || {};
      let stopped = false;
      const orig = e.stopImmediatePropagation;
      e.stopImmediatePropagation = () => { stopped = true; if (orig) orig(); };
      // Registration order stands in for capture-before-bubble: the guard's
      // capture-phase listener is registered before any mock app listener, so
      // stopImmediatePropagation() here halts the app handler just as it would
      // in the browser.
      for (const fn of (handlers[ev] || [])) { fn(e); if (stopped) break; }
    },
    querySelectorAll() { return []; },
  };
}

const nodes = {};
function getNode(id) { if (!nodes[id]) nodes[id] = makeNode(id); return nodes[id]; }

// Mode buttons the real modeButton()/initModeButtons() resolve via ".mode-btn".
const modeButtons = [0, 1, 2, 3, 4, 5].map((m) => {
  const b = makeNode(`mode-btn-${m}`);
  b.dataset.mode = String(m);
  b.classList.add("mode-btn");
  return b;
});
function activeMode() {
  const on = modeButtons.filter((b) => b.classList.contains("active"));
  return on.length === 1 ? parseInt(on[0].dataset.mode, 10) : null;
}

// A single .toggle label for the scroll-tap guard test.
const toggleNode = makeNode("toggle-label");

// A .slider for the track-tap guard test. It sits at x=[0,200], min 0 / max 100,
// value 50 -> thumb centred at x=100 (accounting for the 24px thumb inset the
// centre is at 12 + 0.5*(200-24) = 100). A press on the bare track (x=180) must
// be cancelled; a press on the thumb (x=100) must be left alone.
const sliderNode = makeNode("slider-node");
sliderNode.min = "0";
sliderNode.max = "100";
sliderNode.value = "50";
sliderNode.getBoundingClientRect = () => ({ left: 0, width: 200 });

const documentStub = {
  addEventListener() {}, removeEventListener() {},
  getElementById: (id) => getNode(id),
  querySelectorAll: (sel) => {
    if (sel === ".mode-btn") return modeButtons;
    if (sel === ".toggle") return [toggleNode];
    if (sel === ".slider") return [sliderNode];
    return [];
  },
  querySelector: () => null,
  createElement: () => makeNode("created"),
  body: makeNode("body"),
  hidden: false,
};

// Controllable device snapshot the poll's fetch resolves to.
let dashData = { mode: 1 };
const fetchCalls = [];
let confirmReturn = true;

const ctx = {
  document: documentStub,
  window: {}, console, navigator: {},
  setTimeout: (fn) => { return 0; },
  clearTimeout: () => {},
  setInterval: () => 0, clearInterval: () => {},
  Date: { now: () => clock },
  confirm: () => confirmReturn,
  fetch: (url, options) => {
    fetchCalls.push({ url, body: options && options.body });
    return Promise.resolve({ ok: true, json: () => Promise.resolve(dashData) });
  },
  AbortController: function () { this.signal = {}; this.abort = () => {}; },
  Math, Number, Array, JSON, parseInt, parseFloat, isNaN, Promise, encodeURIComponent,
};
vm.createContext(ctx);
vm.runInContext(src, ctx);

let failures = 0;
function check(cond, msg) {
  if (!cond) { failures++; console.error("FAIL:", msg); }
  else console.log("ok  :", msg);
}

ctx.initModeButtons();
ctx.initSettings();

async function poll(mode) {
  dashData = Object.assign({}, dashData, { mode });
  getNode("diagFreeHeap").textContent = "__unset__"; // probe: set near end of refreshStatus
  await ctx.refreshStatus();
}

(async () => {
  // --- 1. Mode dirty-tracking -----------------------------------------------
  // Seed the live mode to 1 via a clean poll (nothing pending).
  await poll(1);
  check(getNode("diagFreeHeap").textContent !== "__unset__",
    "refreshStatus ran to completion (mode block reached, not thrown past)");
  check(activeMode() === 1, `baseline poll sets mode 1 (got ${activeMode()})`);

  // User picks mode 2. Highlight moves immediately and the pick is now pending.
  modeButtons[2]._fire("click", {});
  check(activeMode() === 2, `user pick highlights mode 2 (got ${activeMode()})`);
  const sentMode = fetchCalls.some((c) => c.url === "/api/mode");
  check(sentMode, "picking a mode POSTs /api/mode");

  // A stale in-flight poll returns the OLD mode 1. It must NOT snap back to 1.
  await poll(1);
  check(activeMode() === 2,
    `stale poll (mode 1) is ignored while pick is pending (got ${activeMode()})`);

  // Device echoes mode 2. Pending clears; highlight stays 2.
  await poll(2);
  check(activeMode() === 2, `device echo of mode 2 keeps highlight (got ${activeMode()})`);

  // With nothing pending, an external change to mode 1 now applies.
  await poll(1);
  check(activeMode() === 1,
    `external mode change applies once nothing is pending (got ${activeMode()})`);

  // Timeout path: pick mode 2, hold against a stale poll, then let the pending
  // window lapse - the device's mode is trusted again even without an echo.
  modeButtons[2]._fire("click", {});
  check(activeMode() === 2, "re-pick mode 2");
  await poll(1);
  check(activeMode() === 2, `stale poll still held inside the window (got ${activeMode()})`);
  clock += 5000; // exceed PENDING_MODE_TIMEOUT_MS (4000)
  await poll(1);
  check(activeMode() === 1,
    `after the pending window lapses the device mode is applied (got ${activeMode()})`);

  // --- 2. Scroll-tap guard ---------------------------------------------------
  ctx.guardScrollTaps(".toggle");
  let prevented = false;
  const clickEvt = () => ({ preventDefault: () => { prevented = true; }, stopPropagation() {} });

  // A finger that moves well beyond tolerance then "clicks" = a scroll: swallow.
  prevented = false;
  toggleNode._fire("touchstart", { touches: [{ clientX: 100, clientY: 100 }] });
  toggleNode._fire("touchmove", { touches: [{ clientX: 100, clientY: 160 }] });
  toggleNode._fire("click", clickEvt());
  check(prevented, "a click after a >tolerance finger move is swallowed (scroll, not tap)");

  // A stationary tap is left alone.
  prevented = false;
  toggleNode._fire("touchstart", { touches: [{ clientX: 100, clientY: 100 }] });
  toggleNode._fire("touchmove", { touches: [{ clientX: 102, clientY: 101 }] });
  toggleNode._fire("click", clickEvt());
  check(!prevented, "a stationary tap is honoured (not swallowed)");

  // --- 3. Confirm-on-enable --------------------------------------------------
  // Decline: enabling disableController with confirm=false reverts and no POST.
  const dc = getNode("disableController");
  fetchCalls.length = 0;
  confirmReturn = false;
  dc.checked = true;
  dc._fire("change", {});
  check(dc.checked === false, "declined confirm reverts the toggle to off");
  check(!fetchCalls.some((c) => c.url === "/api/settings"),
    "declined confirm does not persist the change");

  // Accept: enabling with confirm=true persists.
  fetchCalls.length = 0;
  confirmReturn = true;
  dc.checked = true;
  dc._fire("change", {});
  check(dc.checked === true, "accepted confirm keeps the toggle on");
  check(fetchCalls.some((c) => c.url === "/api/settings" && /disableController/.test(c.body || "")),
    "accepted confirm persists disableController via /api/settings");

  // isStandalone goes through the SAME generic checkbox handler as
  // disableController - confirm it is equally gated (decline reverts + no POST).
  const sa = getNode("isStandalone");
  fetchCalls.length = 0;
  confirmReturn = false;
  sa.checked = true;
  sa._fire("change", {});
  check(sa.checked === false, "declined confirm reverts isStandalone to off");
  check(!fetchCalls.some((c) => c.url === "/api/settings"),
    "declined confirm does not persist isStandalone");

  fetchCalls.length = 0;
  confirmReturn = true;
  sa.checked = true;
  sa._fire("change", {});
  check(sa.checked === true, "accepted confirm keeps isStandalone on");
  check(fetchCalls.some((c) => c.url === "/api/settings" && /isStandalone/.test(c.body || "")),
    "accepted confirm persists isStandalone via /api/settings");

  // analyzerMode is wired by its OWN handler (mutually-exclusive with
  // analyzerSerial), not the generic loop - verify its confirm gate separately.
  const am = getNode("analyzerMode");
  fetchCalls.length = 0;
  confirmReturn = false;
  am.checked = true;
  am._fire("change", {});
  check(am.checked === false, "declined confirm reverts analyzerMode to off");
  check(!fetchCalls.some((c) => c.url === "/api/settings"),
    "declined confirm does not persist analyzerMode");

  fetchCalls.length = 0;
  confirmReturn = true;
  am.checked = true;
  am._fire("change", {});
  check(am.checked === true, "accepted confirm keeps analyzerMode on");
  check(fetchCalls.some((c) => c.url === "/api/settings" && /analyzerMode/.test(c.body || "")),
    "accepted confirm persists analyzerMode via /api/settings");

  // --- 4. Slider touch-action regression ------------------------------------
  // The scroll-tap fix also depends on both slider rules declaring
  // `touch-action: pan-y` so a vertical drag scrolls instead of dragging the
  // thumb. Assert against the real stylesheet so a dropped rule is caught here.
  const css = fs.readFileSync(path.join(__dirname, "..", "data", "style.css"), "utf8");
  const hasPanY = (selector) => {
    const m = new RegExp(`\\${selector}\\s*\\{[^}]*\\}`).exec(css);
    return m ? /touch-action\s*:\s*pan-y/.test(m[0]) : false;
  };
  check(hasPanY(".slider"), ".slider declares touch-action: pan-y");
  check(hasPanY(".slider-inline"), ".slider-inline declares touch-action: pan-y");

  // --- 5. Slider track-tap guard --------------------------------------------
  // A native range input jumps its value to wherever the track is pressed, and
  // mobile browsers ignore preventDefault() for that jump. So the guard's real
  // contract is: after an off-thumb press, the value change the browser produces
  // is UNDONE and SWALLOWED before the app's save handler runs; an on-thumb
  // press is left alone. sliderNode: x=[0,200], value 50 -> thumb centred at
  // x=100, hit radius 24/2 + 12 = 24, so x=180 is off-thumb and x=100 is on it.
  ctx.guardTrackTaps(".slider", 24); // registers the guard's capture listeners first

  // Mock the app's save handler (bubble phase) registered AFTER the guard, so it
  // only runs if the guard didn't stopImmediatePropagation.
  let appSaw = null;
  sliderNode.addEventListener("input", () => { appSaw = sliderNode.value; });

  const pointerEvt = (x) => ({ clientX: x, preventDefault() {} });
  // Simulate the browser's jump-to-tap: value moves, then an input event fires.
  const fireJump = (toValue) => {
    sliderNode.value = String(toValue);
    sliderNode._fire("input", {});
  };

  // Off-thumb press at x=180: the browser jumps the value to (say) 90; the guard
  // must snap it back to the pre-press 50 and keep the app from saving 90.
  sliderNode.value = "50";
  appSaw = null;
  sliderNode._fire("pointerdown", pointerEvt(180));
  fireJump(90);
  check(sliderNode.value === "50",
    `off-thumb press value is reverted (got ${sliderNode.value}, want 50)`);
  check(appSaw === null, "off-thumb press does not reach the app save handler");
  sliderNode._fire("pointerup", {});

  // On-thumb press at x=100: a genuine drag to 60 must pass straight through.
  sliderNode.value = "50";
  appSaw = null;
  sliderNode._fire("pointerdown", pointerEvt(100));
  fireJump(60);
  check(sliderNode.value === "60",
    `on-thumb drag changes the value (got ${sliderNode.value}, want 60)`);
  check(appSaw === "60", "on-thumb drag reaches the app save handler");
  sliderNode._fire("pointerup", {});

  console.log(failures === 0 ? "\nALL INPUT-HARDENING CHECKS PASSED" : `\n${failures} CHECK(S) FAILED`);
  process.exit(failures === 0 ? 0 : 1);
})();
