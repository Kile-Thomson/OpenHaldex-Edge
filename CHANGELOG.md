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

### Added

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
