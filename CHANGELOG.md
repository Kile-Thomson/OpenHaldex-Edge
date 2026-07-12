# Changelog

All notable changes to OpenHaldex-C6 Edge are listed here.

This fork builds on Forbes Automotive's OpenHaldex-C6 **V8.00.2**. The entries
below are what this fork changes on top of that base, each checked against the
V8.00.2 source. Features already present in V8.00.2 - the MQB UDS live-data
readout, hazard force-open, the low-power sleep system and lock-release rate
limiting - are Forbes's own work and are not repeated here.

---

## Unreleased

### Security

- **Committed OTA password removed.** V8.00.2 has a hardcoded `OTA_PASSWORD`
  literal in source and prints it to the serial log. It is removed and flashing
  fails closed (HTTP 503) until a credential is provisioned - on first power-up
  the device redirects to a `/setup` page where you set your own password, which
  is stored to NVS. Credentials are rotatable via the authenticated
  `POST /ota/credential`. There is no build-time or compiled-in credential path:
  the only way to set a password is at runtime, so no build step is required.
- **State-changing endpoints require authentication.** In V8.00.2 the mode,
  settings, tune and learn endpoints (and host-to-device CAN injection on the
  analyzer port) accept commands from any client on the AP. They now require the
  provisioned password; the web UI sends it automatically and reports a clear
  wrong-password (401) or not-provisioned (503) message instead of silently
  failing. Passive analyzer sniffing is unaffected.
- **HTTP body buffer allocation corrected.** The request body buffer was
  allocated with `new String()` but ESPAsyncWebServer frees `_tempObject` with
  `free()`, which is undefined behaviour on a mid-body abort. It is now a
  `malloc`-owned buffer matched to the `free()` call, with the sentinel restored
  so trailing chunks of a rejected body no-op.

### Firmware correctness

- **UDS::sendRequest capacity bug fixed.** The function zeroed the in/out
  `responseLen` before it was used as the copy bound, collapsing every bound to
  zero so every `readDataByIdentifier` returned zero bytes. Capacity is now
  captured before it is zeroed, so the built-in ECU read returns real data.
- **Haldex fin/clutch temp raw words exposed for calibration.** The 16-bit temp
  DIDs (`0x2BE4` cooling fin, `0x2BF1` clutch) are decoded with a
  `(rawLE - 22767) / 100` scale disassembled from the upstream binary that has
  never been checked against a known temperature. The decoded reading still
  displays, and the raw 16-bit wire words are now also exposed in the `/api` JSON
  (`coolingFinTempRaw`, `clutchTempRaw`) so the true offset and scale can be
  solved against live VCDS values once captured. The current scale is left in
  place until real data replaces it - no second guess is stamped over it.
- **ESP_10 (0x116) E2E checksum DataID corrected.** The `ID_SEQ_116` array was
  all `0x05`, a verbatim copy of the EPB_01/0x104 table, so the live pass-through
  and standalone generator both produced a wrong checksum for the ESP_10 frame.
  The correct VW MQB E2E DataID `0xac` is now used. Bench validation against a
  real Gen5 unit is still pending.
- **CAN bus-off recovery automated.** In V8.00.2 the bus-health alert read only
  ran inside an `if (detailedDebugCAN)` block, used the legacy single-bus
  `twai_read_alerts`, and had no recovery path, so a bus-off event left the
  controller stopped until power-cycle. It is now an always-on dual-bus
  `twai_read_alerts_v2` per handle, `twai_start_v2` is called on the recovered
  bus, and per-bus failure state is tracked so a single-bus recovery never clears
  the latch while the other bus is still stopped.
- **Uploaded lock-table rows validated against the speed axis.** The tune-upload
  handler checked each lock-table row length against the throttle axis count when
  the row is indexed by the speed axis. Masked today because both are 7, but a
  malformed row would slip through if the grid dimensions ever diverged.
- **Received engagement percentage clamped instead of extrapolated.** The CAN
  parser scaled the raw Haldex engagement byte with Arduino `map()`, which
  extrapolates outside its input window. A raw byte below the low bound produced a
  negative percentage that wrapped when stored in the `uint8_t` engagement global
  (e.g. -1 became 255), and a byte above the high bound exceeded 100. The scaling
  now clamps into the window first, so every generation's decoded engagement stays
  0-100; in-window frames are byte-identical to before.
- **Speed-disengage gate corrected.** The passive-mode speed gate read
  `(under==0) || (speed<=under) || (speed>=above)`, which was always true because
  the upper bound defaults to 0 (so `speed>=0` always held) and which disengaged
  lock in the wrong band. Lock is now permitted only while the vehicle speed is at
  or above the lower cut AND at or below the upper cut, with either bound of 0
  disabling that side.
- **Learn-table over-request clamps to the highest learned engagement.** When a
  requested lock target exceeded every value in the learn table, the correction-
  factor search fell through with a factor of 0 and delivered zero lock exactly
  when maximum lock was wanted. It now clamps to the highest learned engagement
  instead.
- **Non-monotonic tune axes rejected on upload.** The expert-map interpolation
  assumes strictly-ascending speed and throttle axes; an out-of-order axis
  silently mis-interpolated. The tune-upload handler now rejects a tune whose
  speed or throttle axis is not strictly ascending.
- **No-learn-table fallback correction factor corrected.** Without a valid learn
  table the Gen5 fallback scaled the requested lock with `(target / 2) + 20`,
  which over-delivered (a 40% request produced a ~40% correction factor instead
  of 30%). The formula now matches its own comment and the Gen4.1 branch,
  `(target + 20) / 2`, so a device with no learn data tracks the request instead
  of running high. Devices with a valid learn table were never on this path.
- **Learn sampling hardened against duty-loop overshoot.** Near lock-up the
  Haldex ECU briefly commands full PWM (an audible pump oscillation above ~60%
  engagement), so a single engagement read at the end of a learn step could
  decode to a clean 100 and, written verbatim, poison the learn table. Each
  correction-factor step now samples engagement across a short window and takes a
  median with a monotonic clamp against the previous step, rejecting a lone spike
  or CAN dropout without changing total learn duration.

### Concurrency and persistence

- **Shared control state guarded with a mutex.** Cross-task globals (drive state,
  lock target, and the tables the standalone generators read) were unguarded. A
  single non-recursive `stateMutex` is created before any task spawns and the
  read/write sites are guarded, so the CAN hot paths can't read a half-updated
  value.
- **Settings consolidated onto one NVS namespace.** `readEEP` called
  `pref.begin()` with a setting name as the namespace per call, so only the
  last-opened namespace persisted and first-run seeding never ran. It is
  consolidated onto a single `openhaldex` namespace with a first-run seeded
  sentinel and a one-time migration from the prior de-facto namespace.

### Changed

- **Lock response tuning re-united from %/s to milliseconds.** The attack and
  release sliders were percent-per-second, so a "5 second decay" was buried in an
  awkward unit (5 %/s) and fine control was impossible - near the fast end even a
  10 ms change needed thousands of %/s of slider resolution. They are now
  milliseconds for a full 0-100% travel, `0-1000 ms` in clean 10 ms steps, and
  `0 = instant` in each direction. Making release instant also removes a footgun:
  under the old unit a release rate of 0 meant "never releases" (clutch stuck
  locked), which is why release was floored at a nonzero rate; in milliseconds 0
  cleanly means "snap open." Persisted `%/s` settings on existing devices are
  migrated once to the equivalent time (old 120 %/s release default becomes
  833 ms) and re-saved under the new keys.
- **Dashboard layout consolidation.** The five tabs were 21 stacked single-topic
  cards; related controls are now grouped into 13, cutting vertical scrolling
  by roughly 40% with no signal removed. The Haldex state readout folds into the
  glance card footer; the Lock Response trace sits as its own card directly below
  the glance tiles so the temps/speed/throttle tiles stay top-of-screen on a
  phone; the three lock-disable cutoffs (under-speed, above-speed, min-throttle) become
  one "Lock Disable Conditions" card with a per-row icon and a live hint that
  warns when the two speed cutoffs overlap into a dead band; the CAN status,
  signals and brake/handbrake cards merge into one "CAN" card; the two WiFi
  cards into one "WiFi Access Point"; and the Settings tab folds the generation
  selector into the Learn card and the output toggles into "Inputs & Triggers".
  DOM ids are unchanged, so load/save and the API payloads are untouched.
- **Dashboard hero relabelled "Target duty" -> "Target lock".** The big hero
  number reads `lockTarget`, which the firmware derives from the drive mode (50:50
  requests 100, 60:40 requests 40, and so on) - it is the lock percentage the unit
  is requesting, not a coil duty cycle. The old "Target duty" label wrongly implied
  it was the solenoid PWM the Haldex module applies, which is a separate quantity
  the module chooses itself. The label now matches what the value actually is.
- **Dashboard poll rate no longer pinned to 1Hz.** The live poll used a fixed
  500ms `setInterval` with a one-in-flight guard; when a response landed just after
  a tick had already fired and bailed, the next tick was a full interval away,
  collapsing the effective rate from a nominal 2Hz to about 1Hz. The poll is now a
  self-scheduling loop that fires the next request as soon as the previous one
  settles (150ms floor), so the update rate tracks the device's real response
  latency instead of being quantised to a fixed tick. A generation token stops the
  loop while the page is hidden so backgrounding can't leave two loops running.

### Fixed

- **Haldex temperatures could read a fake 0 before the first sample.** The UDS
  clutch and cooling-fin temperatures (decoded and raw) are zero until the module
  first answers, and were published to `/api` as `0` - indistinguishable from a
  real 0 degC reading, which corrupted the raw-byte capture used to solve the
  temperature scale. Each field now publishes as `null` until its DID has been
  decoded at least once in the session, and reverts to `null` when the session
  ends.
- **A learned lock table could be applied to the wrong CAN frame.** A learning run
  always runs under the DBC-correct BPK packing, so the table it builds is calibrated
  against BPK frames. But when driving with Fix Hunting off the firmware sent the
  legacy V3 frame while still applying that BPK-measured table - a mismatch that
  made lock behave differently than it did during the Learn (worst case, stuck at
  full lock). The frame selector now stays on BPK whenever a valid learn table
  exists, regardless of the Fix Hunting switch, so what you calibrated is what you
  drive. Users who have never done a learning run are unaffected and keep the V3 default.
  The standalone CAN path now shares the same selector, so it can no longer drift
  from the passthrough path.
- **Map slots read "Empty slot" on Load, and you couldn't choose which slot to
  save into.** Two problems in the on-device tune slots. First, the saved-tune
  list and the Load action decided "is this slot used?" with two different NVS
  calls - the list checked the stored blob's reported length, Load actually read
  the blob back - and those two calls can disagree, so a slot that loads fine
  could still be listed as empty (and vice versa). The list now reads the blob
  the exact same way Load does, so the two can never disagree. Second, empty
  slots were greyed out in the dropdown, so there was no way to pick a specific
  slot to save into - "Save As" just took the first free one. Empty slots are now
  selectable and each option shows its slot number, so you can save straight to a
  chosen slot (and re-save reclaims any slot that had been left unreadable).
- **Accidental toggle/slider changes while scrolling.** On the phone a finger that
  landed on a slider while flicking the page would drag the value, and a scroll
  that started or ended on a toggle would flip the switch - so Standalone, Disable
  Controller and the sliders changed by accident. Sliders now let a vertical scroll
  pass straight through (only a deliberate sideways drag moves them), and a toggle
  ignores a "tap" whose finger actually moved (a scroll, not a press). Sliders also
  no longer jump their value when you tap the bar - the value only changes when you
  grab the thumb and drag it, so a stray press on the track can't slam a setting to
  wherever your finger landed.
- **Disable Controller, Standalone and Analyzer now confirm before switching on.**
  These three take the unit out of active control, so switching one into its
  disruptive state now asks first - a stray tap can't silently disable the
  controller, go standalone, or enter analyzer mode. Turning them back off is
  unchanged.
- **Drive-mode selection no longer snaps back to the old mode.** The dashboard
  re-applies the device's reported mode every poll so external changes show up, but
  a poll already in flight when you picked a new mode would report the old mode and
  revert the highlight. The UI now holds your selection until the device confirms
  the new mode (or a short timeout), so the pick sticks.
- **Expert multi-select: first tap after a long-press is no longer eaten.** The
  long-press that enters multi-select armed a "swallow the next click" guard to
  absorb the ghost click a touch normally trails. But a long hold makes the
  browser fire `contextmenu` instead of `click`, so the ghost never came and the
  guard stayed armed, eating the user's next deliberate cell tap (the reported
  "first box doesn't highlight" bug). The guard is now armed with a short expiry,
  so a tap arriving after the ghost window registers normally.

### Added

- **Plain-English help across the tuning UI.** The drive-mode drawer now
  describes what each mode actually does (Stock passes the factory engagement
  through, FWD holds the rear open, 50:50/60:40/75:25 hold progressively lighter
  fixed lock targets, Expert follows the throttle/speed map) instead of showing
  bare labels. Added inline hints to the previously unexplained controls: Haldex
  Generation, the brake/handbrake follow/invert outputs, and the Controller and
  Connectivity toggles (Controller Disabled, Standalone Mode, Use CAN if
  Available, Broadcast over CAN). Copy only - no behaviour change.

- **Per-corner slip and drive-mode over the diagnostic channel.** OpenHaldex now
  answers three supplier-specific UDS DIDs on the Haldex address (0x70F request ->
  0x779 response), the same channel that already carries clutch temps/PWM, because
  the car's gateway does not forward the passive 0x6B0/0x6A0 broadcasts to the OBD
  port. `0xFDA0` (lock) returns commanded lock %, applied engagement, and the live
  drive mode. `0xFDA1` (slip) returns geometry-compensated per-corner slip - each
  wheel's actual speed against the speed Ackermann geometry predicts for its turn
  radius at the current steering angle, so a straight launch and a mid-corner
  break-loose both read true slip rather than steering geometry, in [FL,FR,RL,RR]
  order. `0xFDA2` (mode) is a WriteDataByIdentifier target: a one-byte mode write
  sets `state.mode` (validated, mutex-guarded) and replies positive, so a UDS
  tester such as the Rokketek gauge can set the drive mode and confirm it by
  reading `0xFDA0` back. The four corner-slip values are also exposed as
  `cornerSlip` on the web dashboard JSON. TT Mk3 geometry constants are
  compile-time and flagged for on-car calibration. Not present in V8.00.2.

- **Learn Haldex calibration chart.** When a learned calibration table exists the
  Learn section now plots it as a curve - commanded correction factor on X,
  measured Haldex engagement on Y - against a dashed 1:1 reference, so where the
  Haldex over- or under-responds reads at a glance. It renders on page load and
  after a learn completes, and clears when the table is cleared. Render-only from
  the table `/api/learn/status` already returns; no firmware change, no extra
  device load.

- **Steering-gain reduction (off by default).** Scales the lock target down as
  steering angle increases, so the rear axle is not fighting the front through
  tight corners and car parks. The angle is decoded from the MQB LWI_01 (0x086)
  frame; gain stays 100% up to a configurable start angle, ramps linearly to a
  configurable minimum by the full angle, and the reduction is bypassed whenever
  the steering signal is stale or flagged degraded (fails toward stock
  behaviour). Toggle and sliders live in the web UI; the live signed angle and
  applied gain show on the Diagnostics page so the decode can be verified on the
  car before enabling. Not present in V8.00.2.
- **Lock engage rate, and the dead rate-limit settings wired up.** V8.00.2
  rate-limits only the release direction, and its lock-release rate slider and
  enable toggle were POSTed by the web UI but never read by `/api/settings`, so
  they did nothing. This fork accepts and persists both, adds a lock-engage rate
  so rising transitions can be slewed too (0 keeps the instant default), and
  shares one step function between both directions.
- **Web UI restyle and mobile pass.** A dark dashboard theme, a semi-circular
  engagement arc gauge with a target tick, polling that pauses while the page is
  hidden, and general touch and reduced-motion polish. Client-side hardening:
  overlapping status polls are guarded and time-bounded, missing DOM nodes are
  handled, and failed settings fetches no longer throw.
- **Expert tune-map curve view.** The Expert editor now draws the lock surface
  as a line chart below the grid, so the 7x7 table of numbers reads as curves
  while you tune. Two views toggle between lock-vs-speed (one line per throttle
  band) and lock-vs-throttle (one line per speed band), with a colour ramp and
  legend for the bands. It is read-only and redraws as cells or axis values
  change; it does not alter what is written to the Haldex.
- **Connection-status badge.** The header now shows a live/reconnecting/offline
  badge driven by the dashboard poll. A dropped access-point link used to leave
  the last gauge values frozen on screen looking current; the badge flips to
  "Reconnecting…" after the first missed poll and "Offline" after three, so
  stale numbers can't be mistaken for live data while driving. Recovers to
  "Live" on the next good poll.
- **Live lock-response trace.** The dashboard now plots a rolling 15-second
  strip chart of lock target vs actual engagement beneath the gauge. Where the
  gauge shows the instant pair, the trace shows how the actual chased the target
  over time, so coupling lag and the effect of the attack/release rate limits
  read at a glance while tuning. It is fed from the same dashboard poll (no extra
  device load), and the trace clears on a dropped link so a reconnect never
  bridges the outage. Target and actual drop out independently: the dashed target
  line breaks only where no CAN target is present, and a missing engagement
  reading breaks the actual line rather than plotting a phantom drop to zero or
  taking the target's line down with it.

- **On-device saved tune slots.** Five named map slots now persist in device
  NVS with list/get/save/delete endpoints, so a tune saved from one phone is
  visible from any phone. The slot library uses its own NVS namespace and never
  touches the settings handle. Replaces the earlier phone-side file
  export/import.
- **Gen5 BPK full-lock torque is now a setting.** The "Fix Hunting" (BPK)
  Motor_11 packing mapped 100% lock to a hardcoded 220 Nm of spoofed engine
  torque, so a controller that never reached full engagement had no way to ask
  for more. It is now an adjustable "Full-lock torque ceiling" on the Settings
  page, 100-500 Nm (MQB signal max 509 Nm), default 220 so existing behaviour is
  unchanged. The underlying packing math was widened from 8-bit (which silently
  capped torque at 255 Nm) to the full signal range, and the standalone and
  CAN-passthrough copies of the Motor_11 packing were de-duplicated into one
  host-tested function so the two paths can no longer drift.
- **Dashboard wakes for a USB host on the bench.** A USB host on the USB
  Serial/JTAG port now keeps the AP up and wakes a sleeping device, so the
  dashboard is reachable on the bench without a CAN source. Vehicle-powered
  operation has no USB host, so in-car low-power behaviour is unchanged.

### Build and test

- **Host-runnable native test suite added.** A PlatformIO `[env:native]`
  environment with Unity characterization tests pins the pure-function core:
  CRC8/AUTOSAR checksum, expert-map interpolation, lock-response rate limiting,
  steering-gain reduction, and the low-power fps-threshold wake decision. The
  wake decision was extracted into a pure `lpCanActive` seam so the firmware and
  the native test share one source of truth. Run with `pio test -e native`.
- **Release build profile added.** A separate `esp32c6-release` environment that
  forces all debug output off.
- **Board definition added.** The `esp32-c6-mini-1-n4` board file the build
  requires, which the upstream source drop did not include.
- **CI now packages a flashable binary.** The `firmware-build` job builds the
  LittleFS web-UI image and merges the bootloader, partition table, firmware and
  filesystem into one `firmware-merged.bin` (flash at `0x0`) via
  `scripts/merge_firmware.py`, which resolves the flash offsets from the built
  partition table. Every run uploads the individual binaries plus the merged
  image and `SHA256SUMS.txt` as artifacts. A tag push (`v*`) publishes a merged
  flash-from-scratch image and per-partition binaries to a GitHub Release. The
  release job holds `contents: write` scoped to itself; the rest of CI stays
  read-only.
