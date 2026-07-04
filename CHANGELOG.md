# Changelog

All notable changes to OpenHaldex-C6 Edge are listed here.

---

## Unreleased

### Added
- Haldex live data over UDS (Gen5/MQB only, off by default): a background
  poller opens an extended diagnostic session with the coupling and round-robins
  seven ReadDataByIdentifier requests for clutch temperature, cooling fin
  temperature, module temperature, pump current, pump voltage, clutch PWM duty
  and supply voltage, shown on a new Home page card that appears when the
  toggle is on. The request format, DID table and per-value scaling were
  recovered from the upstream V8.00.2 binary. Responses are diverted out of the
  Haldex receive path by CAN ID, so the bridge forwarding behaviour is
  unchanged; polling stops automatically in analyzer mode, during sleep
  suppression, on bus failure and on non-Gen5 units. The request/response IDs
  (0x70F/0x779), DID formulas and byte order were confirmed against the
  upstream V8.00.2 source: the two temperature readings are little-endian on
  the wire while pump current and voltage are big-endian, and the decoder
  matches that mixed order.
- Hazard force mode (off by default): while the hazard switch is on, the
  coupling is forced fully open (FWD), overriding every mode and every other
  force mode. Decoded from the MQB Blinkmodi_02 frame (0x366, byte 2 bit 4) as
  a level signal, so engagement follows the switch with no latching. Intended
  for towing, recovery and limping home where a locked coupling can bind the
  driveline. Toggle lives under Input Options on the Settings page; the live
  hazard state is exposed in the status API for the dashboard.
- Steering-gain reduction (off by default): optionally scales the lock target
  down as steering angle increases, so the rear axle is not fighting the front
  through tight corners and car parks. The angle is decoded from the MQB LWI_01
  (0x086) frame; gain stays 100% up to a configurable start angle, ramps
  linearly to a configurable minimum by the full angle, and the reduction is
  bypassed whenever the steering signal is stale or flagged degraded (fails
  toward stock behaviour). Toggle and sliders live on the Basic page; the live
  steering angle and currently applied gain show on the Diagnostics page so the
  decode can be verified on the car before enabling.
- Overnight sleep (off by default): stops the controller flattening the battery
  after key-off. The Haldex bus going quiet (the Haldex ECU is ignition-powered)
  is treated as the car switching off: all OpenHaldex CAN transmission stops so
  the car's gateway can complete its shutdown, and once every bus has been fully
  silent for 30 seconds the WiFi access point powers down too. Any bus activity
  wakes everything back up - unlocking the car is enough for WiFi, ignition-on
  restores full operation. In standalone mode a short probe burst every 10
  seconds keeps the wake path alive for Haldex units that only talk when spoken
  to. A calibrate button on the Settings page measures how long the car's
  electronics keep chattering after key-off and sets the sleep threshold
  automatically; the live sleep state and per-bus quiet times show on the
  Diagnostics page.
- Lock response rate limiting (off by default, "Instant" preserves stock
  behaviour): independent engage and release rates (%/s) slew the lock target
  toward new values instead of slamming the coupling on and off as the map value
  jumps. Applied at the single choke point both the CAN-modify path and the
  standalone generators share, so every Haldex generation ramps uniformly.
  Sliders live on the Expert page next to the map.
- Dashboard redesign and mobile pass: engagement gauge hero with a target tick,
  Haldex status chips, a Map Curves chart on the Expert page (lock vs speed or
  throttle with a live operating-point dot), a fixed bottom tab bar with icons on
  phones, and general touch/reduced-motion polish.
- First-run setup flow: navigating to `/` on an unprovisioned device now redirects
  to `/setup`, a minimal inline form that collects and confirms a password (8-63
  chars) and stores it to NVS. Once a credential is set, the `/setup` routes close
  permanently (403 / redirect), and all further credential rotation goes through the
  auth-gated `POST /ota/credential`.
- Per-bus dropped-transmit counters on the Diagnostics page. The standalone
  generators, the pass-through path, the OpenHaldex broadcast and the analyzer
  inject paths now route through one wrapper that counts a drop when the transmit
  fails (TX queue full or bus off) instead of discarding the result, so silent
  frame loss is visible. The UDS transmit paths were already checked and are
  unchanged.

### Fixed
- The "Broadcast over CAN" toggle now actually works. The setting was stored,
  loaded and shown in the UI, but the broadcast task never checked it, so the
  0x6B0 state frame transmitted every 200 ms regardless.
- The analyzer SLCAN frame writer clamps DLC to 8, matching the GVRET writer. A
  received classic-CAN frame can report a DLC of 9-15, and the unclamped loop
  could write past the end of the line buffer on extended-ID frames.
- `POST /api/settings` now validates the whole request before applying any
  field. Previously a body with a valid generation and an invalid value later in
  the payload returned 400 after the earlier fields had already been applied.
- The expert-map live trace highlights the correct cell when the speed and
  throttle axes have different lengths (the row stride used the throttle axis
  length instead of the speed axis length; correct today only because both are 7).
- Selecting a Haldex generation no longer overwrites the stored drive mode. It
  previously wrote the generation number into the persisted mode, forcing the car
  to FWD on the next reboot.
- A freshly seeded device now defaults to a valid Haldex generation (1) instead of
  the invalid 0, which left the controller inert until a generation was picked by hand.
- Standalone-mode CAN frames are now built on a zero-initialised message struct, so
  the extended-ID / remote / single-shot flag bits are deterministic instead of
  taking whatever was left on the stack.
- `openhaldex.local` keeps resolving after the WiFi-reset long-press. The access
  point bring-up is now a single shared routine that also rebinds the mDNS responder
  to the fresh network interface.
- The mode LED switch now uses the mode enum names with an explicit default, so a
  reordered enum can't silently mismap colours and an unknown mode shows a clear
  fault (LED off) rather than holding its previous colour.
- `/ota/info` no longer emits uninitialised strings when the partition description
  read fails, and always returns valid JSON.
- The analyzer GVRET frame writer clamps DLC to 8 for both the copy and the wire
  length, removing a defensive out-of-bounds path.
- The "disengage under speed" gate is corrected. It was both inverted and switched
  off by its own default, so setting an under-speed limit did nothing. Lock now
  disengages below the set speed and re-engages at or above it, with an optional
  independent upper-speed cut. Force mode (TC/ESP or external button) still bypasses
  the speed gate, so a stationary launch through force mode locks as before.
- The web mode endpoint now rejects an out-of-range mode value instead of storing an
  invalid enum, matching the validation already on the CAN control path.
- Settings now reject an inverted disengage window (under-speed above the above-speed,
  with both active), which would otherwise leave the speed gate false at every speed
  and stop passive-mode lock from ever engaging.
- Expert tune uploads are validated at the API boundary: speed cells are clamped to
  0-300, throttle and lock cells to 0-100, and both axes must be strictly ascending
  or the upload is rejected, so a bad map can't silently mis-interpolate.
- The web UI now authenticates to its own protected endpoints. A Device Password
  field on the Settings page stores the first-run password in the browser and sends
  it with every control action (mode, settings, tune, learn); the UI shows clear
  "wrong password" (401) and "not provisioned" (503) messages instead of a silent
  failure. Previously, once a password was set, control actions could quietly not
  take effect.
- The OTA safety check no longer changes the drive mode as a side effect of a status
  read. `GET /ota/check` is now purely read-only; the controller is forced to STOCK
  only inside the authenticated flash path, immediately before the flash begins.
- The CAN receive paths drop short or malformed frames instead of decoding fixed
  byte offsets from stale data. A truncated or spoofed frame on an accepted ID can
  no longer set a bogus speed or trigger force mode from uninitialised bytes.
- The UDS response reader validates that each frame actually carried the bytes it
  claims, and checks consecutive-frame sequence numbers, so a short or out-of-order
  response is rejected rather than reassembled into corrupt data. The STmin comment
  is corrected to the ISO 15765-2 units.
- Settings persistence now surfaces a warning when a flash (NVS) write fails, instead
  of settings silently ceasing to save.
- Expert-map axis edits in the web UI are validated (speed 0-300, throttle 0-100, and
  both axes kept strictly ascending), mirroring the firmware-side check.
- Replaced the exact floating-point equality checks on the lock target with an
  integer comparison, removing a fragile `==` on a float.
- The analyzer TCP listener on port 23 is now released when analyzer mode is turned
  off (or WiFi drops), instead of leaving the socket bound. It is re-opened only on a
  real off-to-on transition.

### Removed
- A batch of dead globals found in the source audit: variables that were
  declared, and in some cases persisted to flash or reported over the API, but
  never read by any code path (`otaUpdate`, `lockReleaseRatePerSec`,
  `rxtxcount`, `isMPH`, unused standalone counters, and others). No behaviour
  change; the settings API simply no longer reports a value nothing consumes.
- Generation 3 (Volvo) dropdown option, which the firmware silently rejected.
- Dead per-message checksum wrappers and a duplicate checksum table.
- The large block of commented-out frame-generator code inside `getLockData`
  (unused Gen5 cases), which buried the live cases. Recoverable from git history.

---

## Foundational fork improvements (before this changelog)

These are the stability and security changes made against upstream before this
file existed. They are grouped by area; the git log carries the per-commit detail.

### Security

- **Committed OTA password removed.** The upstream firmware had a hardcoded
  `OTA_PASSWORD` literal committed to source. It is removed and flashing fails
  closed (HTTP 503) until a credential is provisioned at build time or at runtime
  in NVS. Credentials are rotatable via the authenticated `POST /ota/credential`.
- **Unauthenticated UDS injection closed.** The `GET /api/uds/read` endpoint
  transmitted a 0x22 UDS read-request frame onto CAN with no auth check. It is now
  behind the same fail-closed OTA credential (401/503 when unauthenticated).
- **Unauthenticated CAN injection on the analyzer port closed.** The analyzer TCP
  port (23) accepted GVRET and SLCAN host-to-device transmit frames from any client.
  A fail-closed injection gate is evaluated once per TCP connection; injection is
  refused until an OTA credential is provisioned. Passive sniffing is unaffected.

### Firmware correctness

- **Button bindings corrected.** The onboard and external mode-button long-press
  handlers were crossed. Onboard long-press now disconnects WiFi; external
  long-press sets force mode and external key-up releases it.
- **CAN bus-off recovery automated.** A bus-off event previously left the CAN
  controller stopped until power-cycle. `TWAI_ALERT_BUS_RECOVERED` is now detected,
  `twai_start_v2` is called on the recovered bus, and per-bus failure state is
  tracked so a single-bus recovery never clears the global latch while the other
  bus is still stopped.
- **Haldex engagement clamping.** `map()` (which extrapolates outside its input
  range and wraps a `uint8_t`) is replaced with a clamping function, so received
  bytes outside the valid window are capped, not wrapped, and the lock value fed to
  the Haldex is always in range.
- **Learn-table over-request clamping.** Requesting more lock than the device had
  learned returned zero; it now returns the highest learned value.
- **ESP_10 (0x116) E2E checksum DataID corrected.** The `ID_SEQ_116` array was all
  `0x05`, a verbatim copy of the EPB_01/0x104 table, so the live pass-through and
  standalone generator both produced a wrong checksum for the ESP_10 frame. The
  correct VW MQB E2E DataID `0xac` is now used; the dead `ID_SEQ_110`/`checksum_110`
  code is removed. Bench validation against real hardware is still pending.
- **MOTOR5 rolling counter fixed.** The MOTOR5 standalone frame incremented
  `BRAKES1_counter` instead of `MOTOR5_counter`, leaving its counter byte stuck at
  zero and double-advancing BRAKES1.
- **UDS response-buffer capacity fix.** `UDS::sendRequest` zeroed the in/out
  `responseLen` before the copy bounds were applied, collapsing every bound to zero
  so every `readDataByIdentifier` returned zero bytes. Capacity is now captured
  before it is zeroed.

### Memory and API safety

- **HTTP body buffer allocation corrected.** The body buffer was allocated with
  `new String()` but ESPAsyncWebServer frees `_tempObject` with `free()`, which is
  undefined behaviour on a mid-body abort. It is now a `malloc`-owned buffer matched
  to the `free()` call.
- **Explicit error responses on every handler exit path.** The `/api/mode`,
  `/api/settings`, and `/api/tune` handlers could return without sending a response,
  leaving the socket open to timeout. They now send explicit `400` on malformed
  input and `200` on success.

### Concurrency and persistence

- **Standalone frame tasks guarded with `stateMutex`.** All six standalone
  frame-generation tasks read shared safety state (`state.mode`, `lock_target`)
  without the mutex; the full generator-selection body of each is now guarded.
- **NVS persistence refactored onto one namespace.** `readEEP` called
  `pref.begin()` with a setting name as the namespace per call, so only the
  last-opened namespace persisted. It is consolidated onto a single `openhaldex`
  namespace with a first-run seeded sentinel and one-time migration from the prior
  de-facto namespace.

### Test coverage

- **Host-runnable native test suite added.** A PlatformIO `[env:native]`
  environment with Unity characterization tests pins the pure-function core:
  CRC8/AUTOSAR checksum, expert-map interpolation, lock-target rate limiting,
  and steering-gain reduction.
