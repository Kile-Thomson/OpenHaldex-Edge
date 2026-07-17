<p align="center">
  <a href="https://forbes-automotive.com/?utm_source=github&utm_medium=readme&utm_campaign=openhaldex" target="_blank">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="/Images/FA-logo-white.png">
      <source media="(prefers-color-scheme: light)" srcset="/Images/FA-logo.png">
      <img src="/Images/FA-logo.png" width="250" alt="Forbes Automotive">
    </picture>
  </a>
</p>

# OpenHaldex-C6-Edge

> [!IMPORTANT]
> **This is a personal, non-commercial firmware fork** of [Forbes Automotive OpenHaldex-C6](https://github.com/Forbes-Automotive/OpenHaldex-C6). It exists for three reasons:
>
> 1. **Firmware stability:** fix confirmed bugs before adding anything new.
> 2. **Security hardening:** put the WiFi access point behind a password you set on the device, and gate CAN injection behind it.
> 3. **A host-runnable test suite:** pin wire-byte correctness so changes can't silently break CAN behaviour.
>
> **This repo contains firmware source only.** Hardware design files, Gerbers, BOM, enclosure STLs, and pre-built binaries all live in the upstream project. If you want supported hardware or a ready-to-flash binary, go to [Forbes Automotive OpenHaldex-C6](https://github.com/Forbes-Automotive/OpenHaldex-C6) and [forbes-automotive.com](https://forbes-automotive.com/).
>
> All original design, reverse-engineering, and credit belong to **Forbes Automotive** and the upstream contributors listed under [Acknowledgements](#acknowledgements). The Forbes Automotive copyright and the [LICENSE](LICENSE.md) (FASL v1.0) are preserved unchanged. This fork is not affiliated with or endorsed by Forbes Automotive. See [Upstream](#upstream) for tracking the original and [Improvements over upstream](#improvements-over-upstream) for what has changed here.

<p align="center">

![Platform](https://img.shields.io/badge/platform-ESP32--C6-blue)
![Hardware](https://img.shields.io/badge/hardware-Haldex%20Gen1%20%7C%20Gen2%20%7C%20Gen4%20%7C%20Gen5-green)
![Last commit](https://img.shields.io/github/last-commit/Kile-Thomson/OpenHaldex-C6-Edge)

</p>

OpenHaldex is an open-source **Haldex AWD controller** for Volkswagen and Audi Group vehicles using Haldex Generation 1, 2, 4 (PQ Chassis) and 5 (MQB) differentials.

The firmware runs on an **ESP32-C6** and reads CAN bus messages from the vehicle, allowing the controller to modify or generate commands so the Haldex differential behaves exactly as you've configured. It can operate using OEM CAN signals or in Standalone mode, which is useful for conversions where no chassis bus is present.

![OpenHaldex-C6](/Images/openHaldexUI.png)

## Improvements over upstream

This fork tracks Forbes Automotive's **V8.00.2** firmware and builds directly on it. Same hardware, same modes, same wiring. Most of the driving features - the MQB live-data readout over UDS, hazard force-open, lock-release rate limiting and the low-power sleep system - are Forbes's own work and ship in the V8.00.2 base this fork starts from. Full credit for the platform and its reverse-engineering belongs to them.

What this fork adds on top is a focused security, correctness and testing pass, a handful of optional driving features, over-the-air updates from the browser, and a reworked web UI. Every item below was checked against the V8.00.2 source before it was listed here, so the list reflects what actually differs from the current upstream, not an older release. The change-by-change breakdown is in [CHANGELOG.md](CHANGELOG.md).

### Security hardening

- **No default password; you set your own on the device.** Nothing is committed in source and there is no build step. On first power-up the access point runs open just long enough to reach the setup page, where you choose a WPA2 password (8-63 characters).
- **Joining the WiFi AP is the single gate.** Once the password is set, being on the AP is the one auth boundary: anyone on the network has full control, anyone who cannot join has none. There is no separate HTTP login to manage.
- **CAN injection stays closed until the AP is secured.** Host-to-device CAN injection over the analyzer port is refused while the AP is still open, so the car's behaviour cannot change until the network itself is locked. Passive sniffing (receive only) always works.

### Correctness fixes

- **Recovers from a CAN fault on its own.** After a bus-off event the controller detects the recovery and brings each bus back by itself, no power cycle needed.
- **Corrected Gen5 checksum.** The Gen5 ESP_10 (0x116) frame now uses the correct E2E checksum DataID. (Still to be confirmed against a real Gen5 unit on the bench.)
- **Diagnostic reads return real data.** The built-in ECU read returns the full response rather than an empty buffer.

### Robustness under the hood

- **Won't trip over itself under load.** Data that the CAN tasks read and write concurrently is now guarded by a mutex.
- **Settings survive a reboot correctly.** Storage is consolidated onto one flash namespace, with a one-time migration from the old layout, so every setting persists and first-run defaults seed correctly.

### Added driving features

- **Optional steering-gain reduction.** Off by default. Winds lock back as steering angle increases, so the rear axle isn't fighting the front through tight corners and car parks. The angle is decoded from the MQB LWI_01 (0x086) frame; you choose where the reduction starts, where it bottoms out, and the minimum it can drop to. If the steering signal goes stale or is flagged faulty the reduction switches itself off and behaviour returns to normal. The live angle and applied gain show on the Diagnostics page so you can confirm the reading on your car before turning it on.
- **Lock-rate sliders wired through, in real time units.** The lock-release rate slider and enable toggle are honoured end to end, and a separate engage rate is added, so both lock-up and release can be slewed instead of snapping on and off. Both are set in milliseconds for a full 0-100% sweep (0 = instant), so a rate reads as the time the change takes.
- **Per-corner slip and drive-mode over the diagnostic bus.** OpenHaldex answers three supplier-specific UDS DIDs on the Haldex address, the same channel that already carries clutch temps and PWM, so a tester on the OBD port (such as the Rokketek gauge) can read lock and geometry-compensated per-corner wheel slip and set the drive mode, even though the car's gateway won't forward the passive broadcasts that far. Slip is each wheel's real speed against what the steering geometry predicts, so a straight launch and a mid-corner break-loose both read as true slip. The four slip values are also on the web dashboard JSON. Geometry constants are Audi TT Mk3 defaults and want on-car calibration.
- **Per-car lock calibration for Gen5.** The BPK packing calibration value is now adjustable per car, so a car whose Haldex under- or over-responds to the lock command can be dialled in to match. Run a Learn and nudge it until the lock you command matches what the Haldex actually delivers (the Sent vs Returned line sitting on the 1:1 diagonal). It is not a strength dial - higher does not lock harder, it just shifts the calibration, and there is one correct value per car. Default is unchanged, so nothing moves unless you tune it. Under the hood the packing math was widened past its old 8-bit cap, and the standalone and passthrough copies were merged into one host-tested function so they can't drift apart.

### Updates

- **Update over the air, one file, from the browser.** A Software Update card on the Settings page shows the current version, the live safe-state gate and its blocking reason, and a single upload slot with progress. Feed it the release's merged image - the same file used for a USB flash - and it updates both the firmware and the web UI in one go, splitting the image by flash offset and skipping the bootloader, partition table and NVS so your settings and learn table survive. This fork builds a UI on top of the existing OTA endpoints and extends over-the-air updates to cover the web UI itself. Every update stays behind the same safety gate (car stationary, buses healthy, no temperature fault) and the firmware path keeps dual-slot auto-rollback.

### Web interface

- **Restyled dashboard.** A dark dashboard with a semi-circular engagement gauge and a target tick, touch and reduced-motion polish, and polling that pauses while the page is hidden so it isn't hitting the device in your pocket.
- **Live lock-response trace.** A rolling 15-second strip chart under the gauge plots what you asked the lock to do against what it actually did, so coupling lag and the effect of the rate limits read at a glance while tuning. It clears on a dropped link, so a reconnect never draws a line across the outage.
- **Connection-status badge.** A live/reconnecting/offline badge in the header. A dropped access-point link used to leave the last gauge values frozen on screen looking current; the badge now flips to reconnecting after the first missed poll and offline after three, so stale numbers can't be mistaken for live data while driving.
- **Expert map curve view.** The Expert editor draws the lock surface as curves below the 7x7 grid - lock against speed, or lock against throttle - so the table of numbers reads as shapes while you tune. It is read-only and doesn't change what's written to the Haldex.
- **Learn calibration chart.** Once your Haldex has a learned table, the Learn section plots it - commanded correction factor against measured engagement, with a 1:1 reference line - so where your unit over- or under-responds reads at a glance. Render-only from data the device already returns; no extra load on the module.
- **Install it like an app, full-screen.** The web UI is an installable PWA (manifest, icon set, service worker) so it can go on a phone home screen and open full-screen. A full-screen toggle in the header also works over plain http with no install step, for a clean dashboard on an unmodified phone. Live telemetry and control always hit the device - the service worker never caches the API or POSTs.
- **On-device saved tune slots.** Five named map slots persist in the device's own storage with list/save/load/delete, so a tune saved from one phone is visible from any phone. Replaces the earlier phone-side file export/import.
- **Plain-English help throughout.** The drive-mode drawer describes what each mode actually does, and the previously bare controls (Haldex generation, brake/handbrake follow, the controller and connectivity toggles) now carry inline hints. Copy only - no behaviour change.

### Built and tested

- **A test suite that runs on any computer.** A host-runnable native test suite pins the core maths and safety logic - checksums, expert-map interpolation, lock-response rate limiting and steering-gain reduction - so a change can't quietly alter what the Haldex sees, no hardware needed.
- **Release build profile.** A separate build with debug output disabled.

Origin attribution and the FASL v1.0 license are preserved unchanged.

---

## Contents

- [Supported platforms](#supported-platforms)
- [Modes](#modes)
- [Expert mode](#expert-mode)
- [Haldex learning](#haldex-learning)
- [Changing modes](#changing-modes)
- [Mode numbers](#mode-numbers-can-byte-0)
- [Broadcasted state](#broadcasted-state)
- [WiFi setup](#wi-fi-setup)
- [Low power mode](#low-power-mode)
- [Flashing firmware](#flashing-firmware)
- [CAN sniffing](#can-sniffing-savvycan--gvret)
- [Hardware](#hardware)
- [Acknowledgements](#acknowledgements)
- [Upstream](#upstream)
- [Disclaimer](#disclaimer)

---

## Supported platforms

- Generation 1 - PQ
- Generation 2 - PQ
- Generation 4 - PQ
- Generation 4 - GM
- Generation 4 - Ford (ongoing)
- Generation 5 - MQB (0CQ)
- Generation 5 - PQ (0AY)

> Gen3 is currently unsupported.

---

## Modes

| Mode | Behaviour | LED colour |
|------|-----------|------------|
| Stock | OEM behaviour | Red |
| FWD | Zero lock | Green |
| 7525 | 30% lock | Cyan |
| 6040 | 40% lock | Neon pink |
| 5050 | 100% lock | Blue |
| Expert | User-defined lock profile | White |

---

## Expert mode

Expert mode allows lock targets to be configured based on **speed and throttle setpoints** using a table inside the Web UI. Full control over your Haldex system with no guesswork. Requires OEM CAN messages to be present for throttle and speed inputs.

![ExpertMode](/Images/expertmode.jpg)

*Expert mode grid configuration interface within the OpenHaldex C6 UI.*

Below the grid, a read-only curve view plots the same lock surface so the table reads as lines while you tune. Toggle between **lock vs speed** (one line per throttle band) and **lock vs throttle** (one line per speed band); it redraws as you edit and never changes what is sent to the Haldex.

---

## Haldex learning

The controller can learn the actual engagement curve of your specific Haldex unit by cycling through all available lock percentages. Use the **Learn Haldex** option in the Settings page. Within one minute the controller will know exactly how to hit the lock percentage you request. Once a table is learned it is plotted as a curve - commanded correction factor against measured engagement, with a 1:1 reference line - so you can see where your Haldex over- or under-responds.

### Gen5 'Fix hunting' (planned, not yet built)

> [!NOTE]
> Not implemented in this fork yet. Certain Generation 5 controllers, specifically those running the **554K** variant, use a different torque model to calculate the lock request, so the standard learning process does not produce accurate results on them. A dedicated **Fix Hunt** calibration path is planned to handle this. It needs a 554K unit to characterise how it responds to the sweep before the corrected algorithm can be written. Tracked upstream at [Forbes-Automotive/OpenHaldex-C6#37](https://github.com/Forbes-Automotive/OpenHaldex-C6/issues/37).

---

## Changing modes

**On-board button**

Press the `Mode` button to cycle through modes. Long-press disconnects WiFi.

**External button**

Long-press the external button to engage force mode; release to exit it.

**WiFi**

Use the Web UI at `192.168.1.1` or `openhaldex.local`.

**CAN**

Send a CAN message containing the mode number in Byte 0 (see [Mode numbers](#mode-numbers-can-byte-0)).

**CAN - Force mode (TC / ESP button, hazard switch)**

Force mode can be triggered from CAN signals already present on the bus, with no extra wiring:

- **TC / ESP button** - the parser reads the ESP/ASR-disabled bit (byte 7 on PQ, byte 6 on MQB). Pressing the traction control button to disable ESP triggers force mode.
- **Hazard switch (MQB)** - while the hazards flash, the coupling is forced fully open (FWD), overriding every other mode. Decoded from Blinkmodi_02 (`0x366`, byte 2 bit 4), confirmed live on-car and against the upstream V8.00.2 firmware. Useful for towing, recovery or limping home without the rear axle binding. The upstream request is [Forbes-Automotive/OpenHaldex-C6#36](https://github.com/Forbes-Automotive/OpenHaldex-C6/issues/36).

Both are optional and enabled in the Settings page.

---

## Mode numbers (CAN Byte 0)

```
Stock  = 0
FWD    = 1
5050   = 2
6040   = 3
7525   = 4
Expert = 5
```

---

## Broadcasted state

The module broadcasts its current state on the CAN bus (default ID `0x6B0`). This can be read by aftermarket ECUs or FIS displays.

> [!NOTE]
> Broadcasting can conflict with other devices on the same ID. The CAN ID can be adjusted in code if required.

```
data[0] = reserved (always 0)
data[1] = standalone_flag (1 when standalone mode is active)
data[2] = actual_engagement (raw engagement byte returned by the differential)
data[3] = lock_target_percent (lock requested by the firmware, 0-100)
data[4] = vehicle_speed
data[5] = mode_override_flag
data[6] = current_mode_number
data[7] = pedal_value
```

---

## WiFi setup

1. Connect to the access point **OpenHaldex-C6**.
2. Open a browser and navigate to `192.168.1.1` or `openhaldex.local`.

> [!NOTE]
> **First power-up:** on a fresh or factory-reset device, step 2 redirects to `/setup` instead of the main UI. Set a password there (8-63 characters). Once submitted the setup page closes permanently and the main UI loads. See [Setting your password](#setting-your-password-first-connection) below.

<p align="center">
  <img src="/Images/UIDemo.png" alt="OpenHaldex C6 Web UI" width="900" style="max-width:100%;">
</p>

### Setting your password (first connection)

There is no default password and no build step - you do not compile or flash anything to set one.

On a fresh or factory-reset device, connect to the access point, open `192.168.1.1`, and the device sends you straight to a short setup page. Enter a password and confirm it (8-63 characters). That becomes the **WiFi AP (WPA2) password** - the single auth boundary: anyone who can join the AP can use the UI and flash updates, anyone who cannot, cannot. The AP restarts secured and the main UI loads.

Until you set it, the AP runs open so you can reach the setup page - and while it is open, host-to-device CAN injection over the analyzer port is refused and the dashboard keeps redirecting to setup.

**Changing it later.** Rotate the password from the WiFi Access Point card on the Diagnostics tab (or `POST /api/wifi`). Long-pressing the mode button resets the AP back to open.

**The WiFi access point is open by default** so you can reach the setup page on first connection. Securing the AP behind a password you set, and refusing CAN injection until it is secured, are what keep the CAN bus safe - passive sniffing is harmless, and nothing can change the car's behaviour until the network itself is locked.

---

## Low power mode

Low Power Mode ships in this fork and is **enabled by default**. It is three layers of power saving, each building on the previous, all configured from the **Settings** page:

- **Layer 1 - Idle AP shutdown** is always active (no toggle).
- **Layer 2 - CAN Sleep** is **on by default** (`canSleepEnabled = true`).
- **Layer 3 - CAN Sleep (Aggressive)** is **opt-in / off by default** (`canSleepAggressive = false`).

The controller is designed to live on a **permanent +12 V** feed. With Low Power Mode active it draws approximately:

| State | Current |
|-------|---------|
| Sleeping (car off, WiFi off, CAN quiet) | ~14 mA |
| Awake (CAN activity detected or WiFi client connected) | ~50 mA |

> [!WARNING]
> Estimated, inherited from upstream — **not yet measured on this fork.** These current-draw figures are order-of-magnitude expectations only, pending a bench-meter measurement (see the bench-pending note below).

### Layer 1 - Idle AP shutdown (always active)

After **5 minutes** with no WiFi clients connected and no CAN activity, the controller automatically shuts down the WiFi AP and turns off the LED. WiFi is restored automatically as soon as CAN traffic resumes.

### Layer 2 - CAN sleep (enabled by default)

Controlled by the **CAN Sleep** toggle in Settings and **on out of the box**. The ESP32-C6 CPU enters light sleep when idle; the TWAI peripheral powers down but preserves its receive queue; the first incoming CAN frame wakes the controller immediately.

### Layer 3 - CAN sleep aggressive (opt-in, builds on Layer 2)

Off by default — enable **CAN Sleep (Aggressive)** in Settings to turn it on. The CAN transceiver chips shut down completely, CPU minimum clock drops to 10 MHz, and WiFi AP transmit power is trimmed. Wake is interrupt-driven from a GPIO ISR on each CAN_RX line.

### Setting it up

Low Power Mode works out of the box, but the wake threshold is calibratable because every car idles its CAN bus at a different rate.

The **LP Wake Threshold (fps)** defaults to **1100 fps**. In OEM installs the module stays awake while the **Chassis fps** rate is at or above the threshold and sleeps when it drops below it — so the default keeps the module awake while the vehicle is actively driving the chassis bus (typically well above 1100 fps) and lets it sleep once the bus goes quiet.

1. Park and lock the car. Wait 30 minutes or until the Chassis bus goes fully quiet.
2. Stay connected to the OpenHaldex WiFi AP while you check (the controller stays awake while a client is connected).
3. Open the Web UI and watch the **Chassis fps** and **Haldex fps** counters in Settings.
4. Set **LP Wake Threshold (fps)** above the parked-bus reading and below the driving-bus reading — the default of 1100 fps suits most installs; lower it only if your car's active chassis-bus rate sits below 1100 fps.
5. **CAN Sleep** is already enabled; optionally enable **CAN Sleep (Aggressive)** in Settings.
6. Disconnect from the WiFi AP.

> [!NOTE]
> **Standalone mode:** with no chassis bus, the module wakes on a fixed **50 fps** Haldex-bus threshold. This is independent of the LP Wake Threshold slider, which only governs the chassis-bus decision in OEM installs.

> [!NOTE]
> **Switched-ignition installs:** if the module is already powered off with the ignition, Low Power Mode saves little and is optional.

> [!NOTE]
> **Bench-pending on this fork:** the following hardware-only behaviours are inherited from upstream and have **not yet been measured on real metal** for this fork — the sleeping/awake current draw, the wake latency, transceiver standby in Layer 3, and the exact standalone-threshold value. They are flagged pending a bench-rig measurement increment.

---

## Flashing firmware

You can either flash a pre-built release binary or build from source with PlatformIO.

### Pre-built release binary

Each tagged release on this fork's [Releases page](https://github.com/Kile-Thomson/OpenHaldex-C6-Edge/releases) attaches a single merged image, `openhaldex-c6-<tag>-merged.bin`. It bundles the bootloader, partition table, firmware and the LittleFS web UI, so it is a complete flash-from-scratch build - no separate filesystem upload needed. Flash it at offset `0x0`:

```sh
esptool.py --chip esp32c6 write_flash 0x0 openhaldex-c6-<tag>-merged.bin
```

Or drag it into a browser flasher such as [ESP Web Tools](https://web.esptool.js.org/). `SHA256SUMS.txt` is attached so you can verify the download. The separate `app` and `littlefs` binaries are also attached for partial or OTA updates.

> These binaries are a non-commercial fork build, distributed free of charge under the [FASL v1.0](LICENSE.md); the changes over upstream V8.00.2 are in [CHANGELOG.md](CHANGELOG.md). For official, supported firmware use the upstream project - see [Forbes Automotive](https://forbes-automotive.com/pages/module-software-updater). The upstream binary is the official, supported build; this fork's build adds the changes listed in the CHANGELOG.

### Build and flash via USB

```sh
pio run -e esp32c6 --target upload
```

Connect the controller using a **data-capable USB-C cable**. Power-only cables will not work.

To reproduce the release's single merged image locally:

```sh
pio run -e esp32c6-release
pio run -e esp32c6-release -t buildfs
python scripts/merge_firmware.py --build-dir .pio/build/esp32c6-release
esptool.py --chip esp32c6 write_flash 0x0 .pio/build/esp32c6-release/firmware-merged.bin
```

**Build a release image (debug output disabled):**

```sh
pio run -e esp32c6-release
```

**OTA update:**

The easiest path is the **Software Update** card on the Settings tab of the web
UI: it shows the current version and safe-state, and has a single upload slot
with progress. Give it the release's merged image (the same single file used
for USB flashing) and it updates the firmware and the web UI in one go - the
device splits the image by flash offset and skips the bootloader, partition
table and NVS regions, so settings and the learn table are kept. A bare
`firmware.bin` or `littlefs.bin` works too when only one half changed. Access
is gated by the WiFi AP password - anyone on the AP can update; there is no
separate HTTP login.

From the command line, the device accepts an HTTP upload to `/ota/update`
(merged image, firmware, or LittleFS image - it classifies the file from its
first bytes) or `/ota/updatefs` (explicitly the web-UI LittleFS image). It
does not speak the espota protocol, so PlatformIO's `--upload-port <ip>` will
not work:

```sh
# everything in one file
curl -F "update=@.pio/build/esp32c6-release/firmware-merged.bin" \
  http://192.168.1.1/ota/update

# or just one half
pio run -e esp32c6
curl -F "update=@.pio/build/esp32c6/firmware.bin" \
  http://192.168.1.1/ota/update

pio run -e esp32c6 -t buildfs
curl -F "filesystem=@.pio/build/esp32c6/littlefs.bin" \
  http://192.168.1.1/ota/updatefs
```

Any update is refused unless the safety checks pass (vehicle stationary,
buses healthy, no Haldex temperature fault); check `GET /ota/check` first. A
filesystem or merged update briefly takes the web UI offline and reboots the
module when it completes; if a filesystem upload fails partway the firmware is
untouched, so recovery is just re-uploading the image.

For a ready-to-flash binary of this fork, see [Pre-built release binary](#pre-built-release-binary) above. For official, supported firmware, use the upstream build from [Forbes Automotive](https://forbes-automotive.com/pages/module-software-updater); this fork's build adds the changes listed in the [CHANGELOG](CHANGELOG.md).

---

## CAN sniffing (SavvyCAN / GVRET)

Enable **Analyzer Mode** in the Settings page to capture CAN frames.

> [!WARNING]
> Enabling Analyzer Mode disables active Haldex control and returns the device to OEM pass-through behaviour.

> [!NOTE]
> In this fork, host-to-device CAN injection via the analyzer port (GVRET transmit) is refused until the WiFi AP password has been set - the secured AP is the single auth boundary. Passive sniffing (receive only) is unaffected.

The analyzer speaks the GVRET protocol for SavvyCAN, over the WiFi AP (TCP
port 23) or over the USB serial port. Pick the transport with the **SavvyCAN
via WiFi** and **SavvyCAN via Serial** toggles in the Settings page; both buses
are captured.

### SavvyCAN (GVRET)

1. Connect to the OpenHaldex WiFi AP.
2. In Settings, enable **SavvyCAN via WiFi**.
3. In SavvyCAN: **Connection > Add New Device Connection > Network Connection (GVRET)**
4. IP: `192.168.1.1`, Port: `23`, speed: `500000`

---

## Hardware

This fork is firmware source only. Hardware design files, Gerbers, BOM, enclosure STLs, and pre-built binaries live in the upstream project:

**[Forbes-Automotive/OpenHaldex-C6](https://github.com/Forbes-Automotive/OpenHaldex-C6)**

Assembled modules are available from Forbes Automotive:

**[OpenHaldex C6 Controller - Forbes Automotive](https://forbes-automotive.com/products/openhaldex-controller?utm_source=github&utm_medium=readme&utm_campaign=openhaldex)**

For physical installation instructions, wiring diagrams, and connector pinouts, see the upstream README and the [OpenHaldex Installation Guide](https://openhaldex.com/docs/OpenHaldex_Installation_Guide.pdf).

---

## Acknowledgements

- **Forbes Automotive** - Lead development of the OpenHaldex C6 platform, including reverse-engineering and open-source implementation for Gen2, Gen4 and Gen5 Haldex systems, along with ongoing maintenance of the project
- **A Banging Donk** - [Original OpenHaldex project](https://github.com/ABangingDonk/OpenHaldexT4) for Gen1 vehicles
- **Arwid Vasilev** - PCB redesign (V1.02)
- **LVT Technologies** - OTA update integration (now deprecated, but still appreciated)

---

## Upstream

This fork tracks **[Forbes-Automotive/OpenHaldex-C6](https://github.com/Forbes-Automotive/OpenHaldex-C6)** as its upstream. To pull source changes from the original project:

```bash
git remote add upstream https://github.com/Forbes-Automotive/OpenHaldex-C6.git
git fetch upstream
git merge upstream/main
```

For official, supported hardware and firmware go to the upstream project and [Forbes Automotive](https://forbes-automotive.com/). This fork is unofficial and experimental.

---

## Disclaimer

> [!CAUTION]
> This device modifies Haldex behaviour and should only be used **off-road or on a closed course**.
>
> The unit may behave unpredictably and could increase drivetrain wear.
>
> **Use at your own risk.** Forbes Automotive is not responsible for damages resulting from the use of this device or software.
