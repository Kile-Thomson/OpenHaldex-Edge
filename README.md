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
> 2. **Security hardening:** close unauthenticated surfaces and remove committed credentials.
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

What this fork adds on top is a focused security, correctness and testing pass, plus two optional driving features. Every item below was checked against the V8.00.2 source before it was listed here, so the list reflects what actually differs from the current upstream, not an older release. The change-by-change breakdown is in [CHANGELOG.md](CHANGELOG.md).

### Security hardening

- **No committed default password.** The base firmware carries a flashing password in its source (and prints it to the serial log). This fork removes it: on first power-up you set your own password on the device, and until you do, over-the-air flashing and every setting-changing action are refused outright.
- **Authenticated control surface.** The state-changing endpoints (mode, settings, tune, learn) and host-to-device CAN injection on the analyzer port now require that password. Passive sniffing still works freely; the web app sends the password for you and tells you clearly if it is wrong instead of silently failing to apply a change.

### Correctness fixes

- **Recovers from a CAN fault on its own.** A bus-off event left the controller stopped until you pulled power - the base firmware's bus-health check only ran under a debug flag and had no recovery path. It now detects the recovery and brings each bus back by itself, no restart needed.
- **Corrected Gen5 checksum.** The Gen5 ESP_10 (0x116) frame carried the wrong E2E checksum DataID - a verbatim copy of another frame's table. The correct value is now used. (Still to be confirmed against a real Gen5 unit on the bench.)
- **Diagnostic reads return real data.** The built-in ECU read returned empty every time because of a response-buffer bug; it now returns the actual response.

### Robustness under the hood

- **Won't trip over itself under load.** Data that the CAN tasks read and write concurrently is now guarded by a mutex.
- **Settings survive a reboot correctly.** Storage is consolidated onto one flash namespace (with a one-time migration from the old layout), fixing a case where only the last-written setting persisted and first-run seeding was dead.

### Added driving features

- **Optional steering-gain reduction.** Off by default. Winds lock back as steering angle increases, so the rear axle isn't fighting the front through tight corners and car parks. The angle is decoded from the MQB LWI_01 (0x086) frame; you choose where the reduction starts, where it bottoms out, and the minimum it can drop to. If the steering signal goes stale or is flagged faulty the reduction switches itself off and behaviour returns to normal. The live angle and applied gain show on the Diagnostics page so you can confirm the reading on your car before turning it on.
- **Working lock-rate sliders, plus an engage rate.** The base firmware's lock-release rate slider and enable toggle were sent by the web UI but ignored by the firmware, so they did nothing. This fork makes them work and adds a separate engage rate, so both lock-up and release can be slewed instead of snapping on and off.

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

---

## Haldex learning

The controller can learn the actual engagement curve of your specific Haldex unit by cycling through all available lock percentages. Use the **Learn Haldex** option in the Settings page. Within one minute the controller will know exactly how to hit the lock percentage you request.

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

On a fresh or factory-reset device, connect to the access point, open `192.168.1.1`, and the device sends you straight to a short setup page. Enter a password and confirm it (8-63 characters). That password is stored on the device and from then on is required for firmware updates and every setting-changing action. The setup page closes for good and the main UI loads.

Until you set it, flashing and all state-changing calls are **refused** (HTTP 503) - the device never runs with an empty or default credential.

**Changing it later.** Once set, rotate the password with an authenticated `POST /ota/credential`. The setup page does not reopen.

**The WiFi access point is open by default** so you can reach the setup page on first connection. Removing the committed password and requiring a login for every control action are what keep the CAN bus safe on an open AP - passive sniffing is harmless, and nothing can change the car's behaviour without the password.

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

> These binaries are a non-commercial fork build, distributed free of charge under the [FASL v1.0](LICENSE.md); the changes over upstream V8.00.2 are in [CHANGELOG.md](CHANGELOG.md). For official, supported firmware use the upstream project - see [Forbes Automotive](https://forbes-automotive.com/pages/module-software-updater). Note the upstream binary does not include the security and correctness fixes in this fork.

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

Once the device has an OTA credential provisioned, you can flash over WiFi. The
device accepts an authenticated HTTP upload to `/ota/update` (it does not speak
the espota protocol, so PlatformIO's `--upload-port <ip>` will not work):

```sh
pio run -e esp32c6
curl -u admin:your-ota-secret \
  -F "firmware=@.pio/build/esp32c6/firmware.bin" \
  http://192.168.1.1/ota/update
```

The update is refused unless the safety checks pass (vehicle stationary, buses
healthy, no Haldex temperature fault); check `GET /ota/check` first.

For a ready-to-flash binary of this fork, see [Pre-built release binary](#pre-built-release-binary) above. For official, supported firmware, use the upstream build from [Forbes Automotive](https://forbes-automotive.com/pages/module-software-updater); note the upstream binary does not include the security and correctness fixes in this fork.

---

## CAN sniffing (SavvyCAN / GVRET)

Enable **Analyzer Mode** in the Settings page to capture CAN frames.

> [!WARNING]
> Enabling Analyzer Mode disables active Haldex control and returns the device to OEM pass-through behaviour.

> [!NOTE]
> In this fork, host-to-device CAN injection via the analyzer port (GVRET transmit, SLCAN transmit) requires an OTA credential to be provisioned. Passive sniffing (receive only) is unaffected.

The analyzer interface is TCP-only (port 23) over the WiFi AP. Two wire
protocols are available via the **Analyzer Protocol** select in Settings:
GVRET for SavvyCAN (both buses) and LAWICEL/SLCAN for CANHacker-style tools
(chassis bus only).

### SavvyCAN (GVRET)

1. Connect to the OpenHaldex WiFi AP.
2. In Settings, enable **Turn on SavvyCAN** and set Analyzer Protocol to GVRET.
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
