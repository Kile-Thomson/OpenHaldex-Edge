let currentEditCell = null; // global flag for current editted cell

const MODE_NAMES = ["Stock", "FWD", "50:50", "60:40", "75:25", "Expert"]; // mode names as Strings

// Cached settings for banner force-mode display and state legend (loaded once from /api/settings)
let _tcForceModeValue     = 2;
let _hazardForceModeValue = 2;
let _extBtnForceModeValue = 2;
let _tcForceMode = false;
let _hazardForceMode = false;
let _extBtnForceMode = false;
let _haldexGeneration = 1;
let _isStandalone = false;
let _useCANifAvailable = false;
let _disableController = false;
// Floor gap between the end of one dashboard poll and the start of the next.
// The poll loop is self-scheduling (see initDashboard): it fires the next
// request as soon as the previous one settles, so the live rate tracks the
// device's real response latency instead of being quantised to a fixed tick.
// A small floor keeps a fast device from being hammered while still letting the
// UI run several Hz when the ESP answers quickly.
const POLL_MIN_GAP_MS = 150;

// Optimistic mode tracking. The dashboard poll re-applies modeButton(data.mode)
// every cycle so an externally-driven mode change (button, force trigger) shows
// up. But right after the user picks a mode, a poll whose snapshot was taken
// before the device processed the change would snap the highlight back to the
// old mode - the "didn't respect which drive mode" report. While a change is
// pending we hold the user's selection until the device echoes it back (or a
// timeout elapses, so an ignored/failed change can't wedge the UI forever).
let _pendingMode = null;
let _pendingModeTs = 0;
const PENDING_MODE_TIMEOUT_MS = 4000;

// Reject taps that are really the tail of a scroll. On the phone a finger that
// lands on a toggle while flicking the page up/down fires a click on touchend,
// silently flipping high-consequence switches (Standalone, Disable Controller).
// If the finger moved more than this many pixels between touchstart and the
// click, we swallow the click so only a deliberate stationary tap toggles.
const TAP_MOVE_TOLERANCE_PX = 12;
function guardScrollTaps(selector) {
  document.querySelectorAll(selector).forEach((el) => {
    let startX = 0;
    let startY = 0;
    let moved = false;
    el.addEventListener("touchstart", (e) => {
      const t = e.touches[0];
      startX = t.clientX;
      startY = t.clientY;
      moved = false;
    }, { passive: true });
    el.addEventListener("touchmove", (e) => {
      const t = e.touches[0];
      if (Math.abs(t.clientX - startX) > TAP_MOVE_TOLERANCE_PX ||
          Math.abs(t.clientY - startY) > TAP_MOVE_TOLERANCE_PX) {
        moved = true;
      }
    }, { passive: true });
    el.addEventListener("click", (e) => {
      if (moved) {
        // It was a scroll, not a tap: cancel the label's default control toggle.
        e.preventDefault();
        e.stopPropagation();
        moved = false;
      }
    });
  });
}

// Require sliders to be grabbed by the thumb, not set by tapping the track. A
// native <input type="range"> jumps its value to wherever you press on the bar,
// so a stray tap (or a fat-finger meant for something else) can slam a setting
// to a random value. Mobile browsers ignore preventDefault() on the range
// input's pointerdown for this jump, so asking the browser not to move doesn't
// work. Instead we let the browser do whatever it wants, then FORCIBLY reject
// the change: on a press that doesn't land on the thumb we restore the value the
// slider had before the press and stop the event so the app's save handlers
// never see the jumped value. The value physically cannot change from an
// off-thumb press. thumbPx is the rendered thumb diameter; a finger-slop margin
// is added so the thumb stays easy to grab.
const SLIDER_GRAB_SLOP_PX = 12;
function guardTrackTaps(selector, thumbPx) {
  const hitRadius = thumbPx / 2 + SLIDER_GRAB_SLOP_PX;
  document.querySelectorAll(selector).forEach((el) => {
    // While `blocked` is true, every value change the slider produces is undone
    // and swallowed. It is (re)decided at the start of each press by hit-testing
    // the press position against the thumb's current position.
    let blocked = false;
    let heldValue = el.value; // the value to snap back to when a press is blocked

    const decideBlock = (clientX) => {
      const min = parseFloat(el.min) || 0;
      const max = parseFloat(el.max);
      const rect = el.getBoundingClientRect();
      if (!Number.isFinite(max) || max <= min || !rect.width) {
        blocked = false; // can't hit-test - allow the interaction
        return;
      }
      heldValue = el.value;
      const frac = (parseFloat(el.value) - min) / (max - min);
      // The thumb centre travels inset by half its width at each end.
      const thumbCenterX = rect.left + thumbPx / 2 + frac * (rect.width - thumbPx);
      blocked = Math.abs(clientX - thumbCenterX) > hitRadius;
    };

    el.addEventListener("pointerdown", (e) => {
      decideBlock(e.clientX);
      if (blocked) e.preventDefault(); // best-effort; browsers may ignore it
    });
    // Touch fallback for browsers that don't emit pointer events on the range.
    el.addEventListener("touchstart", (e) => {
      if (e.touches && e.touches[0]) decideBlock(e.touches[0].clientX);
    }, { passive: true });

    // Capture phase so this beats the app's own input/change save listeners
    // (added on the bubble phase in initSettings) - stopImmediatePropagation
    // then prevents them from firing with the jumped value.
    const reject = (e) => {
      if (!blocked) return;
      el.value = heldValue; // undo the jump-to-tap
      e.stopImmediatePropagation();
    };
    el.addEventListener("input", reject, true);
    el.addEventListener("change", reject, true);

    // A completed press clears the block so a later thumb grab or keyboard
    // adjustment isn't wrongly suppressed. The next press re-hit-tests.
    const release = () => { blocked = false; };
    el.addEventListener("pointerup", release);
    el.addEventListener("pointercancel", release);
    el.addEventListener("touchend", release);
    el.addEventListener("touchcancel", release);
  });
}

var speedHeader = [0, 30, 60, 90, 120, 160, 180]; // default speed header (for x-axis)
var throttleHeader = [0, 15, 30, 45, 60, 75, 90]; // default throttle header (for y-axis)

const arrayColumns = speedHeader.length; // var for number of columns
const arrayRows = throttleHeader.length; // var for number of rows

var defaultSpeedHeader = [0, 30, 60, 90, 120, 160, 180]; // default speed header (for x-axis)
var defaultThrottleHeader = [0, 15, 30, 45, 60, 75, 90]; // default throttle header (for y-axis)

// Default lock table for "Restore Defaults". This MUST mirror the firmware's
// compiled-in default (lockArray in src/OpenHaldexC6_globals.cpp) so restoring
// defaults in the UI reproduces exactly what the device ships with on a fresh
// NVS. Rows = throttle (0..90), columns = speed (0..180).
const defaultLock = [
  [0, 0, 0, 0, 0, 0, 0],
  [0, 0, 0, 0, 0, 0, 0],
  [5, 5, 5, 5, 5, 40, 40],
  [40, 40, 40, 40, 40, 40, 40],
  [80, 80, 80, 80, 80, 80, 80],
  [80, 80, 80, 80, 80, 80, 80],
  [80, 80, 80, 80, 80, 80, 80],
];

// variable for current lock (to be used / traced)
var currentLock = [
  [0, 0, 0, 0, 0, 0, 0],
  [0, 0, 0, 0, 0, 0, 0],
  [0, 0, 5, 5, 5, 5, 5],
  [0, 5, 10, 80, 80, 80, 80],
  [10, 20, 30, 40, 40, 40, 40],
  [10, 20, 30, 40, 40, 40, 40],
  [10, 20, 30, 40, 40, 40, 40],
];

// once the document is loaded, start running the script - start with getting settings
// await doesn't seem to work well - so if settings aren't captured first, the page draws WHILE settings are being grabbed.
// this is possible a hack, but I can't think of a better way to do it(!)
document.addEventListener("DOMContentLoaded", initStoredSettings);

// Register the service worker so the app installs as a full-screen PWA and can
// still open its shell when the module is briefly unreachable. Service workers
// only run in a secure context (https or localhost); over plain http to the
// module's LAN IP the registration rejects, so guard it and swallow the failure
// rather than throw an uncaught error on every load.
if ("serviceWorker" in navigator && window.isSecureContext) {
  window.addEventListener("load", () => {
    navigator.serviceWorker.register("/sw.js").catch((err) => {
      console.warn("Service worker registration failed:", err);
    });
  });
}

// Header fullscreen toggle. The Fullscreen API works over plain http (unlike
// PWA install, which needs a secure context), so this is the path to a
// full-screen experience on an unmodified phone: one tap hides the browser
// chrome. The button stays hidden where the API doesn't exist (iPhone Safari
// has no page fullscreen).
function initFullscreen() {
  const btn = document.getElementById("fullscreenToggle");
  if (!btn) return;

  const root = document.documentElement;
  const request = root.requestFullscreen || root.webkitRequestFullscreen;
  if (!request) return; // leave the button hidden
  btn.hidden = false;

  const active = () =>
    !!(document.fullscreenElement || document.webkitFullscreenElement);

  const sync = () => {
    const on = active();
    btn.setAttribute("aria-pressed", on ? "true" : "false");
    btn.setAttribute("aria-label", on ? "Exit full screen" : "Enter full screen");
    btn.title = on ? "Exit full screen" : "Full screen";
  };

  btn.addEventListener("click", () => {
    if (active()) {
      (document.exitFullscreen || document.webkitExitFullscreen).call(document);
    } else {
      // navigationUI:"hide" is ignored where unsupported; the promise rejects
      // if the browser refuses (e.g. not a user gesture), which we swallow -
      // the icon simply stays on "enter".
      const p = request.call(root, { navigationUI: "hide" });
      if (p && p.catch) p.catch(() => {});
    }
  });

  document.addEventListener("fullscreenchange", sync);
  document.addEventListener("webkitfullscreenchange", sync);
  sync();
}

// once settings are stored, start applying data where required. Runs once:
// initStoredSettings() is also used to refresh values later (e.g. after
// Forget Paired Phones), and a second pass would bind every listener and
// timer again.
let appInitialized = false;
function initApp() {
  if (appInitialized) return;
  appInitialized = true;
  initNavigation();
  initDashboard();
  initDashTiles();
  initModeDrawer();
  initModeButtons();
  initSettings();
  initExpertEditor();
  initTuneSelection();
  initTuneChart();
  initCalibrate();
  initLearn();
  initLongLearn();
  initFrameEditReset();
  initWifiSsid();
  initWifi();
  initBle();
  initWifiSta("wifiSta"); // Diagnostics
  initWifiSta("otaWifiSta"); // Update tab copy: gets the phone online for the GitHub check
  initBackupRestore();
  initOtaUpdate();
  initUpdateCheck();
  initFullscreen();
  guardScrollTaps(".toggle"); // stop scroll-flicks from flipping toggles
  guardTrackTaps(".slider", 24); // grab the thumb to move; a track tap does nothing
  guardTrackTaps(".slider-inline", 18);
}

// global function for getting data from the ESP
async function fetchJson(url, options) {
  try {
    const request = await fetch(url, options); // request data from ESP
    // Over the home network (bridge mode) the module wants HTTP Basic auth. The
    // browser asks for it on the page load and then reuses it; a 401 here means
    // that never happened (or the password changed), so say so instead of
    // failing silently.
    if (request.status === 401) {
      noteAuthRequired();
      return undefined;
    }
    const result = await request.json(); // wait for response from ESP
    return result;
  } catch (error) {
    console.log("Error:" + error); // Catches and logs any errors
  }
}

// Shown once when the module answers 401 (home-network access needs a login).
function noteAuthRequired() {
  window._authNeeded = true;
  if (document.getElementById("authBanner")) return;
  const bar = document.createElement("div");
  bar.id = "authBanner";
  bar.className = "auth-banner";
  bar.setAttribute("role", "alert");
  bar.innerHTML =
    "<span>Sign-in needed. On the home network the module asks for a login: user <strong>admin</strong>, " +
    "password = your access-point WiFi password.</span>";
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "btn-secondary";
  btn.textContent = "Reload & sign in";
  btn.addEventListener("click", () => location.reload());
  bar.appendChild(btn);
  document.body.appendChild(bar);
}

function clearAuthRequired() {
  if (!window._authNeeded) return;
  window._authNeeded = false;
  const bar = document.getElementById("authBanner");
  if (bar) bar.remove();
}

// initialise stored settings (async function)
async function initStoredSettings() {
  // initialise stored settings and parse them
  try {
    const data = await fetchJson("/api/settings");
    if (!data) { initApp(); return; } // device unreachable - draw the UI with HTML defaults
    // values
    document.getElementById("haldexGeneration").value =
      data.haldexGeneration || 1;
    document.getElementById("tcForceModeValue").value     = data.tcForceModeValue     ?? 2;
    document.getElementById("hazardForceModeValue").value  = data.hazardForceModeValue  ?? 2;
    document.getElementById("extBtnForceModeValue").value  = data.extBtnForceModeValue  ?? 2;

    document.getElementById("disengageUnderSpeedRange").value =
      data.disengageUnderSpeed || 0;
    document.getElementById("disengageUnderSpeed").textContent =
      data.disengageUnderSpeed || 0;

    document.getElementById("disengageAboveSpeedRange").value =
      data.disengageAboveSpeed || 0;
    document.getElementById("disengageAboveSpeed").textContent =
      data.disengageAboveSpeed || 0;

    document.getElementById("disableThrottleRange").value =
      data.disableThrottle || 0;
    document.getElementById("disableThrottle").textContent =
      data.disableThrottle || 0;

    const ledBrightPct = Math.round((data.ledBrightness !== undefined ? data.ledBrightness : 255) / 2.55);
    document.getElementById("ledBrightnessRange").value = ledBrightPct;
    document.getElementById("ledBrightnessValue").textContent = ledBrightPct;

    const lockReleaseRange = document.getElementById("lockReleaseRampRange");
    const lockReleaseVal   = document.getElementById("lockReleaseRampValue");
    if (lockReleaseRange && data.lockReleaseRampMs !== undefined) {
      lockReleaseRange.value = data.lockReleaseRampMs;
      if (lockReleaseVal) lockReleaseVal.textContent = data.lockReleaseRampMs;
    }

    const lockEngageRange = document.getElementById("lockEngageRampRange");
    const lockEngageVal   = document.getElementById("lockEngageRampValue");
    if (lockEngageRange && data.lockEngageRampMs !== undefined) {
      lockEngageRange.value = data.lockEngageRampMs;
      if (lockEngageVal) lockEngageVal.textContent = data.lockEngageRampMs;
    }

    const lockReleaseEnabledElem = document.getElementById("lockReleaseEnabled");
    if (lockReleaseEnabledElem) {
      lockReleaseEnabledElem.checked = data.lockReleaseEnabled !== undefined ? data.lockReleaseEnabled : true;
      const en = lockReleaseEnabledElem.checked;
      const containers = ["lockReleaseRampContainer", "lockEngageRampContainer"];
      containers.forEach((id) => {
        const container = document.getElementById(id);
        if (container) container.style.opacity = en ? "" : "0.4";
      });
      if (lockReleaseRange) lockReleaseRange.disabled = !en;
      if (lockEngageRange) lockEngageRange.disabled = !en;
    }

    // Steering gain taper
    const steerGainSliders = [
      { range: "steeringGainStartRange", value: "steeringGainStartValue", key: "steeringGainStartDeg" },
      { range: "steeringGainFullRange",  value: "steeringGainFullValue",  key: "steeringGainFullDeg" },
      { range: "steeringGainFloorRange", value: "steeringGainFloorValue", key: "steeringGainFloor" },
    ];
    steerGainSliders.forEach(({ range, value, key }) => {
      const rangeElem = document.getElementById(range);
      const valueElem = document.getElementById(value);
      if (rangeElem && data[key] !== undefined) {
        rangeElem.value = data[key];
        if (valueElem) valueElem.textContent = data[key];
      }
    });

    const steerGainEnabledElem = document.getElementById("steeringGainEnabled");
    if (steerGainEnabledElem) {
      steerGainEnabledElem.checked = data.steeringGainEnabled || false;
      const en = steerGainEnabledElem.checked;
      steerGainSliders.forEach(({ range }) => {
        const rangeElem = document.getElementById(range);
        if (rangeElem) rangeElem.disabled = !en;
        const container = document.getElementById(range.replace("Range", "Container"));
        if (container) container.style.opacity = en ? "" : "0.4";
      });
    }

    if (data.forceModesPriority !== undefined) {
      document.getElementById("forceModesPriority").value = data.forceModesPriority;
    }

    document.getElementById("FW_VERSION").textContent = data.FW_VERSION || "--";

    // bools
    document.getElementById("disableController").checked =
      data.disableController || false;
    document.getElementById("isStandalone").checked =
      data.isStandalone || false;
    document.getElementById("useCANifAvailable").checked =
      data.useCANifAvailable || false;
    document.getElementById("tcForceMode").checked = data.tcForceMode || false;
    {
      const extBtnCtrlElem = document.getElementById("extButtonForceMode");
      if (extBtnCtrlElem) {
        extBtnCtrlElem.value = data.extButtonForceMode ? "1" : "0";
        const fmvRow = document.getElementById("extBtnForceModeValueRow");
        if (fmvRow) fmvRow.style.display = data.extButtonForceMode ? "" : "none";
      }
    }
    document.getElementById("hazardForceMode").checked =
      data.hazardForceMode || false;

    document.getElementById("followBrake").checked = data.followBrake || false;
    document.getElementById("invertBrake").checked = data.invertBrake || false;
    document.getElementById("followHandbrake").checked =
      data.followHandbrake || false;
    document.getElementById("invertHandbrake").checked =
      data.invertHandbrake || false;

    document.getElementById("broadcastOpenHaldexOverCAN").checked =
      data.broadcastOpenHaldexOverCAN || false;

    document.getElementById("disableOnboardButton").checked =
      data.disableOnboardButton || false;
    document.getElementById("disableExternalButton").checked =
      data.disableExternalButton || false;

    const bpkCeilRange = document.getElementById("bpkCeilingRange");
    const bpkCeilVal   = document.getElementById("bpkCeilingValue");
    if (bpkCeilRange && data.bpkCeilingNm !== undefined) {
      bpkCeilRange.value = data.bpkCeilingNm;
      if (bpkCeilVal) bpkCeilVal.textContent = data.bpkCeilingNm;
    }

    const esp14FloorRange = document.getElementById("esp14MinFloorRange");
    const esp14FloorVal   = document.getElementById("esp14MinFloorValue");
    if (esp14FloorRange && data.esp14MinFloorPct !== undefined) {
      esp14FloorRange.value = data.esp14MinFloorPct;
      if (esp14FloorVal) esp14FloorVal.textContent = data.esp14MinFloorPct;
    }

    populateGeometry(data);

    const canSleepElem = document.getElementById("canSleepEnabled");
    if (canSleepElem) canSleepElem.checked = data.canSleepEnabled || false;

    const canSleepAggrElem = document.getElementById("canSleepAggressive");
    if (canSleepAggrElem) canSleepAggrElem.checked = data.canSleepAggressive || false;

    // Aggressive implies basic - lock the basic checkbox while aggressive is on.
    if (canSleepElem && canSleepAggrElem) {
      canSleepElem.disabled = canSleepAggrElem.checked;
    }

    const lpWakeRange = document.getElementById("lpWakeThresholdFpsRange");
    const lpWakeVal   = document.getElementById("lpWakeThresholdFpsValue");
    if (lpWakeRange && data.lpWakeThresholdFps !== undefined) {
      lpWakeRange.value = data.lpWakeThresholdFps;
      if (lpWakeVal) lpWakeVal.textContent = data.lpWakeThresholdFps;
    }

    const analyzerModeElem = document.getElementById("analyzerMode");
    if (analyzerModeElem) analyzerModeElem.checked = data.analyzerMode || false;

    const analyzerSerialElem = document.getElementById("analyzerSerial");
    if (analyzerSerialElem) analyzerSerialElem.checked = data.analyzerSerial || false;

    // Live diagnostics (UDS on Gen5, KWP/TP2.0 on Gen2/4). Older firmware only
    // knew udsMQBEnabled for the same switch.
    const liveDiagElem = document.getElementById("liveDiagEnabled");
    if (liveDiagElem) liveDiagElem.checked = (data.liveDiagEnabled ?? data.udsMQBEnabled) || false;

    const dangerZoneElem = document.getElementById("dangerZoneEnabled");
    if (dangerZoneElem) dangerZoneElem.checked = data.dangerZoneEnabled || false;

    const benchModeElem = document.getElementById("benchMode");
    if (benchModeElem) benchModeElem.checked = data.benchMode || false;

    const espNowEnabledElem = document.getElementById("espNowEnabled");
    if (espNowEnabledElem) espNowEnabledElem.checked = data.espNowEnabled !== false;
    const espNowControlElem = document.getElementById("espNowControl");
    if (espNowControlElem) espNowControlElem.checked = data.espNowControl !== false;

    _sleepCalState = data.sleepCalState ?? 0;
    _sleepCalAvgFps = data.sleepCalAvgFps ?? 0;
    renderSleepCal();
    setTimeout(() => maybeOfferSleepSetup(_haldexGeneration), 1000);

    const bleEnabledElem = document.getElementById("bleEnabled");
    if (bleEnabledElem) bleEnabledElem.checked = data.bleEnabled !== undefined ? data.bleEnabled : true;
    if (data.blePasskey !== undefined) {
      blePasskeyCache = data.blePasskey;
      renderBlePairing(data.bleCodeRequired);
    }

    const llNotes = document.getElementById("longLearnNotes");
    if (llNotes && typeof data.longLearnNotes === "string") llNotes.value = data.longLearnNotes;

    const boardRevEl = document.getElementById("BOARD_REV");
    if (boardRevEl) boardRevEl.textContent = data.boardRev ? `rev ${data.boardRev}` : "--";

    // Per-generation frame blocks (Calibrate tab > Frame blocks)
    renderFrameBlocks(data.frameBlocks);

    // cache for banner / legend
    _tcForceModeValue     = data.tcForceModeValue     ?? 2;
    _hazardForceModeValue = data.hazardForceModeValue  ?? 2;
    _extBtnForceModeValue = data.extBtnForceModeValue  ?? 2;
    _tcForceMode = data.tcForceMode || false;
    _hazardForceMode = data.hazardForceMode || false;
    _extBtnForceMode = data.extButtonForceMode || false;
    _haldexGeneration = data.haldexGeneration || 1;
    _isStandalone = data.isStandalone || false;
    _useCANifAvailable = data.useCANifAvailable || false;
    _disableController = data.disableController || false;

    // parse speed/throttle/lock array from the ESP
    speedHeader = data.speedArray;
    throttleHeader = data.throttleArray;
    currentLock = data.lockArray;

    // parse the mode button
    if (data.mode !== undefined) {
      modeButton(data.mode);
    }
  } catch (error) {
    console.log("Status failed: " + error.message);
  }

  initApp(); // settings captured, now start filling up the interface
}

// refresh ongoing data
const POLL_TIMEOUT_MS = 2000; // bound a stalled poll so the in-flight guard can't wedge

// Connection health. The dashboard is poll-based (no WebSocket), so "connected"
// just means recent polls are landing. A dropped AP link in a moving car used to
// leave the last gauge values frozen on screen looking live; now the header badge
// flips to Reconnecting after the first miss and Offline after a few, so stale
// numbers are never mistaken for current ones.
const CONN_OFFLINE_AFTER = 3; // consecutive missed polls before declaring Offline

function setConnStatus(state) {
  const el = document.getElementById("connStatus");
  if (!el) return;
  el.classList.remove("connected", "stale", "error");
  if (state === "offline") {
    el.textContent = window._authNeeded ? "Sign in" : "Offline";
    el.classList.add("error");
  } else if (state === "stale") {
    el.textContent = "Reconnecting...";
    el.classList.add("stale");
  } else {
    el.textContent = "Live";
    el.classList.add("connected");
  }
}

async function refreshStatus() {
  // one poll at a time: on a busy ESP a slow response must not let the 500ms
  // interval stack requests and flood the async web server once it recovers
  if (refreshStatus._inFlight) return;
  refreshStatus._inFlight = true;
  const ctrl = new AbortController();
  const pollTimeout = setTimeout(() => ctrl.abort(), POLL_TIMEOUT_MS);
  try {
    const data = await fetchJson("/api/dashboard", { signal: ctrl.signal }); // send request for basic data
    if (!data) {
      // fetch failed or timed out - count the miss and surface staleness instead
      // of silently freezing the dashboard on the last-known values
      refreshStatus._missCount = (refreshStatus._missCount || 0) + 1;
      const offline = refreshStatus._missCount >= CONN_OFFLINE_AFTER;
      setConnStatus(offline ? "offline" : "stale");
      // Clear the trace once the link is declared dead so the reconnect starts a
      // fresh line instead of bridging the outage with a flat segment.
      if (offline) resetLockTrace();
      return;
    }
    // got data: link is live. Set this before the render block so a later DOM
    // error can't leave the badge stuck on a stale state.
    refreshStatus._missCount = 0;
    clearAuthRequired();
    setConnStatus("connected");

    // Live frame-rate monitor for LP wake threshold tuning
    if (data.lpChassisFrameCount !== undefined && data.lpHaldexFrameCount !== undefined) {
      const now = Date.now();
      if (refreshStatus._lastFrameTime) {
        const dt = (now - refreshStatus._lastFrameTime) / 1000;
        const cFps = Math.round((data.lpChassisFrameCount - refreshStatus._lastChassis) / dt);
        const hFps = Math.round((data.lpHaldexFrameCount  - refreshStatus._lastHaldex)  / dt);
        const cEl = document.getElementById("lpChassisFrameRate");
        const hEl = document.getElementById("lpHaldexFrameRate");
        if (cEl) cEl.textContent = cFps;
        if (hEl) hEl.textContent = hFps;
      }
      refreshStatus._lastFrameTime = now;
      refreshStatus._lastChassis   = data.lpChassisFrameCount;
      refreshStatus._lastHaldex    = data.lpHaldexFrameCount;
    }

    const displayValue = (value) => (value ?? "--");
    const displayOnOff = (value) =>
      value === undefined || value === null ? "--" : value ? "On" : "Off";

    document.getElementById("speed").textContent = displayValue(data.speed);
    document.getElementById("throttle").textContent = displayValue(
      data.throttle,
    );
    document.getElementById("rpm").textContent = displayValue(data.rpm);
    document.getElementById("boost").textContent = displayValue(data.boost);
    document.getElementById("steeringAngle").textContent = displayValue(data.steeringAngle);
    document.getElementById("steeringGainNow").textContent = displayValue(data.steeringGainNow);
    document.getElementById("throttleBar").style.width = `${data.throttle ?? 0}%`;

    document.getElementById("lockTarget").textContent = displayValue(
      data.lockTarget,
    );
    document.getElementById("lockActual").textContent = displayValue(
      data.lockActual,
    );
    updateEngagementGauge(data.lockTarget ?? null, data.lockActual ?? null);

    // Feed the rolling lock-response strip chart from the same poll.
    const traceNow = Date.now();
    pushLockSample(data.lockTarget, data.lockActual, traceNow);
    renderLockTrace(traceNow);

    // Glance card: reported engagement + clutch values, and the status chips
    // (chips light when the ECU reports the flag active, grey out on no data).
    document.getElementById("haldexEngagement").textContent = displayValue(data.haldexEngagement);
    document.getElementById("clutch1Report").textContent   = displayValue(data.clutch1Report);
    document.getElementById("clutch2Report").textContent   = displayValue(data.clutch2Report);
    setChip("chipTempProtection", data.tempProtection);
    setChip("chipCouplingOpen", data.couplingOpen);
    setChip("chipSpeedLimit", data.speedLimit);
    setChip("chipClutch1", data.clutch1Report);
    setChip("chipClutch2", data.clutch2Report);

    if (data.mode !== undefined) {
      // Hold the user's just-picked mode until the device echoes it (or the
      // pending window expires), so a stale in-flight poll can't snap the
      // highlight back. External mode changes still show once nothing is pending.
      if (_pendingMode !== null) {
        if (data.mode === _pendingMode || Date.now() - _pendingModeTs > PENDING_MODE_TIMEOUT_MS) {
          _pendingMode = null;
          modeButton(data.mode);
        }
      } else {
        modeButton(data.mode); // set the mode button - there may be external influences
      }
    }

    const canStatus = document.getElementById("canStatus");
    const chassisOk = data.chassisCAN;
    const haldexOk = data.haldexCAN;
    canStatus.textContent = `CAN C:${chassisOk ? "✓" : "✗"} H:${haldexOk ? "✓" : "✗"}`;

    document.getElementById("diagChassisCAN").textContent = chassisOk
      ? "✓ Healthy"
      : "X Unhealthy";
    document.getElementById("diagHaldexCAN").textContent = haldexOk
      ? "✓ Healthy"
      : "X Unhealthy";
    document.getElementById("diagThrottle").textContent = displayValue(
      data.throttle,
    );
    document.getElementById("diagSpeed").textContent = displayValue(data.speed);
    document.getElementById("diagAsrStatus").textContent = displayOnOff(
      data.asrOn,
    );
    document.getElementById("diagTcStatus").textContent = displayOnOff(
      data.tcOn,
    );
    document.getElementById("diagHazardActive").textContent = displayOnOff(
      data.hazardActive,
    );
    document.getElementById("diagBrakeIn").textContent = displayOnOff(
      data.brakeIn,
    );
    document.getElementById("diagBrakeOut").textContent = displayOnOff(
      data.brakeOut,
    );
    document.getElementById("diagHandbrakeIn").textContent =
      displayOnOff(data.handbrakeIn);
    document.getElementById("diagHandbrakeOut").textContent =
      displayOnOff(data.handbrakeOut);
    document.getElementById("diagBrakeFromCAN").textContent = displayOnOff(
      data.brakeFromCAN,
    );
    document.getElementById("diagHandbrakeFromCAN").textContent = displayOnOff(
      data.handbrakeFromCAN,
    );
    document.getElementById("diagCpuUsage").textContent = displayValue(
      data.cpuUsage,
    );
    document.getElementById("diagFreeHeap").textContent = Math.round(
      data.freeHeap / 1024,
    );

    document.getElementById("haldexState").textContent =
      data.haldexState === undefined || data.haldexState === null
        ? "--"
        : hex2bin(data.haldexState);

    updateBannerSubtitle(data);
    // Gen50 (0CQ/MQB): state byte is Allrad_03 byte 3 (Charisma) - use gen=50 legend.
    // Gen51 (0AY) and all PQ gens (1/2/4): state byte is Allrad_1 byte 0 (PQ fault flags) - use gen-specific PQ legend.
    const legendGen = _haldexGeneration;
    renderHaldexStateLegend(data.haldexState, legendGen);

    // Gen 4.1 (GM/SAAB) specific data
    const gen41Card = document.getElementById("gen41Card");
    if (data.gen41) {
      gen41Card.style.display = "";
      const sa = data.gen41.secAxle;
      const ra = data.gen41.rearAxle;

      document.getElementById("g41SecTorqueNm").textContent = displayValue(sa?.torqueNm);
      document.getElementById("g41SecClutchState").textContent = displayValue(sa?.clutchState);

      document.getElementById("g41RearMetricA").textContent = displayValue(ra?.metricA);
      document.getElementById("g41RearMetricB").textContent = displayValue(ra?.metricB);
    } else {
      gen41Card.style.display = "none";
    }

    // UDS MQB diagnostic data: the API only includes `uds` while the poller
    // toggle is on. The four headline values sit inline in the glance grid; the
    // rest fold into the details block. Both hide when the feature is off.
    const uds = data.uds;
    // Glance tile visibility is user-controlled (see initDashTiles); only the
    // full UDS details block is gated on whether the poller is returning data.
    const udsDetails = document.getElementById("udsDetails");
    if (udsDetails) udsDetails.style.display = uds ? "" : "none";
    const udsStatusEl = document.getElementById("udsStatus");
    if (udsStatusEl) udsStatusEl.textContent = uds && data.diagToolActive ? "Paused: an external diagnostic tool was detected." : "";
    if (uds) {
      document.getElementById("udsTerminalVoltage").textContent = uds.terminalVoltage?.toFixed(1) ?? "--";
      document.getElementById("udsModuleTemp").textContent = uds.moduleTemp?.toFixed(1) ?? "--";
      document.getElementById("udsClutchTemp").textContent = uds.clutchTemp?.toFixed(1) ?? "--";
      document.getElementById("udsCoolingFinTemp").textContent = uds.coolingFinTemp?.toFixed(1) ?? "--";
      document.getElementById("udsClutchCurrent").textContent = uds.clutchCurrent?.toFixed(3) ?? "--";
      document.getElementById("udsClutchPWM").textContent = displayValue(uds.clutchPWM);
      document.getElementById("udsClutchVoltage").textContent = uds.clutchVoltage?.toFixed(3) ?? "--";
      document.getElementById("udsBlockagePct").textContent = displayValue(uds.blockagePct);
    } else {
      // UDS-sourced glance tiles stay in the grid when the poller is off; blank
      // them back to "--" rather than leaving a stale reading on screen.
      ["udsClutchTemp", "udsModuleTemp", "udsCoolingFinTemp", "udsClutchPWM"].forEach(
        (id) => {
          const el = document.getElementById(id);
          if (el) el.textContent = "--";
        },
      );
    }

    updateV9Status(data, chassisOk, haldexOk);

    refreshTrace(data); // update the live trace (editor-grid cell highlight)

    // Ride the live operating point on the expert tune surface: cache the poll
    // and move the dot without redrawing the whole surface each frame.
    lastDashData = data;
    updateChartMarker();
  } catch (error) {
    console.log("Status failed: " + error.message);
  } finally {
    clearTimeout(pollTimeout);
    refreshStatus._inFlight = false;
  }
}

function hex2bin(hex) {
  return ("00000000" + parseInt(hex, 16).toString(2)).substr(-8);
}

// Keep the hero mode pill reading the live base mode plus any active force
// trigger. The header badge shows the base mode only; the force annotation -
// which must never be silently active - rides here on the pill and refreshes
// every poll.
function updateBannerSubtitle(data) {
  const el = document.getElementById("modePillLabel");
  if (!el) return;
  const modeName = MODE_NAMES[data.mode] ?? "Unknown";
  let text = modeName;

  // Surface EVERY active force trigger so a flag can never be silently active.
  // Each trigger must be BOTH enabled and have its live flag asserted - matching
  // the firmware gate in OpenHaldexC6_can.cpp. tcForceModeFlag/hazardForceModeFlag
  // are CAN-driven and extButtonActive is button-driven, so any of them can fire
  // without the user having touched the UI.
  const active = [];
  if (_tcForceMode     && data.tcOn === false)            active.push({ trig: "TC",     fmv: _tcForceModeValue });
  if (_hazardForceMode && data.hazardActive === true)     active.push({ trig: "Hazard", fmv: _hazardForceModeValue });
  if (_extBtnForceMode && data.extButtonActive === true)  active.push({ trig: "Ext",    fmv: _extBtnForceModeValue });

  if (active.length) {
    const parts = active.map((a) => `${MODE_NAMES[a.fmv] ?? "Unknown"} (${a.trig})`);
    text += ` · Force: ${parts.join(", ")}`;
  }
  el.textContent = text;
}

// Render a bit-by-bit legend for the Haldex state byte, generation-aware.
function renderHaldexStateLegend(rawHex, gen) {
  const el = document.getElementById("haldexStateLegend");
  if (!el) return;
  if (rawHex === undefined || rawHex === null) { el.innerHTML = ""; return; }
  const val = parseInt(rawHex, 16);

  if (gen === 1 || gen === 2 || gen === 4 || gen === 51) {
    // PQ (Gen 1/2/4): Allrad_1 byte 0 - fault/status flags
    const bits = [
      { bit: 0, label: "Clutch Fault",           desc: "Fehler_Allrad_Kupplung" },
      { bit: 1, label: "Over-Temp Protection",    desc: "Übertemperaturschutz" },
      { bit: 2, label: "Clutch Stiffness Fault",  desc: "Fehlerstatus_Kupplungssteifigkeit" },
      { bit: 3, label: "Coupling Fully Open",     desc: "Kupplung_komplett_offen" },
      { bit: 4, label: "Limp Mode",               desc: "Notlauf" },
      { bit: 5, label: "AWD Warning Lamp",        desc: "Allrad_Warnlampe" },
      { bit: 6, label: "Speed Limit",             desc: "Geschwindigkeitsbegrenzung" },
    ];
    let html = `<table><tr><th>Bit</th><th>Name</th><th>State</th></tr>`;
    bits.forEach(({ bit, label }) => {
      const active = (val >> bit) & 1;
      const cls = active ? "bit-active" : "bit-inactive";
      html += `<tr><td>${bit}</td><td>${label}</td><td class="${cls}">${active ? "SET" : "ok"}</td></tr>`;
    });
    html += `</table>`;
    el.innerHTML = html;
  } else if (gen === 50 || gen === 52) {
    // MQB (Gen 5): Allrad_03 byte 3 = ALR_Charisma_FahrPr / ALR_Charisma_Status
    const prog = val & 0x0F;
    const flags = (val >> 4) & 0x0F;
    el.innerHTML =
      `<table><tr><th>Field</th><th>Value</th></tr>` +
      `<tr><td>Driving Programme (bits 0-3)</td><td>${prog}</td></tr>` +
      `<tr><td>Status Flags (bits 4-7)</td><td>0x${flags.toString(16).toUpperCase()}</td></tr>` +
      `</table><p style="margin:4px 0 0;">Note: bit 4-5 of byte 1 = longitudinal lock state (0=open, 1=partial, 2=closed) - separate from this byte.</p>`;
  } else if (gen === 41) {
    el.innerHTML = `<em>Gen 4.1: dedicated status variables used - see Gen41 card above.</em>`;
  } else {
    el.innerHTML = "";
  }
}

// save current lock table
async function saveLockTable() {
  // send the new map data back to the ESP
  try {
    const response = await fetch("/api/tune", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        speedArray: speedHeader,
        throttleArray: throttleHeader,
        lockArray: currentLock.map((r) => r),
      }),
    });

    if (response.ok) {
      showNotification("Map Saved");
    } else {
      showNotification("Failed to Save", "error");
    }
  } catch (error) {
    console.error("Error saving map:", error);
    showNotification("Failed to Save - check connection", "error");
  }
}

// save individual setting immediately
async function saveSetting(key, value) {
  const settings = { [key]: value };

  try {
    const response = await fetchJson("/api/settings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(settings),
    });

    if (!response || !response.ok) {
      showNotification("Failed to save setting", "error");
      return false;
    }
    return true;
  } catch (error) {
    console.log("Saving setting failed: " + error.message);
    showNotification("Error saving setting", "error");
    return false;
  }
}

// Status chip helper: 'on' lights the dot, 'unknown' greys the whole chip out
// (no data - e.g. Haldex CAN down reports null for every flag).
function setChip(id, value) {
  const chip = document.getElementById(id);
  if (!chip) return;
  const unknown = value === undefined || value === null;
  chip.classList.toggle("unknown", unknown);
  chip.classList.toggle("on", !unknown && !!value);
}

// Semi-circular engagement arc, radius 80 centred at (100,100): the fill sweeps
// with the ACTUAL engagement, the tick marks the TARGET, so the lag between them
// (lock response ramp, coupling response) reads at a glance.
const GAUGE_ARC_LEN = Math.PI * 80; // length of the 180-degree track

function updateEngagementGauge(target, actual) {
  const fill = document.getElementById("gaugeArcFill");
  const tick = document.getElementById("gaugeTargetTick");
  if (!fill) return;

  const a = actual === null ? 0 : Math.max(0, Math.min(100, Number(actual) || 0));
  fill.style.strokeDashoffset = GAUGE_ARC_LEN * (1 - a / 100);

  if (tick) {
    if (target === null || Number.isNaN(Number(target))) {
      tick.setAttribute("visibility", "hidden");
    } else {
      const t = Math.max(0, Math.min(100, Number(target)));
      tick.setAttribute("transform", `rotate(${t * 1.8} 100 100)`);
      tick.setAttribute("visibility", "visible");
    }
  }
}

// ---- Live lock-response trace ----------------------------------------------
// Rolling time-history of lock target vs actual engagement. The gauge shows the
// instant target/actual pair; this shows how the actual chased the target over
// the last window, so coupling lag and the effect of the attack/release rate
// limits read at a glance while tuning. Poll-fed from /api/dashboard, so no
// extra device load. Reset on a dropped link so a reconnect never draws a line
// bridging the outage gap.
const TRACE_WINDOW_MS = 15000; // rolling window shown on the strip chart
const lockTrace = []; // ring of { t, target, actual }, pruned to the window

function resetLockTrace() {
  lockTrace.length = 0;
  renderLockTrace(Date.now());
}

function pushLockSample(target, actual, now) {
  // Target and actual drop out independently (CAN target absent vs. no engagement
  // reading yet), so each is validated and stored on its own. A missing value is
  // kept as null so the corresponding line *breaks* over that sample instead of
  // coercing to a phantom 0 (Number(null) === 0 would plot it on the floor).
  const hasTarget = target !== undefined && target !== null && Number.isFinite(Number(target));
  const hasActual = actual !== undefined && actual !== null && Number.isFinite(Number(actual));
  // Nothing to record if both are absent - skip so no empty sample enters the ring.
  if (!hasTarget && !hasActual) return;
  const t = hasTarget ? Math.max(0, Math.min(100, Number(target))) : null;
  const a = hasActual ? Math.max(0, Math.min(100, Number(actual))) : null;
  lockTrace.push({ t: now, target: t, actual: a });

  // Drop points that have scrolled off the left edge, keeping one older sample
  // so the leftmost segment enters from off-screen instead of starting mid-plot.
  const cutoff = now - TRACE_WINDOW_MS;
  let firstKept = 0;
  while (firstKept < lockTrace.length - 1 && lockTrace[firstKept + 1].t < cutoff) firstKept++;
  if (firstKept > 0) lockTrace.splice(0, firstKept);
}

function renderLockTrace(now) {
  const svg = document.getElementById("lockTraceSvg");
  if (!svg) return;

  const W = 320, H = 160;
  const padL = 26, padR = 6, padT = 8, padB = 18;
  const plotW = W - padL - padR;
  const plotH = H - padT - padB;
  const baseY = padT + plotH; // 0% lock (chart floor)

  const windowStart = now - TRACE_WINDOW_MS;
  const xPix = (t) => Math.max(padL, Math.min(W - padR, padL + ((t - windowStart) / TRACE_WINDOW_MS) * plotW));
  const yPix = (v) => padT + (1 - Math.max(0, Math.min(100, Number(v) || 0)) / 100) * plotH;

  // Literal hex (not var()) so the colours render inside string-built SVG on all
  // browsers, matching the tune chart. ACTUAL = --primary-light, others --text-dim/--border.
  const GRID = "#404040", LABEL = "#9ca3af", ACTUAL = "#ef4444", TARGET = "#9ca3af";

  let out = "";
  for (let g = 0; g <= 100; g += 25) {
    const y = yPix(g);
    out += `<line x1="${padL}" y1="${y.toFixed(1)}" x2="${W - padR}" y2="${y.toFixed(1)}" stroke="${GRID}" stroke-width="0.5"/>`;
    out += `<text x="${padL - 4}" y="${(y + 3).toFixed(1)}" fill="${LABEL}" font-size="8" text-anchor="end">${g}</text>`;
  }
  out += `<text x="${padL}" y="${H - 5}" fill="${LABEL}" font-size="8" text-anchor="start">-15s</text>`;
  out += `<text x="${W - padR}" y="${H - 5}" fill="${LABEL}" font-size="8" text-anchor="end">now</text>`;

  // lockTrace is already pruned to the window (plus one older sample for a
  // clean left edge), so it plots directly; xPix clamps any off-screen point.
  const pts = lockTrace;
  if (pts.length >= 2) {
    // Actual: filled area to the floor plus a bold line on top, broken across any
    // span where the engagement reading dropped out (p.actual === null) so a
    // missing sample never bridges a false line through the gap.
    let aSeg = [];
    const flushActual = () => {
      if (aSeg.length >= 2) {
        const line = aSeg.map((p) => `${xPix(p.t).toFixed(1)},${yPix(p.actual).toFixed(1)}`).join(" ");
        const x0 = xPix(aSeg[0].t).toFixed(1);
        const xN = xPix(aSeg[aSeg.length - 1].t).toFixed(1);
        out += `<polygon points="${x0},${baseY.toFixed(1)} ${line} ${xN},${baseY.toFixed(1)}" fill="${ACTUAL}" fill-opacity="0.15" stroke="none"/>`;
        out += `<polyline points="${line}" fill="none" stroke="${ACTUAL}" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"/>`;
      }
      aSeg = [];
    };
    for (const p of pts) {
      if (p.actual === null) { flushActual(); continue; }
      aSeg.push(p);
    }
    flushActual();

    // Target: dashed line, only across the spans where a target was present. Its
    // gaps are driven solely by a missing target, independent of actual dropouts.
    let seg = "";
    for (const p of pts) {
      if (p.target === null) {
        if (seg.trim()) { out += `<polyline points="${seg.trim()}" fill="none" stroke="${TARGET}" stroke-width="1.5" stroke-dasharray="4 3" stroke-linejoin="round"/>`; seg = ""; }
        continue;
      }
      seg += `${xPix(p.t).toFixed(1)},${yPix(p.target).toFixed(1)} `;
    }
    if (seg.trim()) out += `<polyline points="${seg.trim()}" fill="none" stroke="${TARGET}" stroke-width="1.5" stroke-dasharray="4 3" stroke-linejoin="round"/>`;
  }

  svg.innerHTML = out;
}

// ---- Learn Haldex calibration chart ----------------------------------------
// Plots the learned calibration table: X = commanded correction factor (0..100%),
// Y = measured Haldex engagement (0..100%). A dashed 1:1 reference diagonal shows
// where commanded equals measured, so the tuner can see at a glance where the
// Haldex over- or under-responds. Render-only from the table the firmware already
// returns on /api/learn/status when tableValid; no extra device load. Plain SVG,
// no libraries - the page ships from the ESP32's flash.
function renderLearnChart(table) {
  const wrap = document.getElementById("learnChartWrap");
  const svg = document.getElementById("learnChartSvg");
  if (!svg || !wrap) return;
  // A valid table is 101 points (CF 0..100 -> engagement 0..100). Anything shorter
  // (no data yet, or malformed) hides the chart rather than drawing a broken axis.
  if (!Array.isArray(table) || table.length < 2) {
    wrap.style.display = "none";
    svg.innerHTML = "";
    return;
  }
  wrap.style.display = "";

  const W = 320, H = 200;
  const padL = 26, padR = 8, padT = 8, padB = 20;
  const plotW = W - padL - padR;
  const plotH = H - padT - padB;
  const n = table.length;
  const xOf = (cf) => padL + (Math.max(0, Math.min(100, cf)) / 100) * plotW;
  const yOf = (eng) => padT + (1 - Math.max(0, Math.min(100, eng)) / 100) * plotH;

  // Literal hex (not var()) so the colours render inside string-built SVG on all
  // browsers, matching renderLockTrace. CURVE = --primary-light, REF/grid dim.
  const GRID = "#404040", LABEL = "#9ca3af", REF = "#6b7280", CURVE = "#ef4444";

  let out = "";
  for (let g = 0; g <= 100; g += 25) {
    const y = yOf(g);
    out += `<line x1="${padL}" y1="${y.toFixed(1)}" x2="${(W - padR).toFixed(1)}" y2="${y.toFixed(1)}" stroke="${GRID}" stroke-width="0.5"/>`;
    out += `<text x="${(padL - 4).toFixed(1)}" y="${(y + 3).toFixed(1)}" fill="${LABEL}" font-size="8" text-anchor="end">${g}</text>`;
    const x = xOf(g);
    out += `<text x="${x.toFixed(1)}" y="${H - 6}" fill="${LABEL}" font-size="8" text-anchor="middle">${g}</text>`;
  }

  // 1:1 reference diagonal (commanded == measured).
  out += `<line x1="${xOf(0).toFixed(1)}" y1="${yOf(0).toFixed(1)}" x2="${xOf(100).toFixed(1)}" y2="${yOf(100).toFixed(1)}" stroke="${REF}" stroke-width="1" stroke-dasharray="4 3"/>`;

  // Learned curve: one vertex per table entry, index -> commanded CF %.
  let line = "";
  for (let i = 0; i < n; i++) {
    const cf = (i / (n - 1)) * 100;
    const eng = Number(table[i]) || 0;
    line += `${xOf(cf).toFixed(1)},${yOf(eng).toFixed(1)} `;
  }
  out += `<polyline points="${line.trim()}" fill="none" stroke="${CURVE}" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"/>`;

  svg.innerHTML = out;
}

// The two speed cutoffs bracket the band where lock is allowed: lock is disabled
// below the under-speed and above the above-speed, so it only engages between the
// two. A bound of 0 means "no cut" on that side (matches speed_disengage_ok in
// the firmware). If the under-speed meets or passes a non-zero above-speed the
// allowed band collapses and lock silently never engages - surface that rather
// than leave it a foot-gun (the shipped 120/120 default leaves a one-speed sliver).
function updateDisableWindowHint() {
  const hint = document.getElementById("disableWindowHint");
  if (!hint) return;
  const underEl = document.getElementById("disengageUnderSpeedRange");
  const aboveEl = document.getElementById("disengageAboveSpeedRange");
  if (!underEl || !aboveEl) return;
  const under = parseInt(underEl.value, 10) || 0;
  const above = parseInt(aboveEl.value, 10) || 0;
  hint.style.display = "";
  if (above > 0 && under >= above) {
    hint.textContent = under === above
      ? `Both cutoffs are ${under} km/h, so lock only engages at exactly ${under} km/h - widen the gap to give it a usable band.`
      : `Under-speed (${under}) sits above the above-speed cutoff (${above}), so lock never engages. Lower the under-speed or raise the above-speed.`;
    hint.style.color = "var(--warning)";
  } else if (above > 0) {
    hint.textContent = `Lock engages between ${under} and ${above} km/h.`;
    hint.style.color = "";
  } else if (under > 0) {
    hint.textContent = `Lock disabled below ${under} km/h; no upper speed cutoff.`;
    hint.style.color = "";
  } else {
    hint.textContent = "No speed cutoffs - lock allowed at any speed.";
    hint.style.color = "";
  }
}

// initialise navigation:
function initNavigation() {
  // setup tabs
  const tabs = document.querySelectorAll(".nav-tab");
  const pages = document.querySelectorAll(".page");

  tabs.forEach((tab) => {
    tab.addEventListener("click", () => {
      const page = tab.dataset.page;

      tabs.forEach((t) => {
        t.classList.remove("active");
        t.removeAttribute("aria-current");
      });
      tab.classList.add("active");
      tab.setAttribute("aria-current", "page");

      pages.forEach((p) => p.classList.remove("active"));
      document.getElementById(`${page}-page`).classList.add("active");
    });
  });

  // setup 'when slider changes', do this...
  const disengageUnderSpeedRange = document.getElementById(
    "disengageUnderSpeedRange",
  );
  const disengageUnderSpeed = document.getElementById("disengageUnderSpeed");
  disengageUnderSpeedRange.addEventListener("input", () => {
    disengageUnderSpeed.textContent = disengageUnderSpeedRange.value;
    updateDisableWindowHint();
  });

  const disengageAboveSpeedRange = document.getElementById(
    "disengageAboveSpeedRange",
  );
  const disengageAboveSpeed = document.getElementById("disengageAboveSpeed");
  disengageAboveSpeedRange.addEventListener("input", () => {
    disengageAboveSpeed.textContent = disengageAboveSpeedRange.value;
    updateDisableWindowHint();
  });
  updateDisableWindowHint(); // reflect the loaded values on first paint
  const disableThrottleRange = document.getElementById("disableThrottleRange");
  const disableThrottle = document.getElementById("disableThrottle");
  disableThrottleRange.addEventListener("input", () => {
    disableThrottle.textContent = disableThrottleRange.value;
  });

  const ledBrightnessRange = document.getElementById("ledBrightnessRange");
  const ledBrightnessValue = document.getElementById("ledBrightnessValue");
  if (ledBrightnessRange) {
    ledBrightnessRange.addEventListener("input", () => {
      ledBrightnessValue.textContent = ledBrightnessRange.value;
    });
  }

  // Sleep auto-setup toggle: on = armed (measures the next parked stretch), off = declined.
  const sleepCalArm = document.getElementById("sleepCalArm");
  if (sleepCalArm) {
    sleepCalArm.addEventListener("change", async () => {
      if (await saveSetting("sleepCalArm", sleepCalArm.checked)) {
        _sleepCalState = sleepCalArm.checked ? 1 : 3;
        renderSleepCal();
      } else {
        renderSleepCal(); // put the switch back to what the unit holds
      }
    });
  }

  // LP wake threshold slider
  const lpWakeRange = document.getElementById("lpWakeThresholdFpsRange");
  const lpWakeVal   = document.getElementById("lpWakeThresholdFpsValue");
  if (lpWakeRange) {
    lpWakeRange.addEventListener("input", () => {
      if (lpWakeVal) lpWakeVal.textContent = lpWakeRange.value;
    });
    lpWakeRange.addEventListener("change", () => {
      saveSetting("lpWakeThresholdFps", parseInt(lpWakeRange.value));
    });
  }

  // BPK lock-calibration slider (per-car; the Nm the spoof frame claims at full
  // command, not a strength dial - higher does not lock harder). Save on release
  // (change), not on every input tick, so dragging doesn't stream calibration
  // changes at a live car.
  const bpkCeilingRange = document.getElementById("bpkCeilingRange");
  const bpkCeilingValue = document.getElementById("bpkCeilingValue");
  if (bpkCeilingRange) {
    bpkCeilingRange.addEventListener("input", () => {
      if (bpkCeilingValue) bpkCeilingValue.textContent = bpkCeilingRange.value;
    });
    bpkCeilingRange.addEventListener("change", () => {
      saveSetting("bpkCeilingNm", parseInt(bpkCeilingRange.value));
    });
  }

  initGeometry();

  // ESP_14 Min-band launch-PWM floor. Same rationale as the BPK slider: update
  // the label live while dragging, only save on release so the car isn't streamed
  // calibration changes mid-drag.
  const esp14FloorRange = document.getElementById("esp14MinFloorRange");
  const esp14FloorValue = document.getElementById("esp14MinFloorValue");
  if (esp14FloorRange) {
    esp14FloorRange.addEventListener("input", () => {
      if (esp14FloorValue) esp14FloorValue.textContent = esp14FloorRange.value;
    });
    esp14FloorRange.addEventListener("change", () => {
      saveSetting("esp14MinFloorPct", parseInt(esp14FloorRange.value));
    });
  }

  // Lock response ramp sliders (display update only - save handled in initSettings)
  const lockReleaseRange = document.getElementById("lockReleaseRampRange");
  const lockReleaseVal   = document.getElementById("lockReleaseRampValue");
  if (lockReleaseRange) {
    lockReleaseRange.addEventListener("input", () => {
      if (lockReleaseVal) lockReleaseVal.textContent = lockReleaseRange.value;
    });
  }
  const lockEngageRange = document.getElementById("lockEngageRampRange");
  const lockEngageVal   = document.getElementById("lockEngageRampValue");
  if (lockEngageRange) {
    lockEngageRange.addEventListener("input", () => {
      if (lockEngageVal) lockEngageVal.textContent = lockEngageRange.value;
    });
  }

  // Steering gain sliders (display update only - save handled in initSettings)
  [
    ["steeringGainStartRange", "steeringGainStartValue"],
    ["steeringGainFullRange",  "steeringGainFullValue"],
    ["steeringGainFloorRange", "steeringGainFloorValue"],
  ].forEach(([rangeId, valueId]) => {
    const rangeElem = document.getElementById(rangeId);
    const valueElem = document.getElementById(valueId);
    if (rangeElem) {
      rangeElem.addEventListener("input", () => {
        if (valueElem) valueElem.textContent = rangeElem.value;
      });
    }
  });
}

// initialise dashboard:
function initDashboard() {
  renderLockTrace(Date.now()); // draw the empty grid before the first poll lands

  // Self-scheduling poll loop. The old design used a fixed 500ms setInterval with
  // a one-in-flight guard: whenever a response landed just after a tick had
  // already fired-and-bailed (previous poll still in flight), the next tick was a
  // full interval away, collapsing a nominal 2Hz to ~1Hz and wasting the gap
  // between "reply arrived" and "next tick". Here the next poll is scheduled only
  // once the previous one has fully settled, plus a small floor, so the live rate
  // follows the device's real latency instead of being quantised to a tick.
  //
  // A generation token gates the loop: bumping it stops the current loop (its
  // post-await check fails) so visibility changes can't leave two loops running.
  let pollGeneration = 0;

  async function pollLoop(myGen) {
    while (myGen === pollGeneration && !document.hidden) {
      const started = Date.now();
      await refreshStatus(); // resolves when the poll settles (success, miss, or timeout)
      if (myGen !== pollGeneration || document.hidden) return; // superseded or backgrounded
      const gap = POLL_MIN_GAP_MS - (Date.now() - started);
      if (gap > 0) await new Promise((resolve) => setTimeout(resolve, gap));
    }
  }

  pollLoop(++pollGeneration);

  // Stop polling while the page is hidden (phone locked or app backgrounded) so
  // we don't keep waking the ESP's web server; resume with a fresh read on return.
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) {
      pollGeneration++; // running loop exits at its next generation check
    } else {
      pollLoop(++pollGeneration); // fresh loop; any stale one exits on the token
    }
  });
}

// Short vibration on tap where the browser allows it - purely tactile feedback.
function haptic(ms) {
  try {
    if (navigator.vibrate) navigator.vibrate(ms);
  } catch (e) {
    /* vibration blocked or unsupported - ignore */
  }
}

// Drive-mode drawer: the dashboard shows the live mode as a pill, and tapping it
// slides in the full six-button picker from the right. Keeps the mode grid off
// the glance view so lock %, live data and the trace fit one screen.
function setModeDrawer(open) {
  const drawer = document.getElementById("modeDrawer");
  const backdrop = document.getElementById("modeBackdrop");
  const pill = document.getElementById("modePill");
  if (!drawer) return;
  drawer.classList.toggle("open", open);
  drawer.setAttribute("aria-hidden", open ? "false" : "true");
  // inert keeps the off-screen mode buttons out of the tab order and the
  // accessibility tree while the drawer is closed.
  drawer.inert = !open;
  if (backdrop) backdrop.classList.toggle("open", open);
  if (pill) pill.setAttribute("aria-expanded", open ? "true" : "false");
}

function initModeDrawer() {
  const pill = document.getElementById("modePill");
  const backdrop = document.getElementById("modeBackdrop");
  const closeBtn = document.getElementById("modeDrawerClose");
  if (pill) {
    pill.addEventListener("click", () => {
      haptic(10);
      setModeDrawer(true);
    });
  }
  if (backdrop) backdrop.addEventListener("click", () => setModeDrawer(false));
  if (closeBtn) closeBtn.addEventListener("click", () => setModeDrawer(false));
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") setModeDrawer(false);
  });
  setModeDrawer(false); // establish the closed/inert state on load
}

// initialise mode buttons
function initModeButtons() {
  const buttons = document.querySelectorAll(".mode-btn");
  buttons.forEach((btn) => {
    btn.addEventListener("click", () => {
      const mode = parseInt(btn.dataset.mode);

      // Guard: controller disabled
      if (_disableController) {
        showNotification("Controller is disabled - enable it in Controller Options before changing mode", "error");
        return;
      }

      // Guard: Stock unavailable in standalone (no chassis CAN to read from)
      if (mode === 0 && _isStandalone) {
        showNotification("Stock mode is unavailable in Standalone - no chassis CAN to read from", "error");
        return;
      }

      // Guard: Expert requires CAN data (not standalone without 'Use CAN if Available')
      if (mode === 5 && _isStandalone && !_useCANifAvailable) {
        showNotification("Expert mode requires 'Use CAN if Available' to be enabled when in Standalone", "error");
        return;
      }

      haptic(15); // firmer tick for a mode change
      _pendingMode = mode; // hold this selection against stale polls until echoed
      _pendingModeTs = Date.now();
      modeButton(mode); // change highlighted mode
      sendMode(mode); // send new mode to ESP
      setModeDrawer(false); // picked a mode - slide the drawer away
    });
  });

  async function sendMode(mode) {
    const sendData = {
      mode: mode, // send just the mode change
    };

    // fetchJson never throws - transport and parse failures resolve to
    // undefined, so the !resp branch below covers them.
    const resp = await fetchJson("/api/mode", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(sendData),
    });
    if (!resp || resp.ok === false) {
      // Failed on the module (or no response): drop the pending hold so the
      // next poll snaps the highlight back to the real mode, and say why -
      // a silently reverting button reads as a broken UI.
      _pendingMode = null;
      showNotification(
        (resp && resp.error) ? resp.error : "Mode change failed - check connection",
        "error"
      );
    }
  }
}

// initialise settings with immediate save on change
function initSettings() {
  // Selects (dropdown menus)
  const selectIds = ["haldexGeneration", "tcForceModeValue", "hazardForceModeValue", "extBtnForceModeValue", "forceModesPriority"];
  selectIds.forEach((id) => {
    const elem = document.getElementById(id);
    if (elem) {
      elem.addEventListener("change", async () => {
        await saveSetting(id, parseInt(elem.value));
        // A new generation changes which frame blocks can be edited.
        if (id === "haldexGeneration") {
          _haldexGeneration = parseInt(elem.value);
          refreshFrameBlocks();
          maybeOfferSleepSetup(_haldexGeneration, true); // picking Gen5 always asks again
        }
      });
    }
  });

  // Range sliders
  const rangeSliders = [
    { element: "disengageUnderSpeedRange", key: "disengageUnderSpeed", parse: parseInt },
    { element: "disengageAboveSpeedRange", key: "disengageAboveSpeed", parse: parseInt },
    { element: "disableThrottleRange",     key: "disableThrottle",     parse: parseInt },
    { element: "lockReleaseRampRange",     key: "lockReleaseRampMs",    parse: parseInt },
    { element: "lockEngageRampRange",      key: "lockEngageRampMs",     parse: parseInt },
    { element: "steeringGainStartRange",   key: "steeringGainStartDeg",  parse: parseInt },
    { element: "steeringGainFullRange",    key: "steeringGainFullDeg",   parse: parseInt },
    { element: "steeringGainFloorRange",   key: "steeringGainFloor",     parse: parseInt },
  ];
  rangeSliders.forEach(({ element, key, parse }) => {
    const elem = document.getElementById(element);
    if (elem) {
      elem.addEventListener("input", () => {
        saveSetting(key, (parse || parseInt)(elem.value));
      });
    }
  });

  // LED brightness: UI is 0-100%, firmware stores 0-255
  const ledBrightElem = document.getElementById("ledBrightnessRange");
  if (ledBrightElem) {
    ledBrightElem.addEventListener("input", () => {
      saveSetting("ledBrightness", Math.round(parseInt(ledBrightElem.value) * 2.55));
    });
  }

  // Checkboxes - keep cached state in sync for mode-button guards
  const checkboxCacheMap = {
    disableController: (v) => { _disableController = v; },
    isStandalone:      (v) => { _isStandalone = v; },
    useCANifAvailable: (v) => { _useCANifAvailable = v; },
  };

  // High-consequence toggles get a confirm when switched INTO their disruptive
  // state, so a stray tap (or a scroll that slips past guardScrollTaps) can't
  // silently take the car out of active control. Turning them back off is the
  // recovery action and needs no confirm.
  const confirmOnEnable = {
    dangerZoneEnabled: "Enable Danger Zone? A full 50:50 request will demand maximum clutch duty: much higher pump load and current draw, and the Returned % will read lower.",
    disableController: "Disable the Haldex controller? The unit stops modifying CAN frames and the car reverts to stock behaviour.",
    isStandalone: "Switch to Standalone? The unit stops reading the car's CAN bus and synthesises frames on its own.",
    analyzerMode: "Enter Analyzer (SavvyCAN) mode? The controller stops spoofing and only sniffs the bus.",
  };

  const checkboxIds = [
    "disableController",
    "isStandalone",
    "useCANifAvailable",
    "tcForceMode",
    "hazardForceMode",
    "followBrake",
    "invertBrake",
    "followHandbrake",
    "invertHandbrake",
    "broadcastOpenHaldexOverCAN",
    "disableOnboardButton",
    "disableExternalButton",
    "canSleepEnabled",
    "canSleepAggressive",
    "dangerZoneEnabled",
    "benchMode",
    "bleEnabled",
    "espNowEnabled",
    "espNowControl",
    "liveDiagEnabled",
    "lockReleaseEnabled",
    "steeringGainEnabled",
  ];
  checkboxIds.forEach((id) => {
    const elem = document.getElementById(id);
    if (elem) {
      elem.addEventListener("change", async () => {
        if (confirmOnEnable[id] && elem.checked && !confirm(confirmOnEnable[id])) {
          elem.checked = false; // user backed out - revert without saving
          return;
        }
        if (checkboxCacheMap[id]) checkboxCacheMap[id](elem.checked);
        await saveSetting(id, elem.checked);
        if (id === "isStandalone") refreshFrameBlocks(); // standalone has no editable passthrough frames
      });
    }
  });

  // Ext. Control select (Press: Advance Mode / Hold: Force Mode)
  {
    const extBtnCtrlElem = document.getElementById("extButtonForceMode");
    if (extBtnCtrlElem) {
      const fmvRow = document.getElementById("extBtnForceModeValueRow");
      extBtnCtrlElem.addEventListener("change", () => {
        const isHold = extBtnCtrlElem.value === "1";
        saveSetting("extButtonForceMode", isHold);
        if (fmvRow) fmvRow.style.display = isHold ? "" : "none";
      });
    }
  }

  // Lock response: toggle slider enabled state and opacity when checkbox changes.
  const lockReleaseEnabledElem = document.getElementById("lockReleaseEnabled");
  const lockReleaseElem    = document.getElementById("lockReleaseRampRange");
  const lockReleaseContainer   = document.getElementById("lockReleaseRampContainer");
  const lockEngageElem     = document.getElementById("lockEngageRampRange");
  const lockEngageContainer    = document.getElementById("lockEngageRampContainer");
  if (lockReleaseEnabledElem) {
    lockReleaseEnabledElem.addEventListener("change", () => {
      const en = lockReleaseEnabledElem.checked;
      if (lockReleaseElem) lockReleaseElem.disabled = !en;
      if (lockReleaseContainer) lockReleaseContainer.style.opacity = en ? "" : "0.4";
      if (lockEngageElem) lockEngageElem.disabled = !en;
      if (lockEngageContainer) lockEngageContainer.style.opacity = en ? "" : "0.4";
    });
  }

  // Steering gain: toggle slider enabled state and opacity when checkbox changes.
  const steerGainEnabledElem = document.getElementById("steeringGainEnabled");
  if (steerGainEnabledElem) {
    steerGainEnabledElem.addEventListener("change", () => {
      const en = steerGainEnabledElem.checked;
      ["steeringGainStart", "steeringGainFull", "steeringGainFloor"].forEach((base) => {
        const rangeElem = document.getElementById(base + "Range");
        const container = document.getElementById(base + "Container");
        if (rangeElem) rangeElem.disabled = !en;
        if (container) container.style.opacity = en ? "" : "0.4";
      });
    });
  }

  // CAN sleep: Aggressive implies Basic. Keep the two checkboxes in sync.
  const canSleepElem = document.getElementById("canSleepEnabled");
  const canSleepAggrElem = document.getElementById("canSleepAggressive");
  if (canSleepElem && canSleepAggrElem) {
    canSleepAggrElem.addEventListener("change", () => {
      if (canSleepAggrElem.checked && !canSleepElem.checked) {
        canSleepElem.checked = true;
        saveSetting("canSleepEnabled", true);
      }
      canSleepElem.disabled = canSleepAggrElem.checked;
    });
    canSleepElem.addEventListener("change", () => {
      if (!canSleepElem.checked && canSleepAggrElem.checked) {
        canSleepAggrElem.checked = false;
        saveSetting("canSleepAggressive", false);
        canSleepElem.disabled = false;
      }
    });
  }

  // SavvyCAN mode - mutually exclusive: WiFi and Serial cannot both be active.
  const analyzerModeElem = document.getElementById("analyzerMode");
  const analyzerSerialElem = document.getElementById("analyzerSerial");
  if (analyzerModeElem && analyzerSerialElem) {
    analyzerModeElem.addEventListener("change", () => {
      if (analyzerModeElem.checked && !confirm(confirmOnEnable.analyzerMode)) {
        analyzerModeElem.checked = false; // user backed out - revert without saving
        return;
      }
      if (analyzerModeElem.checked) {
        analyzerSerialElem.checked = false;
        saveSetting("analyzerSerial", false);
      }
      saveSetting("analyzerMode", analyzerModeElem.checked);
    });
    analyzerSerialElem.addEventListener("change", () => {
      if (analyzerSerialElem.checked) {
        analyzerModeElem.checked = false;
        saveSetting("analyzerMode", false);
      }
      saveSetting("analyzerSerial", analyzerSerialElem.checked);
    });
  }
}

function modeButton(mode) {
  const buttons = document.querySelectorAll(".mode-btn");
  buttons.forEach((btn) => {
    btn.classList.toggle("active", parseInt(btn.dataset.mode) === mode);
  });

  // Keep the dashboard mode pill reading the live mode (set here rather than in
  // the click handler so external mode changes from the ESP also update it).
  const pillLabel = document.getElementById("modePillLabel");
  if (pillLabel) pillLabel.textContent = MODE_NAMES[mode] || "Unknown";

  // Mirror the base mode onto the header badge so the running mode is visible
  // from any tab, not just the dashboard. Force triggers still annotate the
  // hero pill; this is the compact glance.
  const hdr = document.getElementById("modeStatus");
  if (hdr) hdr.textContent = "Mode " + (MODE_NAMES[mode] || "Unknown");
}

// initialise expert editor
// Build the editor grid (axis headers + lock cells) from the current
// speedHeader/throttleHeader/currentLock globals. Called on first init and
// again whenever a map is loaded (preset, import, or restore defaults).
function buildMapGrid() {
  const mapGrid = document.getElementById("mapGrid"); // find the 'map grid'
  mapGrid.innerHTML = ""; // clear any prior grid so a reload doesn't stack cells

  const cellMarker = document.createElement("div"); // create the first element (which will be a dead cell)
  cellMarker.className = "map-cellHeader";
  cellMarker.setAttribute("speed", String("100")); // make a new attribuate for column
  cellMarker.setAttribute("throttle", String("100")); // make a new attribute for row
  cellMarker.setAttribute("lock", String("na")); // make a new attribute for row
  cellMarker.textContent = String("/"); // marker for showing row/col
  mapGrid.appendChild(cellMarker); // add in the first cell

  // fill the speed axis
  for (let speed = 0; speed < arrayColumns; speed++) {
    const cellSpeedAxis = document.createElement("div");
    cellSpeedAxis.className = "map-cellHeader";
    cellSpeedAxis.min = 0;
    cellSpeedAxis.max = 300;
    cellSpeedAxis.setAttribute("speed", String(speed)); // make a new attribuate for column
    cellSpeedAxis.setAttribute("throttle", String("100")); // make a new attribute for row
    cellSpeedAxis.setAttribute("lock", String("false")); // make a new attribute for row
    cellSpeedAxis.textContent = String(speedHeader[speed]); // update the cell text with the value in currentLock

    cellSpeedAxis.addEventListener("click", () => {
      openEditValue(cellSpeedAxis);
    });

    mapGrid.appendChild(cellSpeedAxis);
  }

  for (let throttle = 0; throttle < arrayRows; throttle++) {
    // fill the throttle axis
    const cellThrottleAxis = document.createElement("div");
    cellThrottleAxis.className = "map-cellHeader";
    cellThrottleAxis.min = 0;
    cellThrottleAxis.max = 100;
    cellThrottleAxis.setAttribute("speed", String("100")); // make a new attribuate for column
    cellThrottleAxis.setAttribute("throttle", String(throttle)); // make a new attribute for row
    cellThrottleAxis.setAttribute("lock", String("false")); // make a new attribute for row
    cellThrottleAxis.textContent = String(throttleHeader[throttle]); // update the cell text with the value in currentLock

    cellThrottleAxis.addEventListener("click", () => {
      openEditValue(cellThrottleAxis);
    });

    mapGrid.appendChild(cellThrottleAxis);
    //end fill

    // fill the contents
    for (let speed = 0; speed < arrayColumns; speed++) {
      const cell = document.createElement("div");
      cell.className = "map-cell";

      cell.setAttribute("speed", String(speed)); // make a new attribuate for column
      cell.setAttribute("throttle", String(throttle)); // make a new attribute for row
      cell.setAttribute("lock", String("true")); // make a new attribute for row
      cell.min = 0;
      cell.max = 100;
      cell.textContent = String(currentLock[throttle][speed]); // update the cell text with the value in currentLock

      // Lock cells no longer bind a click handler: taps and drag-selection are
      // handled by initTuneSelection() via pointer events on the grid so a single
      // tap opens the editor while a drag selects a block of cells.

      updateCellColor(cell, currentLock[throttle][speed]); // update the colour (low/medium/high)

      mapGrid.appendChild(cell);
    }
  }
}

function initExpertEditor() {
  buildMapGrid();

  document.getElementById("cancelEdit").addEventListener("click", cancelEdit);
  document.getElementById("confirmEdit").addEventListener("click", confirmEdit);
  document.getElementById("saveMap").addEventListener("click", saveLockTable);
  document
    .getElementById("restoreDefaults")
    .addEventListener("click", restoreDefaults);

  // Selection toolbar (drag-select block edit + smooth).
  const selSetValue = document.getElementById("selSetValue");
  const selSmooth = document.getElementById("selSmooth");
  const selClear = document.getElementById("selClear");
  if (selSetValue) selSetValue.addEventListener("click", openEditSelection);
  if (selSmooth) selSmooth.addEventListener("click", smoothSelection);
  if (selClear) selClear.addEventListener("click", clearSelection);

  initMapManager(); // presets, saved slots, import/export
}

// ---- Expert editor: drag-select, block edit, smooth, chart dots ------------
// Selected lock cells are tracked as "row,col" (throttle,speed) keys. A single
// tap opens the editor for one cell; a click-drag paints a rectangular block.
// Every selected cell is mirrored as a dot on the pseudo-3D surface so the tuner
// sees exactly which cells an edit or smooth will touch.
let selectedCells = new Set();
let selDragging = false;
let selAnchor = null; // {r, c} where the drag started
let selMoved = false; // did the pointer leave the anchor cell during this gesture
let editMultiMode = false; // confirmEdit applies to the whole selection when true
// Phone-friendly selection: a single tap opens the one-cell editor by default,
// which conflicts with drag-to-select on touch (the page just scrolls). So block
// selection behind an explicit "Select cells" toggle. When off: normal scroll +
// tap-to-edit. When on: the grid stops scrolling, a tap toggles one cell, and a
// drag paints a rectangular block.
let selectMode = false;
// Swallow the ghost click that trails a drag or long-press. A long-press (>450ms)
// often makes the browser fire `contextmenu` instead of `click`, so the trailing
// click never arrives - a naked boolean would then stay armed forever and eat the
// user's *next* deliberate tap (the reported "first box doesn't highlight" bug).
// Arming with an expiry timestamp fixes that: if no ghost click lands inside the
// window, the guard has simply lapsed by the time the next real tap comes.
let suppressClickUntil = 0; // ms timestamp; a click before this is treated as the ghost
const SUPPRESS_CLICK_MS = 400; // ghost click follows its gesture within a few frames
function armSuppressClick() {
  suppressClickUntil = Date.now() + SUPPRESS_CLICK_MS;
}

function cellKey(r, c) {
  return r + "," + c;
}

function getCellRC(el) {
  if (!el || !el.classList || !el.classList.contains("map-cell")) return null;
  const c = parseInt(el.getAttribute("speed"));
  const r = parseInt(el.getAttribute("throttle"));
  if (Number.isNaN(r) || Number.isNaN(c)) return null;
  return { r, c };
}

function cellForRC(r, c) {
  return document.querySelector(
    `#mapGrid .map-cell[throttle="${r}"][speed="${c}"]`,
  );
}

function setRectSelection(a, b) {
  selectedCells.clear();
  const r0 = Math.min(a.r, b.r), r1 = Math.max(a.r, b.r);
  const c0 = Math.min(a.c, b.c), c1 = Math.max(a.c, b.c);
  for (let r = r0; r <= r1; r++) {
    for (let c = c0; c <= c1; c++) selectedCells.add(cellKey(r, c));
  }
  applySelectionHighlight();
}

function applySelectionHighlight() {
  document.querySelectorAll("#mapGrid .map-cell").forEach((cell) => {
    const rc = getCellRC(cell);
    cell.classList.toggle(
      "selected",
      !!rc && selectedCells.has(cellKey(rc.r, rc.c)),
    );
  });
  updateSelectionToolbar();
  updateSelectionMarkers();
}

function clearSelection() {
  selectedCells.clear();
  applySelectionHighlight();
}

function updateSelectionToolbar() {
  const bar = document.getElementById("selToolbar");
  const count = document.getElementById("selCount");
  if (!bar) return;
  const n = selectedCells.size;
  bar.style.display = n > 0 ? "" : "none";
  if (count) count.textContent = `${n} ${n === 1 ? "cell" : "cells"} selected`;
}

// Paint a dot on the 3D surface for every selected cell. Reuses the same
// projection the surface was drawn with so each dot lands on its cell exactly.
function updateSelectionMarkers() {
  const g = document.getElementById("selDots");
  if (!g) return;
  if (!tuneProject) {
    g.innerHTML = "";
    return;
  }
  let out = "";
  selectedCells.forEach((k) => {
    const [r, c] = k.split(",").map(Number);
    if (!currentLock[r]) return;
    const p = tuneProject(c, r, currentLock[r][c]);
    out += `<circle cx="${p.x.toFixed(1)}" cy="${p.y.toFixed(1)}" r="3.2" fill="#38bdf8" stroke="#fff" stroke-width="1.2"/>`;
  });
  g.innerHTML = out;
}

// Flip select mode on/off, updating the grid, the toggle button, and clearing
// any selection when leaving the mode.
function setSelectMode(on) {
  selectMode = on;
  const grid = document.getElementById("mapGrid");
  const btn = document.getElementById("selModeToggle");
  if (grid) grid.classList.toggle("select-mode", on);
  if (btn) {
    btn.classList.toggle("active", on);
    btn.setAttribute("aria-pressed", on ? "true" : "false");
    btn.textContent = on ? "Selecting - tap cells (done)" : "Select cells";
  }
  if (!on) clearSelection();
}

// Toggle one cell in/out of the selection (a tap while in select mode), so a
// scattered set of cells can be built up by tapping.
function toggleCell(rc) {
  const k = cellKey(rc.r, rc.c);
  if (selectedCells.has(k)) selectedCells.delete(k);
  else selectedCells.add(k);
  applySelectionHighlight();
}

function initTuneSelection() {
  const grid = document.getElementById("mapGrid");
  if (!grid) return;

  const toggle = document.getElementById("selModeToggle");
  if (toggle) toggle.addEventListener("click", () => setSelectMode(!selectMode));

  // Primary, always-reliable path: a plain click on a cell. Mobile browsers
  // deliver a click for every tap; the earlier pointer-only approach dropped
  // taps on touch (the reported bug). In select mode a click toggles the cell
  // in/out of the selection; otherwise it opens the single-cell editor.
  grid.addEventListener("click", (e) => {
    if (suppressClickUntil) {
      // A gesture (drag or long-press) armed the ghost-click guard. Clear it
      // unconditionally so it can never leak into a later tap; only swallow THIS
      // click if it landed inside the ghost window - a click arriving after the
      // window has lapsed is a genuine tap and must fall through and register.
      const within = Date.now() < suppressClickUntil;
      suppressClickUntil = 0;
      if (within) return;
    }
    const rc = getCellRC(e.target);
    if (!rc) return; // axis-header cells keep their own click handler
    if (selectMode) {
      toggleCell(rc);
    } else {
      const cell = cellForRC(rc.r, rc.c);
      if (cell) openEditValue(cell);
    }
  });

  // Progressive enhancement: drag to paint a rectangle while in select mode.
  // Pointer events only, and layered so that if they misbehave on a given
  // device the click path above still delivers tap-to-select.
  const onMove = (e) => {
    if (!selDragging) return;
    const el = document.elementFromPoint(e.clientX, e.clientY);
    const rc = getCellRC(el);
    if (!rc) return;
    if (rc.r !== selAnchor.r || rc.c !== selAnchor.c) selMoved = true;
    // A drag paints a fresh rectangle from the anchor to the current cell.
    setRectSelection(selAnchor, rc);
  };

  const teardown = () => {
    selDragging = false;
    document.removeEventListener("pointermove", onMove);
    document.removeEventListener("pointerup", onUp);
    document.removeEventListener("pointercancel", onCancel);
  };

  const onUp = () => {
    if (!selDragging) return;
    teardown();
    // Only a real drag (pointer left the anchor cell) owns the outcome; a tap
    // that never moved falls through to the click handler above. Suppress the
    // click that the browser fires after a drag so it can't undo the paint.
    if (selMoved) armSuppressClick();
  };

  const onCancel = () => teardown();

  // Long-press a cell (while NOT in select mode) to enter multi-select without
  // reaching for the toolbar button - the standard mobile gesture. Holding ~450ms
  // on a cell flips the mode on and seeds the selection with that cell. Any real
  // finger movement first (a scroll) aborts the press so scrolling still works.
  let lpTimer = null;
  let lpStart = null;
  const cancelLongPress = () => {
    if (lpTimer) {
      clearTimeout(lpTimer);
      lpTimer = null;
    }
    lpStart = null;
    document.removeEventListener("pointermove", onLongPressMove);
    document.removeEventListener("pointerup", onLongPressEnd);
    document.removeEventListener("pointercancel", onLongPressEnd);
  };
  const onLongPressMove = (e) => {
    if (!lpStart) return;
    const dx = e.clientX - lpStart.x;
    const dy = e.clientY - lpStart.y;
    if (dx * dx + dy * dy > 100) cancelLongPress(); // moved >10px: treat as scroll
  };
  const onLongPressEnd = () => cancelLongPress();

  grid.addEventListener("pointerdown", (e) => {
    if (!selectMode) {
      // Arm a long-press to enter select mode; a short tap still edits one cell.
      const rc = getCellRC(e.target);
      if (!rc) return;
      cancelLongPress();
      lpStart = { x: e.clientX, y: e.clientY };
      lpTimer = setTimeout(() => {
        lpTimer = null;
        cancelLongPress();
        setSelectMode(true);
        toggleCell(rc); // seed the selection with the held cell
        armSuppressClick(); // swallow the click that trails this press, if one comes
      }, 450);
      document.addEventListener("pointermove", onLongPressMove);
      document.addEventListener("pointerup", onLongPressEnd);
      document.addEventListener("pointercancel", onLongPressEnd);
      return;
    }
    const rc = getCellRC(e.target);
    if (!rc) return;
    e.preventDefault(); // claim the gesture so the page doesn't scroll-fight
    selDragging = true;
    selMoved = false;
    selAnchor = rc;
    document.addEventListener("pointermove", onMove);
    document.addEventListener("pointerup", onUp);
    document.addEventListener("pointercancel", onCancel);
  });

  // Belt-and-suspenders for mobile Safari, where touch-action alone doesn't
  // always stop the scroll: while dragging in select mode, kill touchmove.
  grid.addEventListener(
    "touchmove",
    (e) => {
      if (selectMode && selDragging) e.preventDefault();
    },
    { passive: false },
  );
}

// Open the editor to set one value across the whole selection.
function openEditSelection() {
  if (selectedCells.size === 0) return;
  editMultiMode = true;
  currentEditCell = null;
  const modal = document.getElementById("editModal");
  const input = document.getElementById("editValue");
  document.getElementById("editModalTitle").textContent =
    `Set ${selectedCells.size} ${selectedCells.size === 1 ? "cell" : "cells"} (Lock %)`;
  input.value = "";
  modal.classList.add("active");
  input.focus();
}

// Box-smooth: each selected cell becomes the average of itself and its four
// orthogonal neighbours. Reads from a snapshot so the pass isn't biased by
// cells already smoothed this round. With no selection, smooths the whole map.
function smoothSelection() {
  const keys = selectedCells.size
    ? [...selectedCells]
    : (() => {
        const all = [];
        for (let r = 0; r < arrayRows; r++)
          for (let c = 0; c < arrayColumns; c++) all.push(cellKey(r, c));
        return all;
      })();
  const src = currentLock.map((row) => row.slice());
  keys.forEach((k) => {
    const [r, c] = k.split(",").map(Number);
    let sum = src[r][c], n = 1;
    [[-1, 0], [1, 0], [0, -1], [0, 1]].forEach(([dr, dc]) => {
      const rr = r + dr, cc = c + dc;
      if (rr >= 0 && rr < arrayRows && cc >= 0 && cc < arrayColumns) {
        sum += src[rr][cc];
        n++;
      }
    });
    const v = Math.round(Math.max(0, Math.min(100, sum / n)));
    currentLock[r][c] = v;
    const cell = cellForRC(r, c);
    if (cell) {
      cell.textContent = String(v);
      updateCellColor(cell, v);
    }
  });
  drawTuneChart();
}

// find array position from a value
function arrayIndex(value, array) {
  let position = 0;
  for (let i = 0; i < array.length; i++) {
    if (value >= array[i]) position = i;
  }
  return position;
}

// refresh live trace
function refreshTrace(data) {
  const cell = [...document.querySelectorAll(".map-cell")]; // find all the cells in the grid (as an array)

  cell.forEach((c) => c.classList.remove("activeTrace")); // clear the active trace from every cell so only the current one stays highlighted

  if (
    data.speed === undefined ||
    data.speed === null ||
    data.throttle === undefined ||
    data.throttle === null
  ) {
    return;
  }

  const speed = Number(data.speed); // get the current speed from the data
  const throttle = Number(data.throttle); // get the current throttle from the data

  if (Number.isNaN(speed) || Number.isNaN(throttle)) {
    return;
  }

  const throttlePos = arrayIndex(throttle, throttleHeader); // find the position in the grid for the current throttle
  const speedPos = arrayIndex(speed, speedHeader); // find the position in the grid for the current speed

  cell[speedPos + throttlePos * throttleHeader.length].classList.add(
    "activeTrace",
  ); // find the cell in the grid and update the class to activeTrace
}

// update the cell colours
function updateCellColor(cell, value) {
  cell.classList.remove("low", "medium", "high"); // remove all colour classes

  if (value < 30) {
    cell.classList.add("low"); // if the value is less than 30, add the 'low' class
  } else if (value < 60) {
    cell.classList.add("medium"); // if the value is between 30 and 60, add the 'medium' class
  } else {
    cell.classList.add("high"); // if the value is above 60, add the 'high' class
  }
}

// tune edit
function openEditValue(cell) {
  currentEditCell = cell;
  const modal = document.getElementById("editModal");
  const modalTitle = document.getElementById("editModalTitle");
  const input = document.getElementById("editValue");

  if (cell.getAttribute("speed") === "100") {
    modalTitle.textContent = "Edit Throttle Axis";
  }

  if (cell.getAttribute("throttle") === "100") {
    modalTitle.textContent = "Edit Speed Axis";
  }

  if (cell.getAttribute("lock") === "true") {
    modalTitle.textContent = "Edit Lock %";
  }

  input.value = cell.textContent;
  input.focus();
  input.select();

  modal.classList.add("active");
}

function cancelEdit() {
  document.getElementById("editModal").classList.remove("active");
  currentEditCell = null;
  editMultiMode = false;
}

function confirmEdit() {
  // Multi-cell: apply one lock value across the whole selection.
  if (editMultiMode) {
    const value = parseInt(document.getElementById("editValue").value);
    if (isNaN(value) || value < 0 || value > 100) {
      showNotification("Value must be between 0 and 100", "error");
      return;
    }
    selectedCells.forEach((k) => {
      const [r, c] = k.split(",").map(Number);
      if (!currentLock[r]) return;
      currentLock[r][c] = value;
      const cell = cellForRC(r, c);
      if (cell) {
        cell.textContent = String(value);
        updateCellColor(cell, value);
      }
    });
    cancelEdit();
    drawTuneChart();
    return;
  }

  if (!currentEditCell) return;

  const currentCell = document.getElementById("editValue"); // find the current edit cell

  const throttlePos = parseInt(currentEditCell.getAttribute("throttle")); // get the throttle var (position in grid)
  const speedPos = parseInt(currentEditCell.getAttribute("speed")); // get the speed var (position in grid)
  const lockTrue = currentEditCell.getAttribute("lock"); // get the lock var (position in grid)

  const value = parseInt(document.getElementById("editValue").value);

  if (speedPos === 100 && lockTrue === "false") {
    if (isNaN(value) || value < 0 || value > 100) {
      showNotification("Value must be between 0 and 100", "error");
      return;
    }
    throttleHeader[throttlePos] = Number(value);
  }
  if (throttlePos === 100 && lockTrue === "false") {
    if (isNaN(value) || value < 0) {
      showNotification("Value must be greater than 0", "error");
      return;
    }
    speedHeader[speedPos] = Number(value);
  }

  if (lockTrue === "true") {
    if (isNaN(value) || value < 0 || value > 100) {
      showNotification("Value must be between 0 and 100", "error");
      return;
    }
    currentLock[throttlePos][speedPos] = Number(value);
    updateCellColor(currentEditCell, value);
  }

  currentEditCell.textContent = value;
  cancelEdit();
  drawTuneChart(); // keep the surface in sync with the edited cell/axis
}
// end tune edit

function restoreDefaults() {
  // One tap away from Apply and it discards the whole in-editor tune - make
  // sure it was meant. (The device tune is untouched until Apply is hit.)
  if (!confirm("Replace the current editor tune with the factory default map? Your edits are lost unless already applied or saved to a slot.")) {
    return;
  }
  applyMapToEditor(defaultSpeedHeader, defaultThrottleHeader, defaultLock);
}

// Load a map (axes + lock table) into the editor: repaint the grid, drop any
// selection, and redraw the 3D surface. This does NOT push to the ESP - the
// device holds one tune, committed only when the user hits Apply.
function applyMapToEditor(speed, throttle, lock) {
  speedHeader = speed.slice();
  throttleHeader = throttle.slice();
  currentLock = lock.map((r) => r.slice());
  buildMapGrid();
  clearSelection();
  drawTuneChart();
}

// ---- Map library: on-device saved slots ------------------------------------
// Saved slots live on the ESP itself (a small fixed set of named slots in device
// NVS), so a tune saved from one phone is visible from any phone that connects.
// Loading a slot only fills the editor; the ESP's live tune is committed only
// when the user hits Apply. There is no phone-side storage and no file
// import/export - the device is the single source of truth.

// Cache of the device's slot list ([{index, name, used}, ...]) plus the max
// name length the device accepts. Refreshed from GET /api/maps whenever the
// dropdown repopulates so Save/Load/Delete act on current device state.
let deviceSlots = [];
let deviceNameMax = 23;

async function fetchDeviceSlots() {
  const data = await fetchJson("/api/maps");
  if (data && Array.isArray(data.slots)) {
    deviceSlots = data.slots;
    if (Number.isFinite(data.nameMax)) deviceNameMax = data.nameMax;
  } else {
    deviceSlots = []; // device unreachable - show presets only
  }
  return deviceSlots;
}

// Validate a map has 7x7 dims with strictly-ascending, finite axes and 0..100
// lock values. Returns a normalised {speed,throttle,lock} or null. This mirrors
// the firmware's ascending-axis guard so an imported map can't mis-interpolate.
function validateMap(m) {
  if (!m || !Array.isArray(m.speed) || !Array.isArray(m.throttle) || !Array.isArray(m.lock))
    return null;
  if (m.speed.length !== arrayColumns || m.throttle.length !== arrayRows) return null;
  if (m.lock.length !== arrayRows) return null;
  const speed = m.speed.map(Number);
  const throttle = m.throttle.map(Number);
  const asc = (a) =>
    a.every((v, i) => Number.isFinite(v) && (i === 0 || v > a[i - 1]));
  if (!asc(speed) || !asc(throttle)) return null;
  const lock = [];
  for (let r = 0; r < arrayRows; r++) {
    if (!Array.isArray(m.lock[r]) || m.lock[r].length !== arrayColumns) return null;
    lock[r] = m.lock[r].map((v) => {
      const n = Math.round(Number(v));
      return Number.isFinite(n) ? Math.max(0, Math.min(100, n)) : 0;
    });
  }
  return { speed, throttle, lock };
}

// Rebuild the dropdown from the device's saved slots. Slot options carry value
// "slot:N" (N = device slot index). Empty slots are shown greyed as "Slot N
// (empty)" so the user can see how many of the fixed slots remain.
async function populateMapDropdown(selectValue) {
  const sel = document.getElementById("mapPreset");
  if (!sel) return;
  await fetchDeviceSlots();
  sel.innerHTML = "";

  const ph = document.createElement("option");
  ph.value = "";
  ph.textContent = "Select a map...";
  sel.appendChild(ph);

  // Only device slots live here now, so list them directly - no optgroup
  // heading (with presets gone there is nothing to distinguish it from).
  // Empty slots stay SELECTABLE (not disabled): the dropdown doubles as the
  // "which slot do I save to?" picker, so the user must be able to pick an
  // empty slot as a Save target. Load/Delete reject an empty selection at
  // click time, so nothing breaks by leaving them enabled.
  deviceSlots.forEach((slot) => {
    const o = document.createElement("option");
    o.value = "slot:" + slot.index;
    o.textContent = slot.used
      ? `Slot ${slot.index + 1}: ${slot.name}`
      : `Slot ${slot.index + 1} (empty)`;
    sel.appendChild(o);
  });

  if (selectValue) sel.value = selectValue;
}

async function loadSelectedMap() {
  const sel = document.getElementById("mapPreset");
  if (!sel || !sel.value) {
    showNotification("Pick a map to load first", "error");
    return;
  }
  const key = sel.value.slice(sel.value.indexOf(":") + 1);

  // Device slot: pull the tune off the ESP, then drop it into the editor.
  const data = await fetchJson("/api/maps/get?index=" + encodeURIComponent(key));
  if (!data || !data.ok) {
    showNotification("That slot is empty", "error");
    return;
  }
  const m = validateMap({
    speed: data.speedArray,
    throttle: data.throttleArray,
    lock: data.lockArray,
  });
  if (!m) {
    showNotification("Stored map is invalid", "error");
    return;
  }
  applyMapToEditor(m.speed, m.throttle, m.lock);
  showNotification(`Loaded "${data.name}" - hit Apply to keep it`);
}

// Save the editor's current map into a device slot. Targets the selected slot if
// one is picked, otherwise the first free slot; if all slots are full and none
// is selected, tells the user to pick one to overwrite.
async function saveMapAs() {
  const sel = document.getElementById("mapPreset");
  let targetIndex = -1;

  if (sel && sel.value.startsWith("slot:")) {
    targetIndex = parseInt(sel.value.slice(5), 10);
  } else {
    const free = deviceSlots.find((s) => !s.used);
    if (free) {
      targetIndex = free.index;
    } else {
      showNotification(
        "All slots full - select a slot in the list to overwrite",
        "error",
      );
      return;
    }
  }

  const existing = deviceSlots.find((s) => s.index === targetIndex);
  if (existing && existing.used) {
    if (!confirm(`Overwrite "${existing.name}" (slot ${targetIndex + 1})?`))
      return;
  }

  let name = (prompt("Save map as:", existing && existing.used ? existing.name : "") || "").trim();
  if (!name) return;
  if (name.length > deviceNameMax) name = name.slice(0, deviceNameMax);

  const res = await fetchJson("/api/maps/save", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      index: targetIndex,
      name,
      speedArray: speedHeader,
      throttleArray: throttleHeader,
      lockArray: currentLock.map((r) => r),
    }),
  });
  if (res && res.ok) {
    await populateMapDropdown("slot:" + targetIndex);
    showNotification(`Saved "${name}" to slot ${targetIndex + 1}`);
  } else {
    showNotification((res && res.error) || "Save failed", "error");
  }
}

async function deleteSelectedMap() {
  const sel = document.getElementById("mapPreset");
  if (!sel || !sel.value.startsWith("slot:")) {
    showNotification("Pick a saved slot to delete", "error");
    return;
  }
  const idx = parseInt(sel.value.slice(5), 10);
  const slot = deviceSlots.find((s) => s.index === idx);
  if (!slot || !slot.used) {
    showNotification("That slot is already empty", "error");
    return;
  }
  if (!confirm(`Delete "${slot.name}" from slot ${idx + 1}?`)) return;

  const res = await fetchJson("/api/maps/delete", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ index: idx }),
  });
  if (res && res.ok) {
    await populateMapDropdown("");
    showNotification(`Deleted slot ${idx + 1}`);
  } else {
    showNotification("Delete failed", "error");
  }
}

function initMapManager() {
  populateMapDropdown("");
  const bind = (id, fn) => {
    const el = document.getElementById(id);
    if (el) el.addEventListener("click", fn);
  };
  bind("mapLoad", loadSelectedMap);
  bind("mapSaveAs", saveMapAs);
  bind("mapDelete", deleteSelectedMap);
}

// ---- Dashboard: user-selectable glance tiles -------------------------------
// The glance grid holds a fixed catalogue of tiles (each carries data-signal).
// The user picks which ones show; the choice persists in this browser. Values
// are still populated by refreshStatus regardless - we only toggle visibility.

const DASH_TILE_KEY = "ohEdgeDashTiles";

// id -> label, in grid order. Keep in sync with the data-signal attributes in
// index.html's glance grid.
const DASH_SIGNALS = [
  { id: "udsClutchTemp", label: "Clutch Temp" },
  { id: "udsModuleTemp", label: "Module Temp" },
  { id: "udsCoolingFinTemp", label: "Cooling Fin" },
  { id: "speed", label: "Speed" },
  { id: "throttle", label: "Throttle" },
  { id: "rpm", label: "RPM" },
  { id: "boost", label: "Boost" },
  { id: "haldexEngagement", label: "Reported lock" },
  { id: "udsClutchPWM", label: "Clutch PWM" },
  { id: "steeringAngle", label: "Steering" },
  { id: "steeringGainNow", label: "Steer Gain" },
];

const DASH_DEFAULT_ON = [
  "udsClutchTemp",
  "udsModuleTemp",
  "udsCoolingFinTemp",
  "speed",
  "throttle",
  "boost",
  "haldexEngagement",
  "rpm",
];

function loadDashSelection() {
  try {
    const raw = localStorage.getItem(DASH_TILE_KEY);
    if (raw) {
      const arr = JSON.parse(raw);
      if (Array.isArray(arr)) return new Set(arr);
    }
  } catch (e) {
    /* fall through to defaults */
  }
  return new Set(DASH_DEFAULT_ON);
}

function saveDashSelection(set) {
  try {
    localStorage.setItem(DASH_TILE_KEY, JSON.stringify([...set]));
  } catch (e) {
    /* storage full/blocked - selection just won't persist */
  }
}

function applyDashSelection(set) {
  DASH_SIGNALS.forEach((sig) => {
    const tile = document.querySelector(
      `.glance-card [data-signal="${sig.id}"]`,
    );
    if (tile) tile.style.display = set.has(sig.id) ? "" : "none";
  });
}

function buildDashCustomizer(set) {
  const host = document.getElementById("dashTileOptions");
  if (!host) return;
  host.innerHTML = "";
  DASH_SIGNALS.forEach((sig) => {
    const label = document.createElement("label");
    label.className = "tile-opt";
    const cb = document.createElement("input");
    cb.type = "checkbox";
    cb.checked = set.has(sig.id);
    cb.addEventListener("change", () => {
      if (cb.checked) set.add(sig.id);
      else set.delete(sig.id);
      saveDashSelection(set);
      applyDashSelection(set);
    });
    const span = document.createElement("span");
    span.textContent = sig.label;
    label.appendChild(cb);
    label.appendChild(span);
    host.appendChild(label);
  });
}

function initDashTiles() {
  const set = loadDashSelection();
  applyDashSelection(set);
  buildDashCustomizer(set);
}

// ---- Tune surface (pseudo-3D) ----------------------------------------------
// Renders currentLock as an isometric SVG surface: speed and throttle are the
// two ground axes, lock% is the height, and each facet is shaded on the same
// green -> amber -> red heat ramp as the editor grid so the table and the
// surface read the same. A dot rides the surface at the live operating point.
// Plain SVG, no libraries - the page ships from the ESP32's flash.
let lastDashData = null;

// Projection closure built by drawTuneChart and reused by updateChartMarker so
// the live dot lands on exactly the surface that was drawn.
let tuneProject = null;

// Heat ramp shared by the surface and the editor grid: green (low lock) through
// amber to red (high lock). Returns an "r,g,b" triple for a 0..1 fraction.
function heatRGB(frac) {
  const stops = [
    [16, 185, 129],  // green (#10b981) - low lock
    [245, 158, 11],  // amber (#f59e0b) - mid
    [220, 38, 38],   // red   (#dc2626) - high lock
  ];
  const f = Math.min(1, Math.max(0, frac));
  const pos = f * (stops.length - 1);
  const i = Math.min(stops.length - 2, Math.floor(pos));
  const t = pos - i;
  const mix = stops[i].map((c, k) => Math.round(c + (stops[i + 1][k] - c) * t));
  return `${mix[0]},${mix[1]},${mix[2]}`;
}

// Linear interpolation helper over an ascending header array. Returns the
// fractional index for a value, clamped to the ends (mirrors the firmware's
// bracket scan).
function fracIndex(value, header) {
  if (value <= header[0]) return 0;
  const last = header.length - 1;
  if (value >= header[last]) return last;
  for (let i = 0; i < last; i++) {
    if (value < header[i + 1]) {
      return i + (value - header[i]) / (header[i + 1] - header[i]);
    }
  }
  return last;
}

function initTuneChart() {
  drawTuneChart();
}

function drawTuneChart() {
  const host = document.getElementById("tuneChart");
  const legend = document.getElementById("chartLegend");
  if (!host) return;

  const cols = speedHeader.length;    // speed breakpoints (c axis)
  const rows = throttleHeader.length; // throttle breakpoints (r axis)
  const W = 340, H = 250;

  // Perspective camera. The grid lies flat on the ground (speed = x across,
  // throttle = depth into the scene), lock lifts straight up. We yaw the ground
  // clockwise, tilt the camera down to a 3/4 view, then divide by depth so the
  // far edge converges. That convergence is what makes the surface read as
  // sitting flat on the ground - a parallel projection keeps the far edge as
  // wide as the near one, which is why it looked like it was floating.
  const yaw = -30 * Math.PI / 180;  // counter-clockwise orbit of the ground plane
  const pitch = 26 * Math.PI / 180; // camera tilt: 90 = top-down, 0 = side-on; ~26 = front 3/4
  const sinYaw = Math.sin(yaw), cosYaw = Math.cos(yaw);
  const sinPit = Math.sin(pitch), cosPit = Math.cos(pitch);
  const midC = (cols - 1) / 2, midR = (rows - 1) / 2;
  const heightUnits = (cols - 1) * 0.6; // full-lock peak height in grid units
  const camDist = (cols - 1) * 2.4;     // smaller = stronger perspective
  // Returns screen x/y (pre-fit) plus camera depth for painter ordering.
  const world = (c, r, lock) => {
    const gx = c - midC;                  // centre the grid so it rotates in place
    const gy = r - midR;
    const gz = (lock / 100) * heightUnits;
    const x = gx * cosYaw + gy * sinYaw;  // yaw about the vertical axis
    const y = -gx * sinYaw + gy * cosYaw;
    const up = gz * cosPit + y * sinPit;      // screen-up: height, plus receding ground
    const depth = y * cosPit - gz * sinPit;   // into the scene: far ground, less for peaks
    const persp = camDist / (camDist + depth);
    return { x: x * persp, y: -up * persp, depth };
  };

  // Fit the whole surface (plus its lock-0 floor corners) into the viewBox by
  // measuring the projected bounding box, then scaling and centring to it.
  const pts = [];
  for (let r = 0; r < rows; r++) {
    for (let c = 0; c < cols; c++) pts.push(world(c, r, currentLock[r][c]));
  }
  pts.push(world(0, 0, 0), world(cols - 1, 0, 0), world(0, rows - 1, 0), world(cols - 1, rows - 1, 0));
  let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
  pts.forEach((p) => {
    if (p.x < minX) minX = p.x;
    if (p.x > maxX) maxX = p.x;
    if (p.y < minY) minY = p.y;
    if (p.y > maxY) maxY = p.y;
  });
  const mX = 16, mTop = 12, mBot = 30; // room for the axis captions along the base
  const scale = Math.min((W - 2 * mX) / (maxX - minX || 1), (H - mTop - mBot) / (maxY - minY || 1));
  const ox = (W - (maxX - minX) * scale) / 2 - minX * scale;
  const oy = mTop - minY * scale;
  tuneProject = (c, r, lock) => {
    const w = world(c, r, lock);
    return { x: ox + w.x * scale, y: oy + w.y * scale };
  };

  let svg = `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Lock map surface: speed and throttle on the ground, lock percent as height">`;

  // Facets painter-ordered far-to-near by camera depth so nearer cells overdraw
  // farther ones. Each facet is shaded by the mean lock of its corners. The fill
  // is translucent and every facet carries a light gridline edge: where a peak
  // overhangs a dip behind it the stacked fills composite toward opaque, so the
  // wireframe edges are what keep the hidden dip readable through the surface.
  const facets = [];
  for (let r = 0; r < rows - 1; r++) {
    for (let c = 0; c < cols - 1; c++) {
      const depth =
        (world(c, r, currentLock[r][c]).depth +
          world(c + 1, r, currentLock[r][c + 1]).depth +
          world(c, r + 1, currentLock[r + 1][c]).depth +
          world(c + 1, r + 1, currentLock[r + 1][c + 1]).depth) / 4;
      facets.push({ r, c, depth });
    }
  }
  facets.sort((a, b) => b.depth - a.depth);
  facets.forEach(({ r, c }) => {
    const corners = [[r, c], [r, c + 1], [r + 1, c + 1], [r + 1, c]];
    const poly = corners
      .map(([rr, cc]) => {
        const p = tuneProject(cc, rr, currentLock[rr][cc]);
        return `${p.x.toFixed(1)},${p.y.toFixed(1)}`;
      })
      .join(" ");
    const avg = (currentLock[r][c] + currentLock[r][c + 1] + currentLock[r + 1][c] + currentLock[r + 1][c + 1]) / 4;
    svg += `<polygon points="${poly}" fill="rgb(${heatRGB(avg / 100)})" fill-opacity="0.55" stroke="rgba(0,0,0,0.7)" stroke-width="0.5" stroke-linejoin="round"/>`;
  });

  // Floor footprint: outline the full lock-0 square under the surface, then drop
  // a dotted line from each surface corner down to the matching base corner so
  // the height of the four corners reads against the ground.
  const floorPt = (c, r) => tuneProject(c, r, 0);
  const fc = {
    near:  floorPt(0, rows - 1),          // speed 0,   throttle max (front-left)
    right: floorPt(cols - 1, rows - 1),   // speed max, throttle max (front-right)
    back:  floorPt(cols - 1, 0),          // speed max, throttle 0   (back-right)
    left:  floorPt(0, 0),                 // speed 0,   throttle 0   (back-left)
  };
  const floorLine = (pa, pb) =>
    `<line x1="${pa.x.toFixed(1)}" y1="${pa.y.toFixed(1)}" x2="${pb.x.toFixed(1)}" y2="${pb.y.toFixed(1)}" stroke="#4b5563" stroke-width="1"/>`;
  svg += floorLine(fc.left, fc.back) + floorLine(fc.back, fc.right) +
         floorLine(fc.right, fc.near) + floorLine(fc.near, fc.left);

  const corners3d = [[0, 0], [cols - 1, 0], [0, rows - 1], [cols - 1, rows - 1]];
  corners3d.forEach(([c, r]) => {
    const topP = tuneProject(c, r, currentLock[r][c]);
    const footP = tuneProject(c, r, 0);
    svg += `<line x1="${footP.x.toFixed(1)}" y1="${footP.y.toFixed(1)}" x2="${topP.x.toFixed(1)}" y2="${topP.y.toFixed(1)}" stroke="#6b7280" stroke-width="0.8" stroke-dasharray="2 2"/>`;
  });

  const speedEdge = { pa: fc.near, pb: fc.right };
  const throttleEdge = { pa: fc.back, pb: fc.right };

  // Axis captions just outside their edge midpoints, and the end values so the
  // surface reads with real numbers rather than bare geometry.
  const speedMid = tuneProject((cols - 1) / 2, rows - 1, 0);
  const throttleMid = tuneProject(cols - 1, (rows - 1) / 2, 0);
  svg += `<text x="${speedMid.x.toFixed(1)}" y="${(speedMid.y + 16).toFixed(1)}" text-anchor="middle" font-size="8" font-weight="700" fill="#9ca3af">SPEED (KM/H)</text>`;
  svg += `<text x="${(throttleMid.x + 6).toFixed(1)}" y="${(throttleMid.y + 14).toFixed(1)}" text-anchor="start" font-size="8" font-weight="700" fill="#9ca3af">THROTTLE (%)</text>`;
  svg += `<text x="${(speedEdge.pa.x - 3).toFixed(1)}" y="${(speedEdge.pa.y + 10).toFixed(1)}" text-anchor="end" font-size="7.5" fill="#6b7280">${speedHeader[0]}</text>`;
  svg += `<text x="${(speedEdge.pb.x + 3).toFixed(1)}" y="${(speedEdge.pb.y + 10).toFixed(1)}" text-anchor="start" font-size="7.5" fill="#6b7280">${speedHeader[cols - 1]}</text>`;
  svg += `<text x="${(throttleEdge.pa.x + 4).toFixed(1)}" y="${(throttleEdge.pa.y + 2).toFixed(1)}" text-anchor="start" font-size="7.5" fill="#6b7280">${throttleHeader[0]}</text>`;

  // Live operating point: a dashed stem from the floor up to a dot riding the
  // surface. Both are positioned by updateChartMarker.
  svg += `<line id="chartMarkerStem" stroke="#fff" stroke-width="1" stroke-dasharray="2 2" visibility="hidden"/>`;
  svg += `<circle id="chartMarker" r="4" fill="#dc2626" stroke="#fff" stroke-width="1.5" visibility="hidden"/>`;
  // Dots for the currently selected editor cells, filled by updateSelectionMarkers.
  svg += `<g id="selDots"></g>`;
  svg += `</svg>`;

  host.innerHTML = svg;

  if (legend) {
    legend.innerHTML =
      `<div class="chart-legend-caption">Lock % (surface height &amp; colour)</div>` +
      `<div class="chart-legend-bar" style="background:linear-gradient(90deg, rgb(${heatRGB(0)}), rgb(${heatRGB(0.5)}), rgb(${heatRGB(1)}))"></div>` +
      `<div class="chart-legend-scale"><span>0</span><span>50</span><span>100</span></div>`;
  }
  updateChartMarker();
  updateSelectionMarkers();
}

function updateChartMarker() {
  const marker = document.getElementById("chartMarker");
  const stem = document.getElementById("chartMarkerStem");
  if (!marker || !tuneProject || !lastDashData) return;

  const speed = Number(lastDashData.speed);
  const throttle = Number(lastDashData.throttle);
  if (Number.isNaN(speed) || Number.isNaN(throttle)) {
    marker.setAttribute("visibility", "hidden");
    if (stem) stem.setAttribute("visibility", "hidden");
    return;
  }

  const c = fracIndex(speed, speedHeader);
  const r = fracIndex(throttle, throttleHeader);

  // Land the dot on the drawn surface by bilinearly blending the four projected
  // facet corners it sits between, so it rides the rendered grid exactly rather
  // than floating above it.
  const c0 = Math.floor(c), c1 = Math.min(speedHeader.length - 1, c0 + 1), tc = c - c0;
  const r0 = Math.floor(r), r1 = Math.min(throttleHeader.length - 1, r0 + 1), tr = r - r0;
  const lerp = (a, b, t) => a + (b - a) * t;
  const blend = (q00, q10, q01, q11) => ({
    x: lerp(lerp(q00.x, q10.x, tc), lerp(q01.x, q11.x, tc), tr),
    y: lerp(lerp(q00.y, q10.y, tc), lerp(q01.y, q11.y, tc), tr),
  });
  const corner = (cc, rr) => tuneProject(cc, rr, currentLock[rr][cc]);
  const top = blend(corner(c0, r0), corner(c1, r0), corner(c0, r1), corner(c1, r1));
  // Blend the projected floor corners the same way, so the stem foot rides the
  // drawn floor grid lines instead of the perspective-curved path a fractional
  // tuneProject(c, r, 0) would take, which drifts off the straight mesh edges.
  const floor = (cc, rr) => tuneProject(cc, rr, 0);
  const foot = blend(floor(c0, r0), floor(c1, r0), floor(c0, r1), floor(c1, r1));

  marker.setAttribute("cx", top.x.toFixed(1));
  marker.setAttribute("cy", top.y.toFixed(1));
  marker.setAttribute("visibility", "visible");
  if (stem) {
    stem.setAttribute("x1", foot.x.toFixed(1));
    stem.setAttribute("y1", foot.y.toFixed(1));
    stem.setAttribute("x2", top.x.toFixed(1));
    stem.setAttribute("y2", top.y.toFixed(1));
    stem.setAttribute("visibility", "visible");
  }
}

// PAL-friendly explainers for the Calibrate tab. Keyed by the data-info value on
// each .calib-info button; opened in the shared #calibInfoModal. Kept as plain
// strings (no HTML) so the copy stays readable and can't inject markup.
// ---- Calibrate > Car geometry (per-car slip geometry) -------------------
// Firmware keys: slipWheelbaseMm, slipTrackFrontMm, slipTrackRearMm,
// slipSteeringRatio, slipMinSpeedRaw. The speed floor is stored in raw wheel
// speed units (1 unit = 0.0075 km/h), shown here in km/h.
const GEO_DEFAULTS = { wheelbase: 2505, trackFront: 1572, trackRear: 1543, ratio: 15.0, minKmh: 5.0 };
const GEO_RAW_PER_KMH = 1 / 0.0075;

function geoFields() {
  return {
    wheelbase: document.getElementById("geoWheelbase"),
    trackFront: document.getElementById("geoTrackFront"),
    trackRear: document.getElementById("geoTrackRear"),
    ratio: document.getElementById("geoSteerRatio"),
    minKmh: document.getElementById("geoMinSpeed"),
  };
}

function populateGeometry(data) {
  const f = geoFields();
  if (!f.wheelbase) return;
  if (data.slipWheelbaseMm !== undefined) f.wheelbase.value = data.slipWheelbaseMm;
  if (data.slipTrackFrontMm !== undefined) f.trackFront.value = data.slipTrackFrontMm;
  if (data.slipTrackRearMm !== undefined) f.trackRear.value = data.slipTrackRearMm;
  if (data.slipSteeringRatio !== undefined) f.ratio.value = Number(data.slipSteeringRatio).toFixed(1);
  if (data.slipMinSpeedRaw !== undefined) f.minKmh.value = (data.slipMinSpeedRaw / GEO_RAW_PER_KMH).toFixed(1);
}

function initGeometry() {
  const f = geoFields();
  const save = document.getElementById("geoSave");
  const reset = document.getElementById("geoReset");
  const status = document.getElementById("geoStatus");
  if (!f.wheelbase || !save || !reset) return;

  const setStatus = (msg, ok) => {
    if (!status) return;
    status.textContent = msg;
    status.style.color = ok ? "var(--success)" : "var(--danger)";
  };
  const num = (el, lo, hi) => {
    const v = parseFloat(el.value);
    return Number.isFinite(v) && v >= lo && v <= hi ? v : null;
  };

  save.addEventListener("click", async () => {
    const wb = num(f.wheelbase, 1500, 4000);
    const tf = num(f.trackFront, 1000, 2500);
    const tr = num(f.trackRear, 1000, 2500);
    const ratio = num(f.ratio, 8, 30);
    const kmh = num(f.minKmh, 0, 30);
    if (wb === null) { setStatus("Wheelbase must be 1500 to 4000 mm", false); return; }
    if (tf === null) { setStatus("Front track must be 1000 to 2500 mm", false); return; }
    if (tr === null) { setStatus("Rear track must be 1000 to 2500 mm", false); return; }
    if (ratio === null) { setStatus("Steering ratio must be 8 to 30", false); return; }
    if (kmh === null) { setStatus("Minimum speed must be 0 to 30 km/h", false); return; }
    const ok = await saveGeometry({ wb, tf, tr, ratio, kmh });
    setStatus(ok ? "Saved" : "Could not save - check the connection", ok);
  });

  reset.addEventListener("click", async () => {
    const d = GEO_DEFAULTS;
    f.wheelbase.value = d.wheelbase;
    f.trackFront.value = d.trackFront;
    f.trackRear.value = d.trackRear;
    f.ratio.value = d.ratio.toFixed(1);
    f.minKmh.value = d.minKmh.toFixed(1);
    const ok = await saveGeometry({ wb: d.wheelbase, tf: d.trackFront, tr: d.trackRear, ratio: d.ratio, kmh: d.minKmh });
    setStatus(ok ? "Reset to Audi TT Mk3 values" : "Could not save - check the connection", ok);
  });
}

async function saveGeometry(g) {
  const resp = await fetchJson("/api/settings", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      slipWheelbaseMm: Math.round(g.wb),
      slipTrackFrontMm: Math.round(g.tf),
      slipTrackRearMm: Math.round(g.tr),
      slipSteeringRatio: Math.round(g.ratio * 10) / 10,
      slipMinSpeedRaw: Math.round(g.kmh * GEO_RAW_PER_KMH),
    }),
  });
  return !!(resp && resp.ok);
}

const CALIB_INFO = {
  gen: {
    title: "Generation",
    body:
      "This tells the controller which type of Haldex is fitted to your car. It sets how the controller reads the Haldex over CAN and how it commands lock.\n\n" +
      "It MUST match your car. If it is wrong, lock control will not work at all.\n\n" +
      "MQB cars (e.g. Mk3 TT, 8V S3, MQB Golf R) are Generation 5 (0CQ). The 0AY variant is the older PQ-derived unit. If you are not sure, check what Haldex generation your car uses before changing anything else.",
  },
  learn: {
    title: "Learn Haldex",
    body:
      "Every Haldex responds slightly differently. The Learn finds out how YOUR one behaves so that when you ask for a lock amount, you actually get it.\n\n" +
      "It slowly steps the command from 0% up to 100% and records how much the Haldex actually engages at each step, building a calibration table that replaces the built-in estimate.\n\n" +
      "How to run it:\n" +
      "1. Engine running.\n" +
      "2. Car stationary (it stops itself if you move off).\n" +
      "3. Haldex CAN connected and healthy.\n" +
      "4. Press Learn Haldex and wait - it takes a couple of minutes.\n\n" +
      "Until you have run a Learn, the controller is guessing, and the car usually drives BETTER left in Stock than run uncalibrated.",
  },
  ceiling: {
    title: "Lock calibration",
    body:
      "This lines up the lock you ask for with the lock the Haldex actually delivers, so 50% means 50%.\n\n" +
      "It is NOT a power or strength dial, and it is NOT your engine's torque. There is one correct value: the one where commanded and delivered match 1:1.\n\n" +
      "How to set it: run a Learn, look at the chart, and adjust this until the Sent vs Returned line sits on the dashed 1:1 diagonal.\n\n" +
      "Higher is not better. Too high and the coupling grabs early and slams shut; too low and it under-delivers. Once set, it applies whenever you drive.",
  },
  floor: {
    title: "Launch PWM floor (experimental)",
    body:
      "Optional, experimental. It holds a minimum clutch PWM (solenoid duty) while lock is commanded, so engagement builds faster off the line - closer to what Stock reaches under launch.\n\n" +
      "0% = off (default, unchanged behaviour). Leave it there unless you specifically want a firmer launch.\n\n" +
      "It only applies while lock is actually commanded - off-throttle, FWD, and coasting still open the clutch, so it does not force the car to be always-engaged.\n\n" +
      "How to tune it: watch the Clutch PWM readout on the Dashboard and raise this until PWM climbs to where you want. If it grabs mid-corner, back it off. It is a floor only - the Haldex still modulates above it.",
  },
};

// Whether the current session has dismissed the not-calibrated banner. Resets on
// reload so an uncalibrated car nags again next time the UI is opened.
let calibBannerDismissed = false;

// Monotonic revision so a slow in-flight /api/learn/status response can't clobber
// a newer state. Every call to applyCalibrationState bumps it; async reads capture
// the value before their fetch and only apply if it's still current (see
// applyCalibrationStateFromFetch).
let calibStateRev = 0;

// Apply a calibration state read from an async /api/learn/status fetch, but only
// if no newer authoritative update landed while the request was in flight. Callers
// capture calibStateRev before the fetch and pass it here on resolve.
function applyCalibrationStateFromFetch(tableValid, revAtRequest) {
  if (revAtRequest !== calibStateRev) return; // a newer update already won
  applyCalibrationState(tableValid);
}

// Single source of truth for the calibrated/uncalibrated UI state. Driven by the
// tableValid flag from /api/learn/status, called from every place that resolves
// it (page load, learn complete, clear). Updates both the Dashboard banner and
// the Calibrate-tab status chip so they never disagree.
function applyCalibrationState(tableValid) {
  calibStateRev++; // this is now the latest word; invalidate older in-flight reads
  const banner = document.getElementById("calibBanner");
  if (banner) {
    const show = !tableValid && !calibBannerDismissed;
    banner.hidden = !show;
  }

  const chip = document.getElementById("calibStatusChip");
  const chipText = document.getElementById("calibStatusChipText");
  if (chip && chipText) {
    if (tableValid) {
      chip.classList.remove("uncalibrated");
      chip.classList.add("calibrated");
      chipText.textContent = "Calibrated - learn table active";
    } else {
      chip.classList.remove("calibrated");
      chip.classList.add("uncalibrated");
      chipText.textContent = "Not calibrated - run the Learn (results worse than Stock until you do)";
    }
  }
}

// initialise the Calibrate tab: info modals, the not-calibrated banner controls,
// and an initial calibration-state read.
function initCalibrate() {
  const modal   = document.getElementById("calibInfoModal");
  const title   = document.getElementById("calibInfoTitle");
  const body    = document.getElementById("calibInfoBody");
  const btnClose = document.getElementById("calibInfoClose");

  // The (i) button that opened the modal, so focus can be restored on close.
  let infoTrigger = null;

  function openInfo(key, trigger) {
    const info = CALIB_INFO[key];
    if (!info || !modal) return;
    infoTrigger = trigger || null;
    title.textContent = info.title;
    // Preserve paragraph breaks from the copy without injecting HTML.
    body.textContent = "";
    info.body.split("\n\n").forEach((para) => {
      const p = document.createElement("p");
      p.textContent = para;
      body.appendChild(p);
    });
    modal.classList.add("active");
    if (btnClose) btnClose.focus(); // move focus into the dialog
  }

  function closeInfo() {
    if (modal) modal.classList.remove("active");
    // Return focus to the (i) button the user came from.
    if (infoTrigger) infoTrigger.focus();
    infoTrigger = null;
  }

  document.querySelectorAll(".calib-info").forEach((btn) => {
    btn.addEventListener("click", () => openInfo(btn.dataset.info, btn));
  });
  if (btnClose) btnClose.addEventListener("click", closeInfo);
  if (modal) {
    modal.addEventListener("click", (e) => {
      if (e.target === modal) closeInfo(); // tap the backdrop to dismiss
    });
    // Keyboard support while the dialog is open: Escape closes it, Tab is
    // trapped so focus can't wander to the page behind the backdrop.
    modal.addEventListener("keydown", (e) => {
      if (!modal.classList.contains("active")) return;
      if (e.key === "Escape") {
        closeInfo();
        return;
      }
      if (e.key === "Tab") {
        const focusable = modal.querySelectorAll(
          'button, [href], input, select, textarea, [tabindex]:not([tabindex="-1"])'
        );
        if (!focusable.length) return;
        const first = focusable[0];
        const last = focusable[focusable.length - 1];
        if (e.shiftKey && document.activeElement === first) {
          e.preventDefault();
          last.focus();
        } else if (!e.shiftKey && document.activeElement === last) {
          e.preventDefault();
          first.focus();
        }
      }
    });
  }

  // Not-calibrated Dashboard banner controls.
  const dismissBtn = document.getElementById("calibBannerDismiss");
  const openBtn    = document.getElementById("calibBannerOpen");
  if (dismissBtn) {
    dismissBtn.addEventListener("click", () => {
      calibBannerDismissed = true;
      const banner = document.getElementById("calibBanner");
      if (banner) banner.hidden = true;
    });
  }
  if (openBtn) {
    openBtn.addEventListener("click", () => {
      const tab = document.querySelector('.nav-tab[data-page="calibrate"]');
      if (tab) tab.click();
    });
  }

  // Seed the initial state; initLearn's own status reads keep it current after.
  // Guarded so a slow response can't overwrite a newer state set by the learn
  // poll or a clear that resolved first.
  const seedRev = calibStateRev;
  fetchJson("/api/learn/status").then((data) => {
    if (data) applyCalibrationStateFromFetch(!!data.tableValid, seedRev);
  });
}

// initialise Learn Haldex UI
function initLearn() {
  let learnPollInterval = null;
  let pollInFlight = false;

  const statusText    = document.getElementById("learnStatusText");
  const progressWrap  = document.getElementById("learnProgressWrap");
  const progressFill  = document.getElementById("learnProgressFill");
  const progressLabel = document.getElementById("learnProgressLabel");
  const learnTrack    = document.getElementById("learnTrack");
  const learnCFFill   = document.getElementById("learnCFFill");
  const learnEngFill  = document.getElementById("learnEngFill");
  const learnCFValue  = document.getElementById("learnCFValue");
  const learnEngValue = document.getElementById("learnEngValue");
  const btnStart      = document.getElementById("learnStart");
  const btnCancel     = document.getElementById("learnCancel");
  const btnClear      = document.getElementById("learnClear");

  function setProgress(pct) {
    progressFill.style.width = pct + "%";
    progressLabel.textContent = pct + "%";
  }

  function setTracking(cf, eng) {
    learnCFFill.style.width   = cf  + "%";
    learnEngFill.style.width  = eng + "%";
    learnCFValue.textContent  = cf;
    learnEngValue.textContent = eng;
  }

  function startPolling() {
    btnStart.style.display     = "none";
    btnCancel.style.display    = "";
    progressWrap.style.display = "";
    learnTrack.style.display   = "";
    setProgress(0);
    setTracking(0, 0);
    learnPollInterval = setInterval(pollStatus, 100);
  }

  function stopPolling() {
    clearInterval(learnPollInterval);
    learnPollInterval          = null;
    btnStart.style.display     = "";
    btnCancel.style.display    = "none";
    progressWrap.style.display = "none";
    learnTrack.style.display   = "none";
  }

  async function pollStatus() {
    // one poll at a time, and bound a stalled request, so the 100ms interval
    // can't stack requests and flood the async web server (same as refreshStatus)
    if (pollInFlight) return;
    pollInFlight = true;
    const ctrl = new AbortController();
    const pollTimeout = setTimeout(() => ctrl.abort(), POLL_TIMEOUT_MS);
    try {
      const data = await fetchJson("/api/learn/status", { signal: ctrl.signal });
      if (!data) return; // fetch failed or timed out - skip this cycle

      const pct = Math.min(100, Math.round(data.progress));
      setProgress(pct);
      setTracking(data.currentCF ?? 0, data.currentEng ?? 0);

      if (!data.active) {
        stopPolling();
        if (data.progress === 102) {
          statusText.textContent = "No Haldex CAN data recorded - check connection";
          statusText.style.color = "var(--danger)";
        } else if (data.progress === 103) {
          statusText.textContent = "Learn aborted - vehicle started moving. Stop the car and retry.";
          statusText.style.color = "var(--danger)";
        } else if (data.tableValid) {
          statusText.textContent = "Learn complete \u2713 - calibration table active";
          statusText.style.color = "var(--success)";
          renderLearnChart(data.table); // draw the freshly-learned curve
        } else {
          statusText.textContent = "Learn cancelled or failed";
          statusText.style.color = "var(--warning)";
        }
        applyCalibrationState(!!data.tableValid);
      }
    } finally {
      clearTimeout(pollTimeout);
      pollInFlight = false;
    }
  }

  btnStart.addEventListener("click", async () => {
    statusText.textContent = "Learning...";
    statusText.style.color = "var(--text-dim)";
    const resp = await fetchJson("/api/learn/start", { method: "POST" });
    if (!resp || !resp.ok) {
      statusText.textContent = resp && resp.error ? resp.error : "Failed to start learn";
      statusText.style.color = "var(--danger)";
      return;
    }
    startPolling();
  });

  btnCancel.addEventListener("click", async () => {
    await fetchJson("/api/learn/cancel", { method: "POST" });
    stopPolling();
    statusText.textContent = "Learn cancelled";
    statusText.style.color = "var(--warning)";
  });

  btnClear.addEventListener("click", async () => {
    // Destroys a calibration that takes a full stationary sweep to rebuild,
    // and the button sits right under Learn/Cancel - confirm before wiping.
    if (!confirm("Clear the learned calibration table? The controller reverts to the static factor until you run Learn again.")) {
      return;
    }
    const resp = await fetchJson("/api/learn/clear", { method: "POST" });
    if (!resp || !resp.ok) {
      // Clear failed (network error or endpoint rejected) - leave the current
      // calibration UI untouched rather than pretending the table is gone.
      statusText.textContent = "Clear failed - try again";
      statusText.style.color = "var(--warning)";
      return;
    }
    statusText.textContent = "Learn data cleared - static factor active";
    statusText.style.color = "var(--text-dim)";
    renderLearnChart([]); // table gone - hide the chart
    calibBannerDismissed = false; // fresh uncalibrated state - let it nag again
    applyCalibrationState(false); // table wiped - nag again
  });

  // check initial state on page load
  fetchJson("/api/learn/status").then((data) => {
    if (!data) return;
    if (data.active) {
      statusText.textContent = "Learning...";
      statusText.style.color = "var(--text-dim)";
      startPolling();
    } else if (data.tableValid) {
      statusText.textContent = "Learn complete \u2713 - calibration table active";
      statusText.style.color = "var(--success)";
      renderLearnChart(data.table); // show the stored curve on page load
    } else {
      statusText.textContent = "No learn data - static factor active";
      statusText.style.color = "var(--text-dim)";
    }
  });
}

// initialise WiFi SSID section
function initWifiSsid() {
  const input    = document.getElementById("wifiSsidInput");
  const status   = document.getElementById("wifiSsidStatus");
  const btnSave  = document.getElementById("wifiSsidSave");
  const btnReset = document.getElementById("wifiSsidReset");
  if (!input || !status || !btnSave || !btnReset) return;

  let defaultSsid = "OpenHaldex-C6";

  function renderStatus(ssid) {
    if (!ssid) {
      status.textContent = "--";
      status.style.color = "var(--text-dim)";
      return;
    }
    if (ssid === defaultSsid) {
      status.textContent = "Default SSID: " + ssid;
      status.style.color = "var(--text-dim)";
    } else {
      status.textContent = "\u2713 Custom SSID: " + ssid;
      status.style.color = "var(--success)";
    }
  }

  // load current SSID
  fetchJson("/api/wifi/ssid").then((data) => {
    if (!data) return;
    if (data.default) defaultSsid = data.default;
    if (data.ssid) {
      input.value = data.ssid;
      input.placeholder = data.ssid;
      renderStatus(data.ssid);
    }
  });

  // save SSID
  btnSave.addEventListener("click", async () => {
    const ssid = input.value.trim();
    if (ssid.length < 1) {
      showNotification("SSID cannot be empty", "error");
      return;
    }
    if (ssid.length > 32) {
      showNotification("SSID too long (max 32)", "error");
      return;
    }
    if (!/^[\x20-\x7E]+$/.test(ssid)) {
      showNotification("SSID must be printable ASCII", "error");
      return;
    }
    const resp = await fetchJson("/api/wifi/ssid", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ ssid: ssid }),
    });
    if (!resp) { showNotification("Failed to reach device", "error"); return; }
    if (!resp.ok) {
      showNotification(resp.error || "Failed to save SSID", "error");
      return;
    }
    status.textContent = "AP restarting as \"" + resp.ssid + "\"...";
    status.style.color = "var(--success)";
    showNotification("WiFi SSID saved - reconnect to AP");
  });

  // reset to factory SSID
  btnReset.addEventListener("click", async () => {
    const resp = await fetchJson("/api/wifi/ssid/reset", { method: "POST" });
    if (!resp || !resp.ok) { showNotification("Reset failed", "error"); return; }
    input.value = resp.ssid || defaultSsid;
    status.textContent = "AP restarting as \"" + (resp.ssid || defaultSsid) + "\"...";
    status.style.color = "var(--text-dim)";
    showNotification("WiFi SSID reset to default - reconnect to AP");
  });
}

// initialise WiFi password section
function initWifi() {
  const input   = document.getElementById("wifiPasswordInput");
  const toggle  = document.getElementById("wifiPasswordToggle");
  const status  = document.getElementById("wifiPasswordStatus");
  const btnSave = document.getElementById("wifiPasswordSave");
  if (!input || !toggle || !status || !btnSave) return;

  // show / hide password toggle
  toggle.addEventListener("click", () => {
    const isHidden = input.type === "password";
    input.type = isHidden ? "text" : "password";
    toggle.textContent = isHidden ? "\uD83D\uDE48" : "\uD83D\uDC41";
  });

  // load current status (just whether a password is set; never reveal the value)
  fetchJson("/api/wifi").then((data) => {
    if (!data) return;
    if (data.passwordSet) {
      status.textContent = "\u2713 Password set - AP is secured";
      status.style.color = "var(--success)";
    } else {
      status.textContent = "No password set yet - set one now";
      status.style.color = "var(--danger)";
    }
  });

  // save password
  btnSave.addEventListener("click", async () => {
    const pwd = input.value.trim();
    if (pwd.length < 8) { showNotification("Password must be at least 8 characters", "error"); return; }
    const resp = await fetchJson("/api/wifi", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ password: pwd }),
    });
    if (!resp) { showNotification("Failed to reach device", "error"); return; }
    if (!resp.ok) {
      showNotification(resp.error || "Failed to save password", "error");
      return;
    }
    input.value = "";
    status.textContent = "\u2713 Password set - AP restarting...";
    status.style.color = "var(--success)";
    showNotification("WiFi password saved - reconnect to AP");
  });
}

function showNotification(message, type = "success") {
  const notification = document.createElement("div");
  notification.textContent = message;
  notification.style.cssText = `
        position: fixed;
        top: 20px;
        left: 50%;
        transform: translateX(-50%);
        padding: 1rem 2rem;
        background: ${type === "error" ? "var(--danger)" : "var(--success)"};
        color: white;
        border-radius: 8px;
        z-index: 10000;
        font-weight: 600;
        box-shadow: 0 4px 12px rgba(0,0,0,0.3);
    `;

  document.body.appendChild(notification);

  setTimeout(() => {
    notification.style.transition = "opacity 0.3s";
    notification.style.opacity = "0";
    setTimeout(() => notification.remove(), 300);
  }, 3000);
}

// Software Update (Update tab): live version/safe-state info from /ota/info +
// /ota/check, then ONE upload slot. The module classifies what it was given
// (merged firmware+UI image, bare firmware.bin, or bare littlefs.bin) by itself.
// It reboots after a firmware/merged upload and this page waits for /ota/health
// and reloads. The same upload core serves the GitHub flow in initUpdateCheck().
function initOtaUpdate() {
  const version      = document.getElementById("otaVersion");
  const build        = document.getElementById("otaBuild");
  const chip         = document.getElementById("otaChip");
  const slot         = document.getElementById("otaPartition");
  const safeState    = document.getElementById("otaSafeState");
  const safeReason   = document.getElementById("otaSafeReason");
  const fileInput    = document.getElementById("otaFile");
  const uploadBtn    = document.getElementById("otaUpload");
  const progressWrap = document.getElementById("otaProgressWrap");
  const progressFill = document.getElementById("otaProgressFill");
  const progressLbl  = document.getElementById("otaProgressLabel");
  const result       = document.getElementById("otaResult");
  if (!fileInput || !uploadBtn || !progressWrap || !result) return;

  async function refreshInfo() {
    const info = await fetchJson("/ota/info");
    if (info) {
      const ui = info.fsVersion && info.fsVersion !== "--" && info.fsVersion !== info.version ? " (web UI " + info.fsVersion + ")" : "";
      version.textContent = (info.version || "--") + ui;
      build.textContent = info.appDate ? info.appDate + " " + (info.appTime || "") : "--";
      if (chip) chip.textContent = info.chipModel ? info.chipModel + (info.chipRevision ? " rev " + info.chipRevision : "") : "--";
      slot.textContent = info.partition || "--";
      const inst = document.getElementById("updInstalled");
      if (inst && info.version) inst.textContent = "v" + info.version;
      if (info.version) window._otaInstalledVersion = info.version;
    }
    const check = await fetchJson("/ota/check");
    if (check) {
      safeState.textContent = check.allowed ? "Ready" : "Blocked";
      safeState.style.color = check.allowed ? "var(--success)" : "var(--danger)";
      if (check.reason) safeReason.textContent = check.reason;
    }
  }

  refreshInfo();
  // The safe-state can change (car starts moving, CAN fault) - refresh whenever
  // the user lands on the Update tab rather than polling continuously.
  document.querySelectorAll('.nav-tab[data-page="ota"]').forEach((tab) => {
    tab.addEventListener("click", refreshInfo);
  });

  function setProgress(pct) {
    progressFill.style.width = pct + "%";
    progressLbl.textContent = pct + "%";
  }

  // fetch() has no upload progress, so uploads go through XMLHttpRequest.
  // Resolves {ok, status, text}; never rejects.
  function uploadBin(url, fieldName, blob, filename, onProgress) {
    return new Promise((resolve) => {
      const xhr = new XMLHttpRequest();
      xhr.open("POST", url);
      xhr.withCredentials = true;
      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable && onProgress) onProgress(e.loaded / e.total);
      };
      xhr.onload = () => resolve({ ok: xhr.status === 200, status: xhr.status, text: xhr.responseText || "" });
      xhr.onerror = () => resolve({ ok: false, status: 0, text: "Connection lost during upload" });
      const form = new FormData();
      form.append(fieldName, blob, filename);
      xhr.send(form);
    });
  }

  // Used by the guided GitHub update: the two halves go to their own endpoints
  // (/ota/update/fs for littlefs.bin, /ota/update for firmware.bin). `size` lets
  // the module spot a short upload that would leave half an image behind.
  window.otaUploadBlob = async function (type, blob, filename, opts) {
    opts = opts || {};
    const isFs = type === "filesystem";
    const url = (isFs ? "/ota/update/fs" : "/ota/update") + "?size=" + blob.size;
    const res = await uploadBin(url, isFs ? "filesystem" : "firmware", blob, filename, opts.onProgress);
    if (res.ok) return res.text;
    if (res.status === 403) throw new Error("Blocked: system not safe for update.");
    if (res.status === 401) throw new Error("Sign-in needed (user admin, password = the access-point WiFi password). Reload the page and sign in.");
    if (res.status === 400) throw new Error(res.text || "Image rejected by the module.");
    throw new Error(res.text || (res.status ? "Update failed (" + res.status + ")." : "Upload failed. Check the connection and retry."));
  };

  async function waitForReboot() {
    result.textContent = "Update sent - module is rebooting. Waiting for it to come back...";
    const start = Date.now();
    while (Date.now() - start < 120000) {
      await new Promise((r) => setTimeout(r, 2000));
      try {
        const resp = await fetch("/ota/health", { cache: "no-store" });
        if (resp.ok) {
          result.textContent = "Module is back - reloading...";
          location.reload();
          return;
        }
      } catch (e) {
        // still down - keep waiting
      }
    }
    result.textContent = "Module did not come back within 2 minutes. Check its power and WiFi, then reload this page.";
    uploadBtn.disabled = false;
  }

  // Mirror of the firmware's upload classifier, used only for the status label
  // and to reject an obviously wrong file before wasting an upload. The
  // firmware re-checks server-side and is authoritative. A littlefs image has
  // "littlefs" at byte 8; ESP firmware/bootloader images start with 0xE9, and a
  // merged full-flash image is far larger than one firmware slot.
  async function classifyFile(file) {
    try {
      const head = new Uint8Array(await file.slice(0, 16).arrayBuffer());
      if (head.length >= 16 &&
          String.fromCharCode.apply(null, Array.from(head.slice(8, 16))) === "littlefs") {
        return "web UI";
      }
      if (head[0] === 0xe9) {
        return file.size > 0x1a0000 ? "full package (firmware + web UI)" : "firmware";
      }
      return null;
    } catch (e) {
      return "update"; // can't sniff (old browser) - let the module decide
    }
  }

  async function runUpload() {
    const file = fileInput.files && fileInput.files[0];
    if (!file) {
      showNotification("Choose a .bin file first", "error");
      return;
    }
    const kindLabel = await classifyFile(file);
    if (!kindLabel) {
      showNotification("That file is not a firmware, web-UI, or merged update image", "error");
      return;
    }
    // Pre-flight the safe-state gate so a blocked update fails with a reason
    // instead of dying mid-upload.
    const check = await fetchJson("/ota/check");
    if (!check) {
      showNotification("Failed to reach device", "error");
      return;
    }
    if (!check.allowed) {
      showNotification(check.reason || "Module not in a safe state for updates", "error");
      refreshInfo();
      return;
    }

    uploadBtn.disabled = true;
    progressWrap.style.display = "";
    setProgress(0);
    result.textContent = "Uploading " + kindLabel + ": " + file.name + " (do not close this page or power the module off)";

    const res = await uploadBin("/ota/update?size=" + file.size, "update", file, file.name,
      (f) => setProgress(Math.round(f * 100)));
    if (res.ok) {
      setProgress(100);
      if (kindLabel === "web UI") {
        // A bare filesystem image does not reboot the module.
        result.textContent = "Web UI updated. Reloading...";
        setTimeout(() => location.reload(), 1500);
      } else {
        await waitForReboot();
      }
    } else {
      const msg = res.status === 401 ? "Sign-in needed. Reload the page and sign in (user admin)." : (res.text || "Upload failed");
      result.textContent = msg;
      showNotification(msg, "error");
      uploadBtn.disabled = false;
      refreshInfo();
    }
  }

  uploadBtn.addEventListener("click", runUpload);
}


// Status fields that only exist in the v9 firmware: wheel slip, live-diag
// TP2.0 block, steering health, Bluetooth link, Bench Mode lock. Kept in one
// place so the main poll stays readable. Every field is optional.
function updateV9Status(data, chassisOk, haldexOk) {
  const text = (id, v) => { const e = document.getElementById(id); if (e) e.textContent = v; };
  const num = (v, d) => (typeof v === "number" ? v.toFixed(d) : "--");

  // Per-corner slip [FL, FR, RL, RR]; null (no data / stale) shows as "--".
  // The card stays hidden until real numbers have been seen once.
  const slip = Array.isArray(data.cornerSlip) ? data.cornerSlip : Array.isArray(data.slip) ? data.slip : null;
  const slipCard = document.getElementById("wheelSlipCard");
  if (slip && slip.some((v) => v !== null && v !== undefined)) updateV9Status._slipSeen = true;
  if (slipCard) slipCard.style.display = updateV9Status._slipSeen ? "" : "none";
  ["slipFL", "slipFR", "slipRL", "slipRR"].forEach((id, i) => {
    const v = slip ? slip[i] : null;
    text(id, v === null || v === undefined ? "--" : v);
  });

  // Gen2/Gen4 KWP2000-over-TP2.0 live data (replaces UDS on those generations).
  const kwp = data.kwp;
  const kwpDetails = document.getElementById("kwpDetails");
  if (kwpDetails) kwpDetails.style.display = kwp ? "" : "none";
  if (kwp) {
    text("kwpOilTemp", num(kwp.oilTemp, 1));
    text("kwpPlateTemp", num(kwp.plateTemp, 1));
    text("kwpSupplyVoltage", num(kwp.supplyVoltage, 2));
    text("kwpOilPressure", num(kwp.oilPressure, 0));
    text("kwpEstTorque", num(kwp.estTorque, 0));
    text("kwpClutchDuty", num(kwp.clutchDuty, 0));
    text("kwpClutchValveCurrent", num(kwp.clutchValveCurrent, 3));
    text("kwpStatus", data.diagToolActive ? "Paused: an external diagnostic tool was detected."
      : (kwp.connected ? "Connected" : "Connecting..."));
  }

  // Steering signal health (diag tab)
  const sh = document.getElementById("diagSteeringHealth");
  if (sh) sh.textContent = data.steeringHealthy === undefined || data.steeringHealthy === null ? "--"
    : (data.steeringHealthy ? "\u2713 Healthy" : "X Unhealthy");
  text("diagSteeringAngle", data.steeringAngle ?? "--");

  // Bluetooth link
  if (data.bleCodeRequired !== undefined) renderBlePairing(data.bleCodeRequired);
  const bleStatus = document.getElementById("bleStatus");
  if (bleStatus && data.bleConnected !== undefined) {
    bleStatus.textContent = data.bleConnected ? "\u2713 Phone connected" : "No phone connected";
    bleStatus.style.color = data.bleConnected ? "var(--success)" : "var(--text-dim)";
  }

  // Bench Mode can only be switched while genuinely off the car (both buses
  // silent). The firmware also clears it when CAN appears; this stops it being
  // flipped on by mistake while harnessed to a live car.
  const benchModeElem = document.getElementById("benchMode");
  const benchModeStatus = document.getElementById("benchModeStatus");
  const canDetected = !!(chassisOk || haldexOk);
  if (benchModeElem) benchModeElem.disabled = canDetected;
  if (benchModeStatus) {
    benchModeStatus.textContent = canDetected
      ? "Locked: CAN traffic seen, so this unit is harnessed (sleep behaves normally)"
      : "Available: no CAN on either bus";
    benchModeStatus.style.color = canDetected ? "var(--text-dim)" : "var(--success)";
  }
}

// ---- Frame-edit gating (Calibrate > Frame blocks) ----------------------
// Render the per-generation editable-frame checkboxes from /api/settings data.
function renderFrameBlocks(blocks) {
  const list = document.getElementById("frameEditList");
  if (!list) return;
  if (!Array.isArray(blocks) || blocks.length === 0) {
    list.innerHTML = '<p class="hint">Not available for this generation.</p>';
    return;
  }
  list.innerHTML = "";
  blocks.forEach((b) => {
    const row = document.createElement("label");
    row.className = "toggle";
    const cb = document.createElement("input");
    cb.type = "checkbox";
    cb.checked = !!b.enabled;
    cb.dataset.bit = b.bit;
    cb.addEventListener("change", () => saveFrameEdit(b.bit, cb.checked));
    const slider = document.createElement("span");
    slider.className = "toggle-slider";
    const span = document.createElement("span");
    span.className = "toggle-label";
    span.textContent = b.name;
    row.appendChild(cb);
    row.appendChild(slider);
    row.appendChild(span);
    list.appendChild(row);
  });
}

// Toggle a single frame-edit block for the current generation.
async function saveFrameEdit(bit, on) {
  try {
    const response = await fetchJson("/api/settings", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ frameEditBit: bit, frameEditOn: on }),
    });
    if (!response.ok) showNotification("Failed to save frame setting", "error");
  } catch (error) {
    showNotification("Error saving frame setting", "error");
  }
}

// "Reset to Defaults" under Frame blocks: puts every block mask back to the
// firmware defaults, then redraws the list from the device.
function initFrameEditReset() {
  const btn = document.getElementById("frameEditReset");
  if (!btn) return;
  btn.addEventListener("click", async () => {
    if (!confirm("Turn every frame block back to its default setting?")) return;
    try {
      const resp = await fetchJson("/api/settings", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ frameEditReset: true }),
      });
      if (!resp) {
        showNotification("Failed to reset frame blocks", "error");
        return;
      }
      showNotification("Frame blocks reset to defaults");
    } catch (error) {
      showNotification("Error resetting frame blocks", "error");
    }
    refreshFrameBlocks();
  });
}

// Re-fetch settings and re-render the frame checkboxes (e.g. after a generation
// change or a reset-to-defaults).
async function refreshFrameBlocks() {
  try {
    const data = await fetchJson("/api/settings");
    renderFrameBlocks(data.frameBlocks);
  } catch (error) {
    /* leave existing list in place on error */
  }
}


// ---- Sleep auto-setup (offered once, Gen5) ---------------------------------
let _sleepCalState = 0; // 0 never asked, 1 armed, 2 done, 3 declined
let _sleepCalAvgFps = 0;

function renderSleepCal() {
  const sw = document.getElementById("sleepCalArm");
  if (sw) sw.checked = _sleepCalState === 1;
  const el = document.getElementById("sleepCalStatus");
  if (!el) return;
  if (_sleepCalState === 1) el.textContent = "Armed: next time the car is left parked with the ignition off, the quiet bus is measured for 15 min.";
  if (_sleepCalState === 2) el.textContent = `Done: set from the parked bus (average ${_sleepCalAvgFps} fps).`;
  el.hidden = _sleepCalState !== 1 && _sleepCalState !== 2;
}

// On page load: only if never asked. On a generation change to Gen5: always.
async function maybeOfferSleepSetup(gen, changed = false) {
  if (![50, 51, 52].includes(gen) || (!changed && _sleepCalState !== 0)) return;
  const yes = confirm("Enable CAN sleep and set it up automatically?\n\n" +
    "Next time the car is left, the controller measures the parked bus for 15 min and sets the wake threshold. Runs once.");
  if (await saveSetting("sleepCalArm", yes)) {
    _sleepCalState = yes ? 1 : 3;
    renderSleepCal();
  }
}

// ---- Long Learn (Calibrate > Long Learn) -----------------------------------
// Drives /api/longlearn/*: polls status while a run is active, renders the
// tracker + per-block verdicts, keeps the chassis notes on the unit, and
// exports a plain-text report of the car, calibration, block set and sweeps.
const LL_PHASE_NAMES = ["Idle", "Reference (all blocks on)", "BPK Adjust (hunting)",
                        "Testing blocks (one off, point reads)", "Confirmation sweep", "Complete", "Cancelled", "Failed"];
const LL_RESULT = { 0: ["untested", ""], 1: ["core", "core"], 2: ["needed", "needed"],
                    3: ["no effect", "removed"], 4: ["affects (higher without)", "harmful"] };
const LL_SWEEP_KIND = ["reference", "reference points", "block", "final", "bpk"];
const LL_FAIL = ["", "no Haldex data with all blocks on",
                 "all blocks on is not a smooth 100% - fix that first, nothing to compare blocks against",
                 "the two all-on point reads disagree too much (noisy) to judge blocks"];
// Block-off read for a bit -> " (dev N%)" suffix, or "" if not tested yet.
function llDev(data, bit) {
  const sw = (data.sweeps || []).find((x) => x.kind === 2 && x.bit === bit);
  return sw ? ` (dev ${sw.dev}%)` : "";
}

function llScoreText(sc) {
  if (!sc) return "--";
  const eng = sc.engageCF > 100 ? "never engaged" : `engage @CF${sc.engageCF} \u2192 ${sc.engageJump}%`;
  return `${sc.smooth ? "smooth" : "NOT smooth"} \u00b7 reach ${sc.reach}% \u00b7 max step ${sc.maxStep}% \u00b7 ${eng} \u00b7 score ${sc.score}`;
}

function llFmtElapsed(sec) {
  sec = Math.max(0, Math.floor(sec || 0));
  const m = Math.floor(sec / 60), s = sec % 60;
  return `${m}m ${String(s).padStart(2, "0")}s`;
}

function initLongLearn() {
  const statusText   = document.getElementById("longLearnStatusText");
  const progressWrap = document.getElementById("longLearnProgressWrap");
  const progressFill = document.getElementById("longLearnProgressFill");
  const progressLbl  = document.getElementById("longLearnProgressLabel");
  const tracker      = document.getElementById("longLearnTracker");
  const results      = document.getElementById("longLearnResults");
  const btnStart     = document.getElementById("longLearnStart");
  const btnCancel    = document.getElementById("longLearnCancel");
  const btnExport    = document.getElementById("longLearnExport");
  const testAll      = document.getElementById("longLearnTestAll");
  const notes        = document.getElementById("longLearnNotes");
  if (!statusText || !btnStart) return;

  let pollTimer = null;
  let lastStatus = null; // last /api/longlearn/status payload (used by export)

  function setText(id, v) { const e = document.getElementById(id); if (e) e.textContent = v; }

  function renderBlocks(data) {
    if (!results) return;
    const blocks = Array.isArray(data.blocks) ? data.blocks : [];
    if (blocks.length === 0) { results.innerHTML = ""; return; }
    results.innerHTML = "";
    blocks.forEach((b) => {
      const row = document.createElement("div");
      row.className = "ll-block";
      const name = document.createElement("span");
      name.className = "ll-block-name" + (b.enabled ? "" : " off");
      name.textContent = b.name;
      const tag = document.createElement("span");
      let cls = "", txt = "";
      if (data.active && data.currentBit === b.bit) { cls = "testing"; txt = "testing..."; }
      else {
        const r = LL_RESULT[b.result] || LL_RESULT[0];
        txt = r[0] + llDev(data, b.bit); cls = r[1];
        if (b.result === 0) txt = b.core ? "core" : (data.phase === 0 ? (b.enabled ? "on" : "off") : "queued");
      }
      tag.className = "ll-tag " + cls;
      tag.textContent = txt;
      row.appendChild(name);
      row.appendChild(tag);
      results.appendChild(row);
    });
  }

  function render(data) {
    lastStatus = data;
    const running = !!data.active;
    btnStart.style.display  = running ? "none" : "";
    btnCancel.style.display = running ? "" : "none";
    if (testAll) testAll.disabled = running;

    const showTracker = running || data.phase >= 5;
    progressWrap.style.display = running ? "" : "none";
    tracker.style.display = showTracker ? "" : "none";

    const total = data.sweepTotal || 0, idx = data.sweepIdx || 0;
    const pct = total ? Math.min(100, Math.round((idx / total) * 100)) : 0;
    progressFill.style.width = pct + "%";
    progressLbl.textContent = `${idx}/${total} done`;

    setText("llPhase", LL_PHASE_NAMES[data.phase] || "--");
    setText("llSweep", total ? `${Math.min(idx + (running ? 1 : 0), total)} of ${total}` : "--");
    let testing = "--";
    if (running && data.currentBit >= 0 && Array.isArray(data.blocks)) {
      const b = data.blocks.find((x) => x.bit === data.currentBit);
      testing = b ? `without ${b.name}` : `bit ${data.currentBit}`;
    } else if (running && data.phase === 1) {
      testing = "all blocks on";
    } else if (running && data.phase === 2) {
      testing = `BPK packing / torque ceiling (${data.bpkNow} Nm)`;
    } else if (running && data.phase === 4) {
      testing = "final block set";
    }
    setText("llTesting", testing);
    const isGen5 = data.generation === 50 || data.generation === 52;
    setText("llFloor", isGen5 ? `${data.floorNow}%` + (data.phase >= 2 ? ` (was ${data.floorStart}%)` : "") : "n/a (Gen5 only)");
    setText("llBpk", isGen5 ? `${data.bpkNow} Nm, Fix Hunting ${data.fixHunting ? "on" : "off"}` +
      (data.bpkAdjusted ? ` (was ${data.bpkStart} Nm)` : "") : "n/a (Gen5 only)");
    setText("llBaseline", llScoreText(data.baseline) +
      (data.phase >= 3 && data.phase !== 7 ? ` | noise ${data.noise}%, threshold ${data.tol}%` : ""));
    setText("llElapsed", llFmtElapsed(data.elapsedS));

    const cf = data.cf ?? 0, eng = data.eng ?? 0;
    const cfFill = document.getElementById("llCFFill"), engFill = document.getElementById("llEngFill");
    if (cfFill) cfFill.style.width = cf + "%";
    if (engFill) engFill.style.width = eng + "%";
    setText("llCFValue", running ? cf + "%" : "--");
    setText("llEngValue", running ? eng + "%" : "--");

    if (running) {
      statusText.textContent = `Long Learn running - ${LL_PHASE_NAMES[data.phase] || ""}`;
      statusText.style.color = "var(--text-dim)";
    } else if (data.phase === 5) {
      const f = data.final;
      const kept = (data.blocks || []).filter((b) => b.enabled).length;
      statusText.textContent = `Long Learn complete \u2713 - ${kept} of ${(data.blocks || []).length} blocks enabled, ` +
        (isGen5 ? `PWM floor ${data.floorResult}%, ` : "") +
        (isGen5 && data.bpkAdjusted ? `BPK kept: Fix Hunting ${data.fixHunting ? "on" : "off"}, ceiling ${data.bpkNow} Nm, ` : "") +
        (data.interaction ? "the no-effect blocks changed the curve when off together, so all were left on, " : "") +
        `final: ${llScoreText(f)}`;
      statusText.style.color = f && f.smooth && !data.interaction ? "var(--success)" : "var(--warning)";
    } else if (data.phase === 6) {
      statusText.textContent = "Long Learn cancelled - previous blocks, floor, torque ceiling and learn table put back";
      statusText.style.color = "var(--warning)";
    } else if (data.phase === 7) {
      statusText.textContent = `Long Learn stopped - ${LL_FAIL[data.failReason] || "sweep failed"}. Previous settings restored`;
      statusText.style.color = "var(--danger)";
    } else {
      statusText.textContent = "Not run yet";
      statusText.style.color = "var(--text-dim)";
    }
    renderBlocks(data);
  }

  async function poll() {
    const data = await fetchJson("/api/longlearn/status");
    if (!data) return;
    const wasRunning = pollTimer !== null;
    render(data);
    if (!data.active && wasRunning) {
      clearInterval(pollTimer);
      pollTimer = null;
      // The run changed the mask / floor / learn table - refresh dependents.
      refreshFrameBlocks();
      const esp14Range = document.getElementById("esp14MinFloorRange");
      const esp14Val = document.getElementById("esp14MinFloorValue");
      if (esp14Range) { esp14Range.value = data.floorNow; if (esp14Val) esp14Val.textContent = data.floorNow; }
      const bpkRange = document.getElementById("bpkCeilingRange");
      const bpkVal = document.getElementById("bpkCeilingValue");
      if (bpkRange && data.bpkNow) { bpkRange.value = data.bpkNow; if (bpkVal) bpkVal.textContent = data.bpkNow; }
      fetchJson("/api/learn/status").then((ls) => { if (ls && ls.tableValid) renderLearnChart(ls.table); });
    }
  }

  function startPolling() {
    if (pollTimer) return;
    pollTimer = setInterval(poll, 1000);
    poll();
  }

  btnStart.addEventListener("click", async () => {
    statusText.textContent = "Starting Long Learn...";
    statusText.style.color = "var(--text-dim)";
    const resp = await fetchJson("/api/longlearn/start", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ testAll: !!(testAll && testAll.checked) }),
    });
    if (!resp || !resp.ok) {
      statusText.textContent = resp && resp.error ? resp.error : "Failed to start Long Learn";
      statusText.style.color = "var(--danger)";
      return;
    }
    startPolling();
  });

  btnCancel.addEventListener("click", async () => {
    await fetchJson("/api/longlearn/cancel", { method: "POST" });
    statusText.textContent = "Cancelling...";
    statusText.style.color = "var(--warning)";
  });

  if (notes) {
    notes.addEventListener("change", () => saveSetting("longLearnNotes", notes.value.slice(0, 200)));
  }

  if (btnExport) {
    btnExport.addEventListener("click", async () => {
      const [settings, ll, learn] = await Promise.all([
        fetchJson("/api/settings"), fetchJson("/api/longlearn/status"), fetchJson("/api/learn/status"),
      ]);
      if (!settings || !ll) { showNotification("Could not read data for export", "error"); return; }
      const txt = buildLongLearnReport(settings, ll, learn, notes ? notes.value : "");
      const gen = settings.haldexGeneration;
      const stamp = new Date().toISOString().slice(0, 19).replace(/[:T]/g, "-");
      const blob = new Blob([txt], { type: "text/plain" });
      const a = document.createElement("a");
      a.href = URL.createObjectURL(blob);
      a.download = `openhaldex-longlearn-gen${gen}-${stamp}.txt`;
      document.body.appendChild(a);
      a.click();
      setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
    });
  }

  // initial state (and resume polling if a run is already in progress)
  fetchJson("/api/longlearn/status").then((data) => {
    if (!data) return;
    render(data);
    if (data.active) startPolling();
  });
}

// Plain-text report: car notes, calibration, recommended block set (with a
// ready-to-edit checklist + mask), the sweep log and the stored learn table.
function buildLongLearnReport(settings, ll, learn, notesText) {
  const genSel = document.getElementById("haldexGeneration");
  const genName = genSel && genSel.selectedOptions[0] ? genSel.selectedOptions[0].textContent : String(settings.haldexGeneration);
  const isGen5 = settings.haldexGeneration === 50 || settings.haldexGeneration === 52;
  const pad = (v, n) => String(v).padEnd(n);
  const L = [];
  L.push("OpenHaldex Edge Long Learn Report");
  L.push("=".repeat(60));
  L.push(`Firmware:   ${settings.FW_VERSION || "--"}`);
  L.push(`Board:      rev ${settings.boardRev || "?"}`);
  L.push(`Exported:   ${new Date().toISOString()}`);
  L.push(`Generation: ${settings.haldexGeneration} (${genName})`);
  L.push(`Mode:       ${settings.isStandalone ? "Standalone" : "Normal (passthrough)"}`);
  L.push("");
  L.push("Chassis / car notes:");
  L.push("  " + ((notesText || "").trim() || "(none entered)").replace(/\n/g, "\n  "));
  L.push("");
  L.push("Calibration:");
  L.push(`  Lock Calibration (BPK ceiling): ${settings.bpkCeilingNm} Nm`);
  L.push(`  Launch PWM Floor: ${settings.esp14MinFloorPct} %` +
         (ll.phase === 5 && isGen5 ? `  (Long Learn: ${ll.floorStart} % -> ${ll.floorResult} %)` : ""));
  if (ll.phase === 5 && isGen5 && ll.bpkAdjusted) {
    L.push(`  BPK set by Long Learn (kept): Fix Hunting ${ll.fixHunting ? "on" : "off"}, ceiling ${ll.bpkStart} Nm -> ${ll.bpkNow} Nm`);
  }
  L.push("");
  L.push(`Long Learn: ${LL_PHASE_NAMES[ll.phase] || "--"}` +
         (ll.phase >= 5 ? `  (${ll.sweepIdx} sweeps, ${llFmtElapsed(ll.elapsedS)}, test-all ${ll.testAll ? "on" : "off"})` : ""));
  if (ll.phase === 7) L.push(`  Stopped: ${LL_FAIL[ll.failReason] || "sweep failed"}`);
  if (ll.baseline) L.push(`  Reference (all on): ${llScoreText(ll.baseline)}`);
  if (ll.final)    L.push(`  Final (kept set):   ${llScoreText(ll.final)}`);
  if (ll.phase === 5) L.push(`  Reference noise ${ll.noise}%, block threshold ${ll.tol}% (worst of the points CF ${(ll.ptCF || []).join("/")})`);
  if (ll.interaction) L.push("  INTERACTION: the no-effect blocks changed the curve when off together - all left on");
  L.push("");
  L.push(`Frame blocks (mask ${ll.mask || "--"}) - [x] = enabled. Edit and re-apply under Calibrate > Frame blocks:`);
  (ll.blocks || []).forEach((b) => {
    const r = LL_RESULT[b.result] || LL_RESULT[0];
    const verdict = b.result === 0 ? (ll.phase === 0 ? "" : "untested") : r[0];
    L.push(`  [${b.enabled ? "x" : " "}] ${pad(b.name, 22)} bit ${pad(b.bit, 3)} ${pad(b.core ? "core" : "static", 8)} ${verdict}${llDev(ll, b.bit)}`);
  });
  L.push("");
  if (Array.isArray(ll.sweeps) && ll.sweeps.length) {
    L.push("Sweep log:");
    ll.sweeps.forEach((sw, i) => {
      let what = LL_SWEEP_KIND[sw.kind] || "?";
      if (sw.kind === 2) {
        const b = (ll.blocks || []).find((x) => x.bit === sw.bit);
        what = `without ${b ? b.name : "bit " + sw.bit}`;
      } else if (sw.kind === 4) {
        what = `bpk ${sw.bpk} Nm`;
      }
      const pts = Array.isArray(sw.pts) ? sw.pts : null; // point reads, not a curve
      const verdict = sw.kind === 2 ? ((LL_RESULT[sw.verdict] || ["?"])[0]) : sw.kind === 1 ? "" : (sw.verdict ? "smooth" : "NOT smooth");
      const dev = sw.kind === 2 || sw.kind === 3 ? ` dev ${pad(sw.dev, 3)} mean ${pad((sw.dMean > 0 ? "+" : "") + sw.dMean, 4)}` : "";
      L.push(`  #${pad(i + 1, 3)} ${pad(what, 28)} ` + (pts ? pts.map((v, k) => `@${(ll.ptCF || [])[k]} ${pad(v, 4)}`).join("") :
             `reach ${pad(sw.reach, 4)} step ${pad(sw.maxStep, 3)} engage@${pad(sw.engageCF > 100 ? "--" : sw.engageCF, 3)}->${pad(sw.engageJump, 3)} score ${pad(sw.score, 3)}`) +
             `${dev}${verdict ? " => " + verdict : ""}`);
    });
    L.push("");
  }
  if (learn && learn.tableValid && Array.isArray(learn.table)) {
    L.push("Stored learn table (CF% -> engagement%):");
    for (let i = 0; i < learn.table.length; i += 10) {
      L.push("  " + learn.table.slice(i, i + 10).map((v, j) => `${pad(i + j, 3)}:${pad(v, 3)}`).join(" "));
    }
  } else {
    L.push("Stored learn table: none (static factor active)");
  }
  L.push("");
  return L.join("\n");
}


// Pairing line on the Bluetooth card: the code is only asked from the second phone on.
let blePasskeyCache = null;
function renderBlePairing(codeRequired) {
  const el = document.getElementById("blePairing");
  if (!el) return;
  el.textContent = codeRequired
    ? `Pairing code for another phone: ${blePasskeyCache ?? "--"}`
    : "No phone paired yet - the first phone pairs without a code.";
}

// initialise Bluetooth (DashCAN app) section
function initBle() {
  const btnForget = document.getElementById("bleForget");

  btnForget.addEventListener("click", async () => {
    const resp = await fetchJson("/api/ble/forget", { method: "POST" });
    if (!resp) {
      showNotification("No response from the controller", "error");
      return;
    }
    if (!resp.ok) {
      showNotification("Bluetooth is off - nothing to forget", "error");
      return;
    }
    showNotification("Paired phones forgotten, new pairing code - the next phone pairs without it");
    initStoredSettings(); // refresh the pairing line
  });
}



// ---------------------------------------------------------------------------
// Check for Updates (guided OTA)
// The page - not the controller - fetches the release list and the .bin files
// from GitHub, then pushes each image through the same safety-gated /ota
// endpoints as a manual upload. The device validates each image itself
// (esp_ota_end for firmware, a mount for the filesystem) - no hash gate.
//
// Where the internet comes from: the intended route is bridge mode - the
// controller joins the home router (Home WiFi card, duplicated on this tab)
// and the phone sits on that same network, so it keeps its normal internet
// and can still reach the controller by LAN address. On the bare OpenHaldex
// AP most phones drop mobile data (the AP hands out no gateway, and Android
// only keeps cellular for internet until the user "accepts" the no-internet
// network), so that route is best-effort. Either way, every check starts by
// pinging the controller: there's no point fetching from GitHub if the
// device has gone to sleep or the phone has wandered off its network.
//
// Two sources are merged:
//   1. Releases/releases.json - the index tools/make_release.py writes: notes,
//      date, channel, ota flag per release.
//   2. The Releases/ folder listing from the GitHub API - so a V<x.y.z> folder
//      that has just been dropped in shows up even before the index is
//      regenerated (no notes).
// Two mirrors are tried for the index and the images. raw.githubusercontent
// is the source of truth; jsDelivr serves the same repo and gets through on
// networks that block or mangle raw.githubusercontent. Whichever answers
// first is used for the .bin downloads too.
// ---------------------------------------------------------------------------
const UPD_REPO = "Kile-Thomson/OpenHaldex-Edge";
const UPD_BRANCH = "ota";
const UPD_MIRRORS = [
  { name: "GitHub", base: "https://raw.githubusercontent.com/" + UPD_REPO + "/" + UPD_BRANCH + "/Releases/" },
  { name: "jsDelivr", base: "https://cdn.jsdelivr.net/gh/" + UPD_REPO + "@" + UPD_BRANCH + "/Releases/" },
];
const UPD_DIR_API = "https://api.github.com/repos/" + UPD_REPO + "/contents/Releases?ref=" + UPD_BRANCH;
// Update channel. Everything is published on one branch (ota) so the channel
// changes which builds are *offered*, not where they are fetched from.
//   stable - only a higher version number than the one installed (as before)
//   latest - also the newest release itself, because its .bin files get
//            rebuilt in place when a fix lands before the next version is cut
const UPD_CHANNEL_KEY = "otaUpdateChannel";
let updChannel = "stable";
// Folder names are V<x.y.z> plus an optional pre-release suffix (V9.01.0-beta1).
const UPD_FOLDER_RE = /^V(\d+(?:\.\d+)*(?:-[0-9A-Za-z.]+)?)$/i;
// Firmware and the web UI report only the core number (9.01.0) even when the
// release is tagged 9.01.0-beta1, so post-install checks compare the core.
function updCoreVersion(v) {
  return String(v || "").split("-")[0];
}
// Mirror that last answered - the .bin downloads follow the index.
let UPD_RELEASES_BASE = UPD_MIRRORS[0].base;

// numeric compare of "x.yy.z" strings: >0 if a newer than b
function updCompareVersions(a, b) {
  const pa = String(a || "").split(".").map((n) => parseInt(n, 10) || 0);
  const pb = String(b || "").split(".").map((n) => parseInt(n, 10) || 0);
  for (let i = 0; i < Math.max(pa.length, pb.length); i++) {
    const d = (pa[i] || 0) - (pb[i] || 0);
    if (d) return d;
  }
  return 0;
}

// fetch() with a timeout - a phone that has silently lost its route can
// otherwise hang for a minute before the browser gives up.
function updFetch(url, ms, opts) {
  const ctrl = typeof AbortController === "function" ? new AbortController() : null;
  const t = ctrl ? setTimeout(() => ctrl.abort(), ms) : null;
  const o = Object.assign({ cache: "no-store" }, opts || {});
  if (ctrl) o.signal = ctrl.signal;
  return fetch(url, o).finally(() => { if (t) clearTimeout(t); });
}

function initUpdateCheck() {
  const $ = (id) => document.getElementById(id);
  const checkBtn = $("updCheckBtn"), installBtn = $("updInstallBtn"), showAll = $("updShowAll"), bridgeBtn = $("updBridgeBtn");
  const picker = $("updPicker"), sel = $("updVersion"), notes = $("updNotes"), status = $("updStatus");
  const wrap = $("updProgressWrap"), bar = $("updProgressBar"), label = $("updProgressLabel");
  if (!checkBtn || !sel) return;

  let index = null;   // merged release list: { latest, releases[] }
  let busy = false;

  const setStatus = (msg, cls) => { status.textContent = msg; status.className = "status-line" + (cls ? " " + cls : ""); };
  const setState = (msg, cls) => { const e = $("updState"); if (e) { e.textContent = msg; e.className = cls || ""; } };
  const setPct = (f, text) => {
    const p = Math.max(0, Math.min(100, Math.round(f * 100)));
    if (bar) bar.style.width = p + "%";
    if (label) label.textContent = text ? text + " " + p + "%" : p + "%";
  };
  const setStep = (id, state) => {
    document.querySelectorAll("#updSteps .ota-step").forEach((el) => {
      if (el.dataset.step === id) { el.classList.toggle("active", state === "active"); el.classList.toggle("done", state === "done"); }
    });
  };
  const resetSteps = () => document.querySelectorAll("#updSteps .ota-step").forEach((el) => el.classList.remove("active", "done"));
  const installed = () => window._otaInstalledVersion || "";
  // After a failed check the button reads "Retry" and, when the fix is the
  // bridge, a second button jumps to the Home WiFi card on this tab.
  const showRetry = (needBridge) => {
    checkBtn.textContent = "Retry";
    if (bridgeBtn) bridgeBtn.hidden = !needBridge;
  };
  const goBridge = (ev) => {
    if (ev) ev.preventDefault();
    const card = $("otaWifiStaCard");
    if (card) { card.scrollIntoView({ behavior: "smooth", block: "start" }); const i = $("otaWifiStaSsidInput"); if (i) setTimeout(() => i.focus(), 400); }
  };

  function otaCandidates() {
    if (!index || !Array.isArray(index.releases)) return [];
    return index.releases.filter((r) => r && r.ota && r.firmware && r.filesystem);
  }

  function renderPicker() {
    const cur = installed();
    const all = showAll && showAll.checked;
    const list = otaCandidates().filter((r) => all || (r.channel !== "beta" && updCompareVersions(r.version, cur) >= 0));
    // Nothing extra to add on "latest": the >= 0 compare above already keeps
    // the installed version in the list, which is what makes a rebuild of it
    // installable. The channel only changes the wording and the rebuild check.
    list.sort((a, b) => updCompareVersions(b.version, a.version));
    sel.innerHTML = "";
    list.forEach((r) => {
      const o = document.createElement("option");
      o.value = r.version;
      const tags = [];
      if (r.version === index.latest) tags.push("latest");
      if (r.channel === "beta") tags.push("beta");
      if (r.unindexed) tags.push("not in index");
      const c = updCompareVersions(r.version, cur);
      if (c === 0) tags.push("installed");
      else if (c < 0) tags.push("rollback");
      o.textContent = "v" + r.version + (tags.length ? " (" + tags.join(", ") + ")" : "");
      sel.appendChild(o);
    });
    picker.hidden = list.length === 0;
    if (!list.length) {
      // Nothing to offer. Usually that just means every published release is
      // older than what is installed, which is a rollback, not an error.
      if (!all && otaCandidates().length) {
        setStatus("Nothing newer than the installed version. Tick \"Show beta / older versions\" to roll back.");
      } else {
        setStatus("No installable releases listed. Use \"Update from a File\" below.", "error");
      }
      return;
    }
    // default to latest, else the first entry
    sel.value = list.some((r) => r.version === index.latest) ? index.latest : list[0].version;
    renderNotes();
  }

  function selected() { return otaCandidates().find((r) => r.version === sel.value) || null; }

  function renderNotes() {
    const r = selected();
    notes.textContent = r ? ((r.date ? r.date + " - " : "") + (r.notes || "")) : "";
    if (installBtn) installBtn.textContent = r && updCompareVersions(r.version, installed()) < 0 ? "Roll back to v" + r.version : "Install v" + (r ? r.version : "");
  }

  // Fold the folder listing into the index. The folders are what actually
  // exists on GitHub, so a folder the index doesn't know is offered anyway
  // (without a checksum); an index entry with no folder is dropped since its
  // downloads would 404. `latest` is the newest stable, OTA-capable entry.
  function mergeSources(idx, dirs) {
    const byVer = {};
    if (idx && Array.isArray(idx.releases)) idx.releases.forEach((r) => { if (r && r.version) byVer[r.version] = r; });
    if (dirs) {
      dirs.forEach((v) => {
        if (byVer[v]) return;
        byVer[v] = {
          version: v, channel: v.indexOf("-") >= 0 ? "beta" : "stable", ota: true, unindexed: true,
          notes: "Not in the release index yet - no release notes.",
          firmware: { path: "V" + v + "/firmware.bin" },
          filesystem: { path: "V" + v + "/littlefs.bin" },
        };
      });
      Object.keys(byVer).forEach((v) => { if (dirs.indexOf(v) < 0) delete byVer[v]; });
    }
    const releases = Object.keys(byVer).map((v) => byVer[v]).sort((a, b) => updCompareVersions(b.version, a.version));
    const stable = releases.filter((r) => r.ota && r.firmware && r.filesystem && r.channel !== "beta");
    return { releases: releases, latest: stable.length ? stable[0].version : (releases.length ? releases[0].version : "") };
  }

  // Release index from the first mirror that answers. Resolves {index} or
  // {reached} (HTTP-level failure: internet fine, index missing/broken) or
  // {netErr} (transport failure: no route to the internet at all).
  async function fetchIndex() {
    let reached = "", netErr = "";
    for (const m of UPD_MIRRORS) {
      let res = null;
      try {
        res = await updFetch(m.base + "releases.json", 12000, { mode: "cors" });
      } catch (e) {
        netErr = m.name + ": " + (e && e.name === "AbortError" ? "timed out" : (e && e.message ? e.message : "unreachable"));
        continue;
      }
      if (!res.ok) { reached = m.name + " answered HTTP " + res.status; continue; }
      try {
        const idx = await res.json();
        UPD_RELEASES_BASE = m.base;
        return { index: idx };
      } catch (e) {
        reached = m.name + " answered HTTP 200 but the release index is not valid JSON";
      }
    }
    return reached ? { reached: reached } : { netErr: netErr };
  }

  // V<x.y.z> folder names under Releases/ from the GitHub contents API.
  // Unauthenticated calls are rate-limited (60/h per address), so a failure
  // here is not fatal - the index alone still works.
  async function fetchFolders() {
    try {
      const res = await updFetch(UPD_DIR_API, 12000, { mode: "cors", headers: { Accept: "application/vnd.github+json" } });
      if (!res.ok) return { reached: "GitHub API answered HTTP " + res.status };
      const arr = await res.json();
      if (!Array.isArray(arr)) return { reached: "GitHub API returned an unexpected listing" };
      const dirs = [];
      arr.forEach((e) => {
        const m = e && e.type === "dir" && UPD_FOLDER_RE.exec(e.name || "");
        if (m) dirs.push(m[1]);
      });
      return { dirs: dirs };
    } catch (e) {
      return { netErr: "GitHub API: " + (e && e.name === "AbortError" ? "timed out" : (e && e.message ? e.message : "unreachable")) };
    }
  }

  // Tailored "get online" advice, from what the controller says about its
  // own bridge link. Resolves to [message, needsBridgeSetup].
  async function offlineAdvice(detail) {
    let sta = null;
    try { sta = await fetchJson("/api/wifi/sta"); } catch (e) { /* advice below still stands */ }
    const why = detail ? " (" + detail + ")" : "";
    if (sta && sta.ssid && sta.connected) {
      return ["This browser has no internet" + why + ". The controller is already on \"" + sta.ssid + "\" at http://" + sta.ip +
        "/ - join this phone to \"" + sta.ssid + "\", open http://" + sta.ip + "/ (or http://openhaldex.local/), come back to this tab and press Retry.", false];
    }
    if (sta && sta.ssid) {
      return ["This browser has no internet" + why + ". The controller is set up for \"" + sta.ssid + "\" but isn't connected right now - " +
        "out of range, wrong password, or still trying (it retries every 5 minutes). Check the Home WiFi card below (Save & Apply " +
        "reconnects straight away), then join this phone to the same network, open the address the card shows and press Retry.", true];
    }
    return ["This browser has no internet while on the OpenHaldex WiFi" + why + ". Connect the controller to your home router in the " +
      "Home WiFi (Bridge Mode) card below, join this phone to that same network, open the address the card shows and press Retry. " +
      "No router available? Use \"Update from a File\" below - it needs no internet here.", true];
  }

  async function check() {
    if (busy) return;
    const t0 = Date.now();
    const secs = () => ((Date.now() - t0) / 1000).toFixed(1) + " s";
    checkBtn.disabled = true;
    checkBtn.textContent = "Checking...";
    if (bridgeBtn) bridgeBtn.hidden = true;
    picker.hidden = true;
    index = null;
    const finish = (needBridge) => {
      checkBtn.textContent = needBridge === undefined ? "Check for updates" : "Retry";
      if (needBridge !== undefined) showRetry(needBridge);
      checkBtn.disabled = false;
    };

    // 1. The controller must be reachable from here before anything else.
    // Also refreshes "Installed" from the device itself so the comparison is
    // against what is really running, not whatever loadInfo() saw at page load.
    setStatus("1/2 Contacting the controller...");
    setState("Checking...");
    let info = null;
    try {
      const res = await updFetch("/ota/info", 6000);
      if (res.ok) info = await res.json();
    } catch (e) { /* unreachable - handled below */ }
    if (!info || !info.version) {
      setState("Controller unreachable", "upd-bad");
      setStatus("Can't reach the controller from this browser (gave up after " + secs() + "). Stay on the OpenHaldex\u2011C6 WiFi - or, if you're using the home router, " +
        "make sure the Home WiFi card shows Connected and that you opened this page at the address it gives. The controller also " +
        "switches WiFi off after 5 minutes with no CAN traffic unless Bench Mode is on. Then press Retry.", "error");
      finish(false);
      return;
    }
    window._otaInstalledVersion = info.version;
    const set = (id, v) => { const e = $(id); if (e) e.textContent = v || "--"; };
    set("updInstalled", "v" + info.version);
    set("otaFwVersion", info.version + (info.fsVersion && info.fsVersion !== "--" && info.fsVersion !== info.version ? " (web UI " + info.fsVersion + ")" : ""));

    // 2. Release index + folder listing, in parallel. Each mirror gets 12 s,
    // so a phone with no route can sit here a while - say so.
    setStatus("2/2 Contacting GitHub for the release list... (controller answered in " + secs() + "; this can take up to 30 s with no internet)");
    const [ir, fr] = await Promise.all([fetchIndex(), fetchFolders()]);

    if (!ir.index && !fr.dirs) {
      // Neither source answered. A transport failure on both means the phone
      // can't get off this network at all; an HTTP status means the internet
      // is fine and the published files are the problem.
      if (ir.reached || fr.reached) {
        setState("Release list unavailable", "upd-bad");
        setStatus("The phone is online but the release list could not be read: " + (ir.reached || fr.reached) +
          ". Nothing is wrong with the controller or the phone - the published releases are missing or broken. Use \"Update from a File\" below.", "error");
        finish(false);
      } else {
        setState("No internet access", "upd-bad");
        const adv = await offlineAdvice((ir.netErr || fr.netErr || "") + ", after " + secs());
        setStatus(adv[0], "error");
        finish(adv[1]);
      }
      return;
    }

    index = mergeSources(ir.index, fr.dirs);
    const cur = installed();
    const latest = index.latest || "";
    set("updLatest", latest ? "v" + latest : "--");
    const c = updCompareVersions(latest, cur);
    const srcNote = !ir.index ? " (release index unavailable - folder listing only, no release notes)"
      : !fr.dirs ? " (folder listing unavailable - " + (fr.reached || fr.netErr || "no answer") + "; showing the index only)" : "";
    const via = " Release list from " + UPD_MIRRORS.filter((m) => m.base === UPD_RELEASES_BASE).map((m) => m.name).join("") + " in " + secs() + ".";
    if (c > 0) { setState("Update available", "upd-available"); setStatus("v" + latest + " is available (installed v" + cur + ")." + srcNote + via); }
    else if (c === 0) { setState("Up to date", "upd-current"); setStatus("You are on the latest release." + srcNote + via); }
    else { setState("Ahead of release", "upd-current"); setStatus("Installed v" + cur + " is newer than the published v" + latest + "." + srcNote + via); }
    renderPicker();
    if (updChannel === "latest" && index.releases && index.releases.length) {
      const newest = otaCandidates().find((r) => r.version === index.latest);
      if (newest) {
        const rb = await rebuiltSinceIndexed(newest);
        if (rb.changed) {
          setState("Rebuilt since release", "upd-available");
          setStatus("v" + newest.version + " has been rebuilt since it was indexed - the published firmware is " + rb.published +
            " bytes, the release index records " + rb.indexed + ". That usually means a fix landed without the version being " +
            "bumped. Installing v" + newest.version + " again picks it up." + via);
        } else {
          setStatus("Latest build channel: could not confirm whether v" + newest.version + " has been rebuilt since release (" +
            (rb.unknown || "no comparison available") + "). Re-installing it is still the way to pick up an in-place fix." + via);
        }
      }
    }
    finish();
  }

  // Has the newest release been rebuilt in place since it was indexed?
  // releases.json records the size each .bin had when make_release.py ran, so
  // a Content-Length that disagrees is positive evidence the file changed
  // afterwards. One HEAD, no download.
  //
  // This is a one-way test. A size that matches does NOT mean the build is
  // unchanged: littlefs.bin is a fixed-size partition image, so a rebuilt
  // filesystem is always the same length, and a firmware rebuild can land on
  // the same size by chance. (On this repo today 8.00.1 and 8.00.2 match the
  // index exactly, while 8.00.3 and 9.00.0 do not - and for both of those the
  // filesystem sha differs while its size does not.) Proving the negative
  // would mean downloading both images and hashing them, which is not worth
  // doing on a check, so a match is reported as "cannot tell", never as "no".
  // Returns:
  //   {changed:true, published, indexed} - definitely rebuilt since indexing
  //   {unknown:"reason"}                 - could not tell, say so
  async function rebuiltSinceIndexed(rel) {
    const want = rel && rel.firmware && rel.firmware.size;
    if (!want || rel.unindexed) return { unknown: "this release has no indexed size to compare against" };
    const url = UPD_RELEASES_BASE + rel.firmware.path;
    let res = null;
    try {
      res = await updFetch(url, 10000, { method: "HEAD", mode: "cors" });
    } catch (e) {
      return { unknown: "the published build could not be reached" };
    }
    if (!res.ok) return { unknown: "the published build answered HTTP " + res.status };
    const len = parseInt(res.headers.get("content-length") || "0", 10);
    // Not every mirror sends Content-Length (jsDelivr may not); never guess.
    if (!len) return { unknown: "this mirror does not report a size" };
    if (len !== want) return { changed: true, published: len, indexed: want };
    return { unknown: "the firmware is the size the index expects, which does not rule out a rebuild" };
  }

  // streamed download with progress; returns a Blob and checks size when known
  async function download(rel, part, stepId) {
    const info = rel[part];
    const url =/^https?:\/\//i.test(info.path) ? info.path : UPD_RELEASES_BASE + info.path;
    setStep(stepId, "active");
    setStatus("Downloading " + part + " (v" + rel.version + ")...");
    const res = await fetch(url, { cache: "no-store", mode: "cors" });
    if (!res.ok) throw new Error("Download failed: HTTP " + res.status + " for " + info.path);
    // Progress against what the server says it is sending; the index's size
    // is only a hint (it goes stale when a .bin is rebuilt in place).
    const total = parseInt(res.headers.get("content-length") || "0", 10) || info.size || 0;
    const chunks = [];
    let got = 0;
    if (res.body && res.body.getReader) {
      const reader = res.body.getReader();
      for (;;) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push(value);
        got += value.length;
        if (total) setPct(got / total, "Download");
      }
    } else {
      const buf = await res.arrayBuffer();
      chunks.push(new Uint8Array(buf));
      got = buf.byteLength;
    }
    const cl = parseInt(res.headers.get("content-length") || "0", 10) || 0;
    if (cl && got !== cl) throw new Error(part + " download was cut short (" + got + " of " + cl + " bytes).");
    if (!got) throw new Error(part + " download was empty.");
    setPct(1, "Download");
    setStep(stepId, "done");
    return new Blob(chunks, { type: "application/octet-stream" });
  }

  async function flash(rel, part, type, stepId) {
    setStep(stepId, "active");
    setStatus("Flashing " + part + "... do not power off.");
    setPct(0, "Flash");
    const blob = rel._blobs[part];
    await window.otaUploadBlob(type, blob, part === "filesystem" ? "littlefs.bin" : "firmware.bin", {
      onProgress: (f) => setPct(f, "Flash"),
    });
    setPct(1, "Flash");
    setStep(stepId, "done");
  }

  async function waitForReboot(rel) {
    setStep("reboot", "active");
    setStatus("Device rebooting... waiting for it to come back.");
    const t0 = Date.now();
    await new Promise((r) => setTimeout(r, 4000));
    while (Date.now() - t0 < 90000) {
      setPct((Date.now() - t0) / 90000, "Reboot");
      try {
        const res = await updFetch("/ota/info", 3000);
        if (res.ok) {
          const i = await res.json();
          setStep("reboot", "done");
          setPct(1, "Done");
          window._otaInstalledVersion = i.version;
          const set = (id, v) => { const e = $(id); if (e) e.textContent = v || "--"; };
          set("updInstalled", "v" + i.version);
          set("otaFwVersion", i.version);
          if (i.version === rel.version || i.version === updCoreVersion(rel.version)) {
            setState("Installed v" + i.version, "upd-current");
            setStatus("Update complete: now running v" + i.version + ". Reload the page to pick up the new web UI.", "ok");
            setTimeout(() => location.reload(), 2500);
          } else {
            setState("Rolled back", "upd-bad");
            setStatus("Device came back on v" + i.version + " instead of v" + rel.version + " - the new image was rejected or rolled back. Try again or use \"Update from a File\".", "error");
          }
          return;
        }
      } catch (e) { /* still rebooting - AP may drop and rejoin, or the home router lease takes a moment */ }
      await new Promise((r) => setTimeout(r, 2000));
    }
    setStatus("Device didn't respond within 90 s. Reconnect to the OpenHaldex WiFi (or the home network) and reload this page.", "error");
  }

  // Install sequence for a release from the list: web UI first (downloaded,
  // flashed, verified), then firmware, then wait for the reboot.
  async function runInstall(rel) {
    busy = true;
    checkBtn.disabled = true;
    if (installBtn) installBtn.disabled = true;
    sel.disabled = true;
    resetSteps();
    if (wrap) wrap.hidden = false;
    setPct(0);
    let stage = "check";
    try {
      const safe = await fetchJson("/ota/check");
      if (!safe || !safe.allowed) throw new Error("Blocked: " + ((safe && safe.reason) || "system not safe for update."));

      stage = "dlfs";
      rel._blobs.filesystem = await download(rel, "filesystem", "dlfs");
      stage = "fs";
      await flash(rel, "filesystem", "filesystem", "fs");

      // verify: the device has already remounted; check the web UI version it holds
      stage = "verify";
      setStep("verify", "active");
      setStatus("Verifying filesystem...");
      const fsi = await fetchJson("/ota/fsinfo");
      if (!fsi || !fsi.ok) throw new Error("Filesystem verification failed (" + ((fsi && fsi.error) || "not mounted") + "). Retry the update.");
      if (fsi.fsVersion && fsi.fsVersion !== "--" && fsi.fsVersion !== rel.version && fsi.fsVersion !== updCoreVersion(rel.version)) {
        throw new Error("Filesystem reports v" + fsi.fsVersion + ", expected v" + rel.version + ". Retry the update.");
      }
      setStep("verify", "done");
      rel._blobs.filesystem = null;

      stage = "dlfw";
      rel._blobs.firmware = await download(rel, "firmware", "dlfw");
      stage = "fw";
      await flash(rel, "firmware", "firmware", "fw");
      rel._blobs.firmware = null;
      await waitForReboot(rel);
    } catch (e) {
      let msg = e.message;
      if (stage === "fs" || stage === "verify") {
        // The device wipes a rejected filesystem image, so the firmware keeps
        // running but this web UI is gone until littlefs.bin goes on again.
        msg += " The controller is still running v" + installed() + "; the web UI partition was cleared. Press Install again " +
          "(or upload littlefs.bin under \"Update from a File\"). If this page won't load, the controller now shows a recovery page at its address.";
      }
      setStatus(msg, "error");
      if (wrap) wrap.hidden = true;
    }
    rel._blobs = null;
    busy = false;
    checkBtn.disabled = false;
    if (installBtn) installBtn.disabled = false;
    sel.disabled = false;
  }

  function install() {
    const rel = selected();
    if (!rel || busy) return;
    const cur = installed();
    const dir = updCompareVersions(rel.version, cur);
    const what = dir < 0 ? "roll back to v" + rel.version : (dir === 0 ? "re-install v" + rel.version : "update to v" + rel.version);
    let msg = "This will " + what + " (currently v" + cur + ").\n\nThe web UI is replaced first, then the firmware, then the device reboots. Keep this page open.";
    if (dir < 0) msg += "\n\nRolling back: older releases may not have this update page, so coming forward again could mean a USB flash. Settings may also be reset - export a backup first (Diagnostics tab).";
    if (dir === 0) {
      msg += "\n\nSame version number: this re-downloads whatever is published under v" + rel.version + " right now. If that " +
        "build was rebuilt after release it will bring the newer code in; if it was not, you end up back where you started. " +
        "It is not a published release in its own right, so export a settings backup first (Diagnostics tab).";
    }
    if (!confirm(msg + "\n\nContinue?")) return;
    rel._blobs = {};
    runInstall(rel);
  }

  // --- update channel -----------------------------------------------------
  // Kept in localStorage rather than on the controller: nothing on the device
  // talks to GitHub (it has no internet), so this only affects this browser,
  // and opting into unreleased builds should not silently follow the device to
  // whoever opens it next.
  const chanSel = $("updChannel"), chanHint = $("updChannelHint");
  function renderChannel() {
    if (chanSel) chanSel.value = updChannel;
    if (!chanHint) return;
    if (updChannel === "latest") {
      chanHint.textContent = "A fix sometimes lands before the next version number is cut, and the newest release is then rebuilt in place under the same version. " +
        "On this channel the newest release stays installable even when its version matches what you already have, and a check reports whether it has been rebuilt since it was indexed. " +
        "These builds have not been through a release - keep a settings backup (Diagnostics tab).";
      chanHint.hidden = false;
    } else {
      chanHint.hidden = true;
    }
  }
  try {
    const saved = localStorage.getItem(UPD_CHANNEL_KEY);
    if (saved === "latest" || saved === "stable") updChannel = saved;
  } catch (e) { /* private mode - stay on stable */ }
  renderChannel();
  if (chanSel) {
    chanSel.addEventListener("change", () => {
      updChannel = chanSel.value === "latest" ? "latest" : "stable";
      try { localStorage.setItem(UPD_CHANNEL_KEY, updChannel); } catch (e) { /* not fatal */ }
      renderChannel();
      // Re-check rather than re-label a stale result: the rebuild check only
      // runs on the latest channel, so the previous status may not apply.
      index = null;
      picker.hidden = true;
      resetSteps();
      setState("Not checked");
      setStatus(updChannel === "latest"
        ? "Latest build channel selected. Press Check for updates."
        : "Stable channel selected. Press Check for updates.");
      checkBtn.textContent = "Check for updates";
      if (bridgeBtn) bridgeBtn.hidden = true;
    });
  }

  checkBtn.addEventListener("click", check);
  if (bridgeBtn) bridgeBtn.addEventListener("click", goBridge);
  const goLink = $("updGoBridge");
  if (goLink) goLink.addEventListener("click", goBridge);
  if (installBtn) installBtn.addEventListener("click", install);
  sel.addEventListener("change", renderNotes);
  if (showAll) showAll.addEventListener("change", () => { if (index) renderPicker(); });
}

// ---------------------------------------------------------------------------
// Home WiFi (bridge mode) card - PR #39 (louij2), ported. The controller joins
// a home/garage network as a station alongside its own AP. Status is polled
// because association takes a few seconds after a save or restart.
//
// The card exists twice - Diagnostics ("wifiSta..." ids) and the OTA tab
// ("otaWifiSta..."), where it's the way to get the phone online for the GitHub
// update check - so the element ids are built from a prefix.
// ---------------------------------------------------------------------------
function initWifiSta(prefix) {
  const p = prefix || "wifiSta";
  const ssidInput = document.getElementById(p + "SsidInput");
  const ssidList = document.getElementById(p + "SsidList");
  const scanBtn = document.getElementById(p + "Scan");
  const pwInput = document.getElementById(p + "PasswordInput");
  const pwToggle = document.getElementById(p + "PasswordToggle");
  const status = document.getElementById(p + "Status");
  const btnSave = document.getElementById(p + "Save");
  const btnReset = document.getElementById(p + "Reset");
  if (!ssidInput || !pwInput || !status || !btnSave || !btnReset) return;

  // Unsaved edits in either field hold the periodic refresh off the SSID box,
  // so typing is never clobbered - including after tabbing into the password.
  let userEditing = false;
  ssidInput.addEventListener("input", () => { userEditing = true; });
  pwInput.addEventListener("input", () => { userEditing = true; });

  const signalQuality = (rssi) => (rssi >= -50 ? "excellent" : rssi >= -60 ? "good" : rssi >= -70 ? "fair" : "weak");

  function renderStatus(d) {
    if (!d || !d.ssid) {
      status.textContent = "Disabled - AP only";
      status.style.color = "var(--text-dim)";
    } else if (d.connected) {
      const sig = typeof d.rssi === "number" ? " (" + signalQuality(d.rssi) + " signal, " + d.rssi + " dBm)" : "";
      status.textContent = "\u2713 Connected to \"" + d.ssid + "\"" + sig + " - reachable at http://" + d.ip + "/ and http://openhaldex.local/";
      status.style.color = "var(--success)";
    } else {
      status.textContent = "Configured for \"" + d.ssid + "\" - not connected (out of range, or still trying)";
      status.style.color = "var(--text-dim)";
    }
  }

  function refresh() {
    fetchJson("/api/wifi/sta").then((d) => {
      if (!d) return;
      if (!userEditing) ssidInput.value = d.ssid || "";
      renderStatus(d);
    });
  }
  refresh();
  setInterval(refresh, 5000);

  if (pwToggle) pwToggle.addEventListener("click", () => {
    const hidden = pwInput.type === "password";
    pwInput.type = hidden ? "text" : "password";
    pwToggle.textContent = hidden ? "\u{1F648}" : "\u{1F441}";
  });

  // Network scan: explicit button, never automatic - the single radio leaves
  // the AP's channel for the scan. The device runs it asynchronously; poll
  // until the list is back (a few seconds).
  if (scanBtn) scanBtn.addEventListener("click", async () => {
    scanBtn.disabled = true;
    const prev = ssidInput.placeholder;
    ssidInput.placeholder = "Scanning...";
    let resp = null;
    for (let i = 0; i < 12; i++) {
      resp = await fetchJson("/api/wifi/scan");
      if (resp && !resp.scanning) break;
      await new Promise((r) => setTimeout(r, 700));
    }
    ssidInput.placeholder = prev;
    scanBtn.disabled = false;
    if (!resp || !Array.isArray(resp.networks)) { showNotification("Scan failed", "error"); return; }
    if (ssidList) {
      ssidList.innerHTML = "";
      resp.networks.forEach((n) => {
        const o = document.createElement("option");
        o.value = n.ssid;
        o.textContent = n.ssid + (n.secure ? " \u{1F512}" : "") + " (" + n.rssi + " dBm)";
        ssidList.appendChild(o);
      });
    }
    showNotification(resp.networks.length + " network" + (resp.networks.length === 1 ? "" : "s") + " found - pick from the list");
    ssidInput.focus();
  });

  btnSave.addEventListener("click", async () => {
    const ssid = ssidInput.value.trim();
    const pwd = pwInput.value;
    if (ssid.length > 32) { showNotification("SSID too long (max 32)", "error"); return; }
    if (pwd.length > 0 && pwd.length < 8) { showNotification("Password must be at least 8 characters, or blank", "error"); return; }
    const resp = await fetchJson("/api/wifi/sta", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ ssid: ssid, password: pwd }),
    });
    if (!resp) { showNotification("Failed to reach device", "error"); return; }
    if (!resp.ok) { showNotification(resp.error || "Failed to save", "error"); return; }
    userEditing = false;
    pwInput.value = "";
    if (ssid) {
      status.textContent = "Connecting to \"" + ssid + "\"... (the AP restarts - reconnect if you drop off)";
      status.style.color = "var(--text-dim)";
      showNotification("Home WiFi saved - connecting...");
    } else {
      status.textContent = "Disabled - AP only";
      showNotification("Bridge mode disabled");
    }
  });

  btnReset.addEventListener("click", async () => {
    const resp = await fetchJson("/api/wifi/sta/reset", { method: "POST" });
    if (!resp || !resp.ok) { showNotification("Failed to disable", "error"); return; }
    userEditing = false;
    ssidInput.value = "";
    pwInput.value = "";
    status.textContent = "Disabled - AP only";
    status.style.color = "var(--text-dim)";
    showNotification("Bridge mode disabled");
  });
}

// ---------------------------------------------------------------------------
// Backup & Restore card - PR #39 (louij2), ported and widened to everything
// this firmware's /api/settings accepts back. The device already ignores keys
// it doesn't know, so the list below is only there to keep out the entries
// that are actions or would change what the controller is doing right now
// (frame-edit commands, the SavvyCAN analyser toggles).
// ---------------------------------------------------------------------------
const BACKUP_GENERAL_KEYS = [
  "haldexGeneration", "isStandalone", "useCANifAvailable", "broadcastOpenHaldexOverCAN", "disableController",
  "disengageUnderSpeed", "disengageAboveSpeed", "disableThrottle",
  "tcForceMode", "tcForceModeValue", "hazardForceMode", "hazardForceModeValue",
  "extButtonForceMode", "extBtnForceModeValue", "forceModesPriority", "disableOnboardButton", "disableExternalButton",
  "followBrake", "invertBrake", "followHandbrake", "invertHandbrake",
  "fixHunting", "dangerZoneEnabled", "esp14MinFloorPct", "bpkCeilingNm",
  "slipWheelbaseMm", "slipTrackFrontMm", "slipTrackRearMm", "slipSteeringRatio", "slipMinSpeedRaw",
  "lockReleaseEnabled", "lockReleaseRampMs", "lockEngageRampMs",
  "steeringGainEnabled", "steeringGainStartDeg", "steeringGainFullDeg", "steeringGainFloor",
  "liveDiagEnabled", "ledBrightness",
  "canSleepEnabled", "canSleepAggressive", "benchMode", "lpWakeThresholdFps",
  "longLearnNotes", "bleEnabled", "espNowEnabled", "espNowControl",
];

function initBackupRestore() {
  const btnExport = document.getElementById("backupExport");
  const btnImport = document.getElementById("backupImportBtn");
  const fileInput = document.getElementById("backupImportFile");
  const status = document.getElementById("backupStatus");
  const pwSection = document.getElementById("backupPwSection");
  const pwInput = document.getElementById("backupPwInput");
  const pwApply = document.getElementById("backupPwApply");
  const staPwSection = document.getElementById("backupStaPwSection");
  const staPwInput = document.getElementById("backupStaPwInput");
  const staPwApply = document.getElementById("backupStaPwApply");
  if (!btnExport || !btnImport || !fileInput || !status) return;

  const setStatus = (msg, ok) => { status.textContent = msg; status.style.color = ok ? "var(--success)" : "var(--danger)"; };
  const post = (url, body) => fetchJson(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });

  btnExport.addEventListener("click", async () => {
    setStatus("Exporting...", true);
    const [settings, ssidData, pwData, staData, slotList] = await Promise.all([
      fetchJson("/api/settings"), fetchJson("/api/wifi/ssid"), fetchJson("/api/wifi"), fetchJson("/api/wifi/sta"), fetchJson("/api/maps"),
    ]);
    if (!settings) { setStatus("Export failed - couldn't reach the device", false); return; }
    const stamp = new Date().toISOString();
    // The BLE pairing code is shown on the Bluetooth card but must not travel in a backup file.
    const exportable = Object.assign({}, settings);
    delete exportable.blePasskey;
    // On-device map slots (Expert tab > Maps) so a firmware update cannot lose them.
    const mapSlots = [];
    if (slotList && Array.isArray(slotList.slots)) {
      for (const sl of slotList.slots) {
        if (!sl.used) continue;
        const m = await fetchJson("/api/maps/get?index=" + sl.index);
        if (m && m.ok) mapSlots.push({ index: sl.index, name: m.name, speedArray: m.speedArray, throttleArray: m.throttleArray, lockArray: m.lockArray });
      }
    }
    const backup = {
      _product: "OpenHaldex-C6",
      _exportedAt: stamp,
      _fwVersion: settings.FW_VERSION,
      settings: exportable,
      mapSlots: mapSlots,
      wifi: { ssid: ssidData ? ssidData.ssid : null, passwordSet: !!(pwData && pwData.passwordSet) },
      wifiSta: { ssid: staData && staData.ssid ? staData.ssid : null, passwordSet: !!(staData && staData.passwordSet) },
    };
    const blob = new Blob([JSON.stringify(backup, null, 2)], { type: "application/json" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = "openhaldex-backup-" + stamp.replace(/[:.]/g, "-") + ".json";
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
    setStatus("Exported \u2713 " + stamp, true);
    showNotification("Config exported");
  });

  btnImport.addEventListener("click", () => fileInput.click());

  fileInput.addEventListener("change", async () => {
    const file = fileInput.files[0];
    if (!file) return;
    if (pwSection) pwSection.style.display = "none";
    if (staPwSection) staPwSection.style.display = "none";
    setStatus("Reading " + file.name + "...", true);
    let backup;
    try { backup = JSON.parse(await file.text()); } catch (e) {
      setStatus("Not a valid backup file (bad JSON)", false); fileInput.value = ""; return;
    }
    const s = backup.settings || {};
    if (!Array.isArray(s.throttleArray) || !Array.isArray(s.speedArray) || !Array.isArray(s.lockArray)) {
      setStatus("Backup file is missing the tune table", false); fileInput.value = ""; return;
    }
    if (!confirm("Restore settings from " + file.name + (backup._fwVersion ? " (exported from v" + backup._fwVersion + ")" : "") +
      "?\n\nThis replaces the Expert tune, steering scale, frame edits and settings on the controller.")) { fileInput.value = ""; return; }

    // 1. Generation first, so the frame-edit mask below lands on the right table.
    if (typeof s.haldexGeneration === "number") await post("/api/settings", { haldexGeneration: s.haldexGeneration });

    // 2. Tune table (+ steering scale if the backup has it).
    setStatus("Restoring Expert tune...", true);
    const tune = { throttleArray: s.throttleArray, speedArray: s.speedArray, lockArray: s.lockArray };
    if (Array.isArray(s.steeringArray) && Array.isArray(s.steeringLockScaleArray)) {
      tune.steeringArray = s.steeringArray;
      tune.steeringLockScaleArray = s.steeringLockScaleArray;
    }
    const tuneResp = await post("/api/tune", tune);
    if (!tuneResp || !tuneResp.ok) { setStatus("Failed to restore the tune table - is the device reachable?", false); fileInput.value = ""; return; }

    // 3. General settings.
    setStatus("Restoring settings...", true);
    const general = {};
    BACKUP_GENERAL_KEYS.forEach((k) => { if (k in s) general[k] = s[k]; });
    await post("/api/settings", general);

    // 4. Frame edits for this generation, one bit at a time (that's the API).
    if (Array.isArray(s.frameBlocks)) {
      for (const fb of s.frameBlocks) {
        if (typeof fb.bit === "number" && typeof fb.enabled === "boolean") {
          await post("/api/settings", { frameEditBit: fb.bit, frameEditOn: fb.enabled });
        }
      }
    }

    // 4b. Expert map slots (older backups have none).
    if (Array.isArray(backup.mapSlots)) {
      setStatus("Restoring map slots...", true);
      for (const m of backup.mapSlots) {
        if (m && typeof m.index === "number" && m.name) {
          await post("/api/maps/save", { index: m.index, name: m.name, speedArray: m.speedArray, throttleArray: m.throttleArray, lockArray: m.lockArray });
        }
      }
    }

    // 5. WiFi identity (names only - passwords are never in the file).
    const wifi = backup.wifi || {};
    if (wifi.ssid) await post("/api/wifi/ssid", { ssid: wifi.ssid });
    const wifiSta = backup.wifiSta || {};
    if (wifiSta.ssid) await post("/api/wifi/sta", { ssid: wifiSta.ssid, password: "" });

    const needsApPw = !!(wifi.passwordSet && pwSection);
    const needsStaPw = !!(wifiSta.passwordSet && staPwSection);
    if (needsApPw) pwSection.style.display = "";
    if (needsStaPw) staPwSection.style.display = "";
    if (needsApPw || needsStaPw) {
      setStatus("Tune + settings restored \u2713. Enter the password(s) below to finish.", true);
    } else {
      setStatus("Restored \u2713 - reload the page to see the new values.", true);
      showNotification("Config imported");
    }
    fileInput.value = "";
  });

  if (pwApply) pwApply.addEventListener("click", async () => {
    const pwd = pwInput.value;
    if (pwd.length < 8) { setStatus("Password must be at least 8 characters", false); return; }
    const resp = await post("/api/wifi", { password: pwd });
    if (!resp || !resp.ok) { setStatus("Failed to apply the AP password", false); return; }
    pwInput.value = "";
    pwSection.style.display = "none";
    setStatus("Restored \u2713 - the AP is restarting, reconnect to WiFi...", true);
    showNotification("Config imported");
  });

  if (staPwApply) staPwApply.addEventListener("click", async () => {
    const pwd = staPwInput.value;
    if (pwd.length > 0 && pwd.length < 8) { setStatus("Home WiFi password must be at least 8 characters (or blank for an open network)", false); return; }
    const cur = await fetchJson("/api/wifi/sta");
    const ssid = cur && cur.ssid ? cur.ssid : "";
    const resp = await post("/api/wifi/sta", { ssid: ssid, password: pwd });
    if (!resp || !resp.ok) { setStatus("Failed to apply the home WiFi password", false); return; }
    staPwInput.value = "";
    staPwSection.style.display = "none";
    setStatus("Restored \u2713 - connecting to home WiFi...", true);
    showNotification("Config imported");
  });
}
