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

This fork is a stability-and-security pass over [Forbes-Automotive/OpenHaldex-C6](https://github.com/Forbes-Automotive/OpenHaldex-C6). Same hardware, same modes, same wiring. The differences are all in how the firmware behaves. In plain terms, here is what you get over the stock firmware. The full change-by-change technical breakdown is in [CHANGELOG.md](CHANGELOG.md).

### Safer to leave on the car's network

- **No hidden default password.** The stock firmware shipped with a flashing password baked into the public source code, so anyone could read it and push firmware to your module. This fork removes it. On first power-up you set your own password, and until you do, over-the-air flashing and every setting-changing action are refused outright.
- **Strangers can't poke your CAN bus.** Two ways an unknown device on the WiFi could previously send messages onto the car's bus (the diagnostics read tool and the SavvyCAN/analyzer port) are now closed unless you are logged in. Passive sniffing, just watching traffic, still works freely.
- **The web app actually logs in.** Once you set a password, the on-device controls (mode, settings, tune, learn) send it automatically and tell you clearly if it is wrong, instead of silently failing to apply your changes.

### More reliable in the car

- **Recovers from a CAN fault on its own.** A bus error used to leave the controller dead until you pulled power. It now detects the recovery and brings each bus back by itself, no restart needed.
- **Buttons do what they say.** The onboard and external button long-press actions were swapped in the stock firmware. Onboard long-press now resets WiFi; the external button force-mode and its release work as labelled.
- **`openhaldex.local` keeps working after a WiFi reset.** Previously the friendly hostname stopped resolving after the button WiFi reset until you power-cycled.
- **Settings that fail to save now tell you**, instead of quietly ceasing to persist.

### The AWD behaves the way the settings read

- **"Disengage under speed" finally works.** This setting did nothing on the stock firmware; it was both backwards and switched off by default. It now correctly drops lock below your chosen speed (for car parks and low-speed manoeuvring) and re-engages above it. Force-mode launches from the button still lock regardless, so a standstill launch is unaffected.
- **Lock values stay in range.** A scaling bug could turn an out-of-range reading into a near-zero or over-100% lock request. Values are now capped to a sane range before they reach the Haldex.
- **Ask for full lock, get full lock.** When you requested more lock than the unit had learned, it used to fall back to zero. It now gives the highest useful value it knows.
- **Corrected Gen5 checksum.** A copy-paste error meant the Gen5 ESP_10 frame carried the wrong safety checksum. The correct value is now used. (Still to be confirmed against a real Gen5 unit on the bench.)
- **Diagnostic reads return real data.** The built-in ECU read tool returned empty every time because of a buffer bug; it now returns the actual response.
- **Bad tune maps are rejected.** The expert map editor and the firmware now refuse out-of-range or out-of-order values instead of silently mis-reading the map.
- **Optional steering-gain reduction.** A new, off-by-default feature that winds lock back as you add steering angle, so the rear axle isn't fighting the front through tight corners and car parks. You choose the angle where the reduction starts, the angle where it bottoms out, and the minimum it can drop to. If the steering signal disappears or is flagged faulty, the reduction switches itself off and behaviour returns to normal. The live steering angle is shown on the Diagnostics page so you can confirm the reading on your car before turning it on.
- **Live Haldex health data (Gen5/MQB, optional).** An off-by-default poller reads the coupling's own diagnostics over UDS - clutch temperature, cooling fin temperature, module temperature, pump current/voltage, PWM duty and supply voltage - and shows them on a Home page card. The request wire format and per-value scaling were recovered from the upstream V8.00.2 firmware binary; the response CAN ID and 16-bit byte order still need confirming against a real car before the absolute numbers are trusted.

### Cleaner under the hood

- **Won't trip over itself under load.** Shared data that the CAN tasks read and write is now properly locked, and settings live in one place that survives power cycles correctly (with a one-time migration from the old layout).
- **You can see dropped frames.** The Diagnostics page now shows a per-bus count of CAN frames that failed to send, so silent frame loss is visible.
- **Sensible defaults out of the box.** A fresh device boots with a valid Haldex generation selected instead of sitting inert until you pick one, and the Gen 3 option that never did anything has been removed.

### Built and tested properly

- **A test suite that runs on any computer.** A host-runnable native test suite pins the core maths and safety logic - Haldex checksums, expert-map interpolation, lock rate limiting, and steering-gain reduction - so a change can't quietly break what the Haldex sees, no hardware needed.

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
- [Low power mode](#low-power-mode-planned-not-yet-built)
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
> **First power-up:** on a fresh or factory-reset device, step 2 redirects to `/setup` instead of the main UI. Set a password there (8-63 characters) to provision the OTA credential. Once submitted the setup page closes permanently and the main UI loads. See [OTA update credential](#ota-update-credential) below.

<p align="center">
  <img src="/Images/UIDemo.png" alt="OpenHaldex C6 Web UI" width="900" style="max-width:100%;">
</p>

### Access point security

The access point is **open by default**. There is no in-UI password control and no button gesture to reset WiFi.

To require a password, set a build-time WPA2 credential and reflash. The AP comes up open whenever the credential is unset or shorter than the 8-character WPA2 minimum, so the default build is unaffected:

```ini
; platformio.ini - inject without committing the secret
-D OPENHALDEX_AP_PASSWORD='"${sysenv.OPENHALDEX_AP_PASSWORD}"'
```

```sh
export OPENHALDEX_AP_PASSWORD='your-ap-secret'   # then: pio run -e esp32c6
```

### OTA update credential

Over-the-air firmware updates and the state-changing API endpoints are gated by a credential that is **never committed to source**. There are two ways to provision one:

**Option A - first-run web setup (recommended)**

On a fresh device, connecting to the AP and opening `192.168.1.1` redirects to `/setup`, a minimal form that asks for a password and a confirmation. The password must be 8-63 characters. Submitting it writes the credential to NVS and permanently closes the `/setup` routes; subsequent visits redirect to `/`. There is no default password.

**Option B - build-time injection**

Pass the credential as a build flag injected from the environment. It is compiled in as a fallback, so the device already counts as provisioned and the `/setup` routes are skipped (they redirect to `/`). A credential later provisioned at runtime in NVS takes precedence over the build-time value:

```ini
; platformio.ini
build_flags =
  -D OPENHALDEX_OTA_PASSWORD='"${sysenv.OPENHALDEX_OTA_PASSWORD}"'
```

```sh
export OPENHALDEX_OTA_PASSWORD='your-ota-secret'
pio run -e esp32c6 --target upload
```

**Rotating the credential**

Once provisioned, rotate via an authenticated `POST /ota/credential` request. The `/setup` route is permanently closed after first provisioning and cannot be used for rotation.

**Fail-closed behaviour**

When no credential is provisioned, flashing and all state-changing API calls are **refused** (HTTP 503). The device never accepts a default or empty password.

---

## Low power mode (planned, not yet built)

> [!NOTE]
> Low Power Mode is not implemented in this fork yet. The three layers below describe the planned design. Layer 1 (idle AP shutdown) is self-contained and buildable now; Layers 2-3 need a spike to coordinate ESP32-C6 light sleep with the active SoftAP and both TWAI controllers. The current-draw figures below are upstream's numbers and are unverified on this fork until measured on a bench meter.

The controller is designed to live on a **permanent +12 V** feed. With Low Power Mode configured correctly it is expected to draw approximately:

| State | Current |
|-------|---------|
| Sleeping (car off, WiFi off, CAN quiet) | ~14 mA |
| Awake (CAN activity detected or WiFi client connected) | ~50 mA |

The plan is three layers of power saving, each building on the previous, all configured from the **Settings** page.

### Layer 1 - Idle AP shutdown (always active)

After **5 minutes** with no WiFi clients connected and no CAN activity, the controller automatically shuts down the WiFi AP and turns off the LED. WiFi is restored automatically as soon as CAN traffic resumes.

### Layer 2 - CAN sleep (optional)

Enable the **CAN Sleep** toggle in Settings. The ESP32-C6 CPU enters light sleep when idle; the TWAI peripheral powers down but preserves its receive queue; the first incoming CAN frame wakes the controller immediately.

### Layer 3 - CAN sleep aggressive (optional, builds on Layer 2)

Enable **CAN Sleep (Aggressive)** in Settings. The CAN transceiver chips shut down completely, CPU minimum clock drops to 10 MHz, and WiFi AP transmit power is trimmed. Wake is interrupt-driven from a GPIO ISR on each CAN_RX line.

### Setting it up

Low Power Mode needs one calibration step because every car idles its CAN bus at a different rate.

1. Park and lock the car. Wait 30 minutes or until the Chassis bus goes fully quiet.
2. Stay connected to the OpenHaldex WiFi AP while you check (the controller stays awake while a client is connected).
3. Open the Web UI and watch the **Chassis fps** and **Haldex fps** counters in Settings.
4. Set **LP Wake Threshold (fps)** to a value above the parked-bus reading, e.g. 5 if the bus is quiet at 0 fps, 15 if it idles at 8 fps.
5. Enable **CAN Sleep** (and optionally **CAN Sleep Aggressive**) in Settings.
6. Disconnect from the WiFi AP.

> [!NOTE]
> **Standalone mode:** with no chassis bus, any Haldex-bus CAN traffic wakes the module regardless of the slider value.

> [!NOTE]
> **Switched-ignition installs:** if the module is already powered off with the ignition, Low Power Mode saves little and is optional.

---

## Flashing firmware

This fork does not ship pre-built binaries. You need to build from source using PlatformIO.

**Build and flash via USB:**

```sh
pio run -e esp32c6 --target upload
```

Connect the controller using a **data-capable USB-C cable**. Power-only cables will not work.

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

If you want a ready-to-flash binary without building from source, use the official upstream firmware from [Forbes Automotive](https://forbes-automotive.com/pages/module-software-updater). Note that the upstream binary does not include the security and correctness fixes in this fork.

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
