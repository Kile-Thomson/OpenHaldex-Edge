<p align="center">
  <a href="https://forbes-automotive.com/?utm_source=github&utm_medium=readme&utm_campaign=openhaldex" target="_blank">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="/Images/FA-logo-white.png">
      <source media="(prefers-color-scheme: light)" srcset="/Images/FA-logo.png">
      <img src="/Images/FA-logo.png" width="250" alt="Forbes Automotive">
    </picture>
  </a>
</p>

# OpenHaldex Edge

> [!IMPORTANT]
> **OpenHaldex Edge is a fork of [Forbes Automotive OpenHaldex-C6](https://github.com/Forbes-Automotive/OpenHaldex-C6).** Forbes did the hard work: the reverse engineering, the Gen2, Gen4 and Gen5 logic, the hardware and the open-source release. Edge starts from their V9.00.0 firmware and adds to it. Same hardware, same wiring, same modes.
>
> Edge uses the **same MIT licence** as upstream, with nothing added to it. There are no plans to restrict it, now or later. Edge's own code is contributed under the same terms. See [Licensing](#licensing).
>
> For assembled modules, official firmware and support, go to [forbes-automotive.com](https://forbes-automotive.com/). Edge is a personal project, not affiliated with or endorsed by Forbes Automotive. Credit for the platform is in [Acknowledgements](#acknowledgements); what Edge adds is in [What Edge adds](#what-edge-adds) and the [CHANGELOG](CHANGELOG.md).

<p align="center">

![Platform](https://img.shields.io/badge/platform-ESP32--C6-blue)
![Hardware](https://img.shields.io/badge/hardware-Haldex%20Gen1%20%7C%20Gen2%20%7C%20Gen4%20%7C%20Gen5-green)
![Last commit](https://img.shields.io/github/last-commit/Kile-Thomson/OpenHaldex-Edge)

</p>

OpenHaldex is an open-source **Haldex AWD controller** for Volkswagen and Audi Group vehicles with Haldex Generation 1, 2, 4 (PQ chassis) and 5 (MQB) differentials.

The firmware runs on an **ESP32-C6**. It sits between the car and the Haldex, reads the CAN bus, and changes or generates the Haldex's commands so the differential locks the way you configured. It can work from the car's own CAN signals, or in Standalone mode for conversions with no chassis bus.

Edge takes Forbes's platform and puts day-to-day use first. The whole controller is set up from a phone: a dark web UI served by the module itself, a live engagement gauge and lock trace, a 3D expert map, per-car calibration, backup and restore, and updates straight from GitHub.

![OpenHaldex Edge web UI: dashboard, expert 3D map, Long Learn, basic modes, home WiFi and Bluetooth, and update from GitHub](/Images/openHaldexUI.png)

*The Edge web UI, served by the module over its own WiFi access point. No app to install; open a browser on your phone. These screenshots come from the UI running against a simulator, so the numbers are made up and the layout is real.*

## What Edge adds

Everything below runs on the Forbes hardware.

**From Forbes Automotive (V9.00.0, all included):**

- Gen1, Gen2, Gen4 (PQ, GM/SAAB) and Gen5 (MQB 0CQ, 0AY, VAQ) control, Standalone mode, Expert mode with the speed/throttle table.
- Learn Haldex, Long Learn and per-generation CAN frame blocks.
- Live diagnostics: UDS for Gen5, KWP2000 over TP2.0 for Gen2 and Gen4.
- CAN sniffing for SavvyCAN, low power sleep, force modes from TC / hazards / external button.
- Home WiFi (bridge mode), Backup & Restore and Bench Mode, contributed by louij2 (PR #39).
- Bluetooth LE for the DashCAN app, contributed by danati (PR #44).
- OTA updates with rollback protection, plus the release channels from PR #43.
- The PCB, enclosure and BOM files, now in this repo too.

**Added by Edge:**

- A restyled web UI: dashboard with engagement gauge, live lock trace and connection badge, drive-mode drawer, 3D expert map, tune slots saved on the module, Calibrate tab, installable as a phone app.
- A web-access rule for the home network: a browser reaching the module through your router signs in with the WiFi password. See [Home WiFi](#home-wifi-bridge-mode).
- A forced WiFi password on first power-up, and no CAN injection from the analyzer port until it is set.
- Updating from one file: the release's merged image updates firmware and web UI together.
- Lock engage and release timed in milliseconds, a steering-angle lock taper, per-car geometry for the slip readout, a per-car torque ceiling and a Launch PWM floor.
- Safety work: the learn sweep refuses to run in a moving car, stale CAN inputs fall back to safe values, control state is mutex-guarded, a stalled CAN controller revives itself.
- A host-runnable test suite (`pio test -e native`) and a release build with debug output off.

## Contents

- [Before you flash: upgrading from Edge v8 or upstream](#before-you-flash-upgrading-from-edge-v8-or-upstream)
- [Supported platforms](#supported-platforms)
- [Modes](#modes)
- [Basic tab: when lock is allowed](#basic-tab-when-lock-is-allowed)
- [Expert mode](#expert-mode)
- [Calibrate: Learn, lock calibration and geometry](#calibrate-learn-lock-calibration-and-geometry)
- [Long Learn](#long-learn)
- [Frame blocks](#frame-blocks)
- [Changing modes](#changing-modes)
- [WiFi setup](#wifi-setup)
- [Home WiFi (bridge mode)](#home-wifi-bridge-mode)
- [Backup & Restore](#backup--restore)
- [DashCAN app (Bluetooth)](#dashcan-app-bluetooth)
- [Low power mode and Bench Mode](#low-power-mode-and-bench-mode)
- [Live diagnostics](#live-diagnostics)
- [CAN sniffing](#can-sniffing-savvycan--gvret)
- [Installation](#installation)
- [Flashing and updating](#flashing-and-updating)
- [Building from source](#building-from-source)
- [PCB and enclosure](#pcb-and-enclosure)
- [Acknowledgements](#acknowledgements)
- [Upstream](#upstream)
- [Licensing](#licensing)
- [Disclaimer](#disclaimer)

---

## Before you flash: upgrading from Edge v8 or upstream

> [!IMPORTANT]
> **The first v9 flash is over USB.** The Bluetooth release changed the flash layout: the firmware slots got bigger (0x1C0000 each) and the web UI partition smaller (0x70000 at 0x390000). A partition table cannot be changed over the air, so a unit on Edge v8, or on an upstream build from before the Bluetooth release, needs one USB flash. After that, updates work over WiFi again. The update page tells you so if you try a file that will not fit.

Things worth knowing when you move from Edge v8:

- **Settings keys are unchanged.** The module still stores everything in the one `openhaldex` settings area, so a flash that leaves that area alone keeps your settings. The merged image is one file covering flash from 0x0 to the end of the web UI partition, and the settings area inside that range is filled with blank flash, so treat a merged-image USB flash as a factory reset and plan to set the unit up again. Edge v8 has no Backup & Restore, so write down your Expert table first or screenshot it. v9 units can export a backup (see [Backup & Restore](#backup--restore)).
- **The speed window now gates every lock path.** Disengage under speed, disengage above speed and minimum throttle apply to force modes (TC, hazards, external button) and to Expert mode, not only to the fixed-ratio modes. On Edge v8 those two bypassed the window. If you relied on a force mode locking the car below your "disengage under" speed, lower that setting.
- **From a home network the UI asks for a login.** Over the module's own WiFi nothing changes. See [Home WiFi](#home-wifi-bridge-mode).
- **The learn sweep needs the car stationary** (5 km/h or less). It refuses to start, or aborts, in a moving car.
- **The steering-angle lock taper is on by default on a fresh unit**, with the curve described under [Basic tab](#basic-tab-when-lock-is-allowed). A unit that carries Edge v8 settings keeps whatever its steering setting was.
- **The default correction factor** used before you run a learn follows upstream's formula (lock/2 + 20), where Edge v8 used (lock + 20)/2. Run the learn and it stops mattering.

---

## Supported platforms

- Generation 1 - PQ
- Generation 2 - PQ
- Generation 4 - PQ
- Generation 4 - GM / SAAB
- Generation 4 - Ford (ongoing)
- Generation 5 - MQB (0CQ)
- Generation 5 - MQB (0CQ VAQ)
- Generation 5 - PQ (0AY)

> Gen3 is not supported. The generation list shows a "Generation 3 - Volvo" entry, but there is no Gen3 drive logic behind it.

Pick the generation on the **Calibrate** tab. It has to match the car, or nothing else will behave.

---

## Modes

| Mode | Behaviour | LED colour |
|------|-----------|------------|
| Stock | OEM behaviour (frames pass through untouched) | Red |
| FWD | Zero lock | Green |
| 75:25 | 30% lock | Cyan |
| 60:40 | 40% lock | Neon pink |
| 50:50 | 100% lock | Blue |
| Expert | Lock from your speed/throttle table | White |

The percentages are the lock the controller asks for. In Stock the controller edits nothing: the car's own signals reach the Haldex as they are. In Standalone mode there is no car to pass through from, so Stock holds the clutch open, and the mode button skips Expert.

If you turn on **Disable Controller** (Settings), the module drops to Stock, and mode changes from the web UI and the app are refused until you turn it back on.

![OpenHaldex Edge web UI: dashboard, live lock trace, expert 3D map and diagnostics](/Images/UIDemo.png)

*Dashboard with the engagement gauge, the last 15 seconds of target against actual lock, the Expert 3D map and Diagnostics.*

---

## Basic tab: when lock is allowed

The Basic tab holds three guards that apply to every lock path: the fixed-ratio modes, Expert, and the force modes.

| Setting | What it does | Default |
|---------|--------------|---------|
| Disable under speed | No lock below this speed. Car parks, tight manoeuvres. | 0 (off) |
| Disable above speed | No lock above this speed. | 0 (off) |
| Disable under throttle | No lock until the pedal passes this percentage. Coasting. | 0 (off) |

A bound of 0 switches that side off. Lock is allowed only while speed sits inside the window and the pedal is above the threshold.

**Lock response times.** How long the commanded lock takes to ramp, in milliseconds for a full 0 to 100% travel. *Engage* (lock-up) defaults to 0, instant. *Release* defaults to 500 ms. A higher number is slower. The **Enable Rate Limiting** switch turns both ramps off for instant changes either way. The same release ramp is what the DashCAN app's "gradual lock release" setting changes.

**Steering gain taper.** Lowers the lock request as the steering angle grows, so the rear axle is not fighting the front in tight turns and car parks. It works on Gen2, Gen4 and Gen5 (0CQ and VAQ) and applies to every mode that produces lock. The default curve is 100% up to 45 degrees, 80% at 90, 50% at 180 and 20% at 360. In the UI you set where the taper starts, where it is fully applied, and the minimum it drops to. If the steering signal is missing or stale, the taper switches itself off and lock goes back to the full request. The live angle and applied gain are on the Diagnostics tab, so you can check the reading on your car.

---

## Expert mode

Expert mode sets the lock target from **speed and throttle** using a table in the web UI. It needs the car's CAN signals for speed and throttle, so it is not available in Standalone.

![Expert mode: speed by throttle lock table with the 3D surface below](/Images/expertmode.jpg)

*Expert mode in the Edge UI: a colour-coded 7 by 7 table over speed and throttle, the live 3D surface below, and a dot that rides the surface as you drive.*

- **Editing.** Tap a cell to edit it. Press and hold to select several cells at once.
- **3D surface.** The same table drawn as a surface: speed and throttle on the floor, lock as height. The operating-point dot shows where you are right now. It is a view; it does not change what is sent.
- **Curve view.** Below the grid, toggle between lock against speed (one line per throttle band) and lock against throttle (one line per speed band).
- **Tune slots.** Five named slots are stored on the module, with save, load and delete. A tune saved from one phone shows on any other phone.
- **Apply / Restore Defaults.** Nothing is written to the Haldex until you apply.

The speed window and the steering taper on the Basic tab apply to Expert too.

---

## Calibrate: Learn, lock calibration and geometry

Set this up once per car, top to bottom. Until a learn table exists, the Dashboard shows a dismissible banner saying lock is running on an estimate, and an uncalibrated guess drives worse than Stock. The tab does not block anything; it only nags.

1. **Generation.** Match the car. See [Supported platforms](#supported-platforms).
2. **Learn Haldex.** The controller steps through every lock percentage and records what your Haldex actually delivers, so a request for 40% gives 40%. Run it with the engine running, the car stationary (5 km/h or less) and Haldex CAN live. It takes a couple of minutes. If you cancel, or the car starts moving, the previous table is put back. Only a finished sweep replaces it. When a table exists the tab plots it: the commanded correction factor against the measured engagement, with a dashed 1:1 reference line. Points above the line mean your Haldex over-responds, below means it under-responds.
3. **Lock calibration** (Gen5). The torque ceiling in Nm, default 220. It lines up the lock you command with the lock the Haldex delivers. Run a Learn, then adjust it until the Sent against Returned line sits on the diagonal. It is not a strength dial: higher does not lock harder, and there is one correct value per car. Leave it alone unless the learn chart shows a gap.
4. **Launch PWM floor** (experimental, default 0%). Raises the minimum clutch PWM the Haldex is told to hold while lock is commanded, so engagement builds faster off the line. It only applies while lock is commanded, and it is kept below the maximum so the Haldex can still modulate. Watch the reported engagement as you tune: if the clutch PWM climbs but engagement was already near 100%, the extra PWM is only heat.
5. **Car geometry** (optional). Wheelbase, front and rear track, steering ratio and the speed below which slip is ignored. The defaults are for an Audi TT Mk3 (2505 mm, 1572 mm, 1543 mm, 15.0:1, 5 km/h). They only feed the per-wheel slip card on the Dashboard. They do not change how lock behaves, so leave them alone unless you want that card to be accurate on another car.

**Gen5 Fix Hunting.** Some Gen5 controllers, specifically the 554K variant, take the torque request in a different packing and can hunt at partial lock. Fix Hunting (default off) switches the Motor_11 packing to the other format. The v9 web UI does not have a switch for it; Long Learn tries it for you on Gen5 when the baseline learn is not smooth, and keeps it only if the result is better.

---

## Long Learn

If a normal learn is not clean (jumps, plateaus, never reaches 100%), Long Learn automates the manual loop of adding or removing a [frame block](#frame-blocks) and learning again. It is on the Calibrate tab. Run it with the car stationary, the engine running and Haldex CAN live. Allow 10 to 20 minutes.

The phases:

1. **Initial Sweep.** Every frame block for your generation is switched on and one learn runs at your current Launch PWM floor. This gives a baseline.
2. **BPK Adjust** (Gen5 0CQ and VAQ only, and only if the baseline is not smooth or does not reach about 90%). It tries Fix Hunting and keeps it only if the sweep improves. If lock is still short it raises the torque ceiling in 40 Nm steps, up to 500 Nm, until the target is reached.
3. **Sweeping Blocks.** Each extra block (anything outside your generation's default set) is removed one at a time and checked with a release-to-0-then-back-to-100% cycle. A block whose removal makes things worse goes back on. One that makes things better is flagged "affects, better without". One that makes no difference stays off. Tick *Also test the default (core) blocks* to test those as well; that takes longer and can leave the Haldex with no lock command for part of a sweep.
4. **Confirmation.** A full sweep is stored against the final block set.

The tracker shows the phase, the sweep count, the block under test, the current floor and torque ceiling, a reference score and live Sent and Returned bars. Cancelling, or losing Haldex data during the first sweep, puts back the blocks, floor, torque ceiling, Fix Hunting setting and learn table you had before. Moving above 5 km/h aborts the run the same way.

A **chassis notes** box (saved on the module, up to 200 characters) and **Export report (.txt)** produce a plain text record of the car, the calibration values, the recommended block set, the sweep log and the stored table. It is useful for sharing a known good layout for a chassis.

---

## Frame blocks

On the Calibrate tab, the **Frame blocks** list shows each CAN frame OpenHaldex can edit for your generation, with a switch for each. Switched off, the car's own message passes through untouched. Switched on, OpenHaldex modifies or generates that frame. Changes apply immediately and are saved per generation, and **Reset to Defaults** restores the generation's standard set.

You would use this to find which edited frame upsets a learn or raises a fault code, or to tailor a car that behaves unusually. Most users never need to touch it; Long Learn does the same job automatically. It applies in both normal and Standalone mode, and only to the generation currently selected.

---

## Changing modes

**On-board button.** A short press cycles Stock, FWD, 50:50, 60:40, 75:25, Expert, then back to Stock. In Standalone mode Expert is skipped. A long press clears the WiFi password and restores the default network name, then the access point restarts in first-run setup mode. See [WiFi setup](#wifi-setup).

**External button.** A short press cycles modes the same way. If you set **External button** to "Hold: Force Mode" in Settings, a short press does nothing and a long press engages the force mode you chose for it.

**WiFi.** The web UI at `192.168.1.1` or `openhaldex.local`.

**CAN.** Send a mode number to the module. This needs **Broadcast over CAN** switched on in Settings (on by default on a fresh unit). The module listens on `0x6A0` and reads Byte 0 as the mode number.

| Byte 0 | Mode |
|--------|------|
| 0 | Stock |
| 1 | FWD |
| 2 | 50:50 |
| 3 | 60:40 |
| 4 | 75:25 |
| 5 | Expert |

The same switch makes the module broadcast its state every cycle on `0x6B0`. Turning it on adds those two CAN IDs to your bus, so check nothing else already uses them.

```
data[0] = reserved (always 0)
data[1] = 1 when Standalone mode is on, otherwise 0
data[2] = engagement raw byte returned by the Haldex
data[3] = lock target percent requested by the firmware (0-100)
data[4] = vehicle speed in km/h, clamped at 255
data[5] = mode override flag
data[6] = current mode number (0-5)
data[7] = pedal value (0-100%)
```

**Force modes from the car's own signals.** Each of these is optional, off by default, and set up in Settings. Each has its own target mode (default 50:50). A priority setting decides which one wins when more than one is active.

- **TC / ESP button.** Pressing the traction control button to switch ESP off triggers force mode. The module reads the ESP-disabled bit (byte 7 on PQ, byte 6 on MQB).
- **Hazard switch.** While the hazards flash, the force target applies. Read from Blinkmodi_02 (`0x366`) on MQB. Pick FWD as the target and you get a way to open the coupling for towing, recovery or limping home.
- **External button.** The hold behaviour above.

---

## WiFi setup

1. Join the access point **OpenHaldex-C6** (you can rename it).
2. Open `192.168.1.1` or `openhaldex.local` in a browser.

### First power-up: set a password

A fresh or factory-reset module has no password. The access point comes up open, but only so you can reach one page: the browser is sent to `/setup`, which asks for a WiFi password and confirmation. Nothing else on the module answers until you set it, and CAN injection from the analyzer port is refused. Once you submit, the access point restarts protected with that password and the setup page closes. The password is the **WPA2 password of the access point** and also the sign-in for the [home-network side](#home-wifi-bridge-mode). It must be 8 to 64 characters.

There is no default password and nothing to build or flash to set one.

**Changing it later.** The WiFi Access Point card on the Diagnostics tab changes the network name and password. The access point always needs a password, so there is no "open" option there.

**Forgotten it?** Long-press the mode button on the unit. That clears the password and restores the default network name, and the setup page asks for a new one. Anyone with physical access to the module can do this, which is the intent: the unit is in your hands.

### Connecting from a phone

Use your phone's normal WiFi settings, not its Personal Hotspot. The module is the access point: it broadcasts its own network like a router does, and your phone joins it.

The module has no internet connection and does not need one. Your phone will say the network has no internet. That is expected. If your phone keeps dropping the network because of that warning, choose "Stay connected". The module does not act as a captive portal and does not hand out a gateway or DNS, so the phone keeps using mobile data for everything else.

---

## Home WiFi (bridge mode)

Optional. The module can also join a home or garage WiFi network as a client while keeping its own access point running. Then it is reachable at `openhaldex.local`, or at the address the card shows, from any phone or laptop already on that network. Leave the field blank and nothing changes.

Set it up on the **Diagnostics** tab under **Home WiFi (Bridge Mode)**: pick the network from the scan (or type a hidden one), enter its password, press **Save & Apply**. The card then shows the connection state, the address and the signal strength. The same card is repeated on the **Update** tab, because the phone needs internet to check GitHub for updates, and this is how it gets both.

> [!IMPORTANT]
> Putting the module on your home network means other devices on that network can reach it. So a browser coming in through the home network has to sign in: user name `admin`, password your access point's WiFi password. The check covers every page and every `/api` and `/ota` request. A phone joined directly to the module's own access point does not sign in; joining that network already needed the password. The CAN analyzer's host-to-CAN injection is also refused for home-network clients.

Things to know:

- **One radio.** The ESP32-C6 shares one radio between its access point and the home connection. A scan or connection attempt pulls it off the access point's channel for a second or two. So the module only looks for your home network for 20 seconds after starting, then once every 5 minutes. A saved network cannot keep interrupting the access point while the car is away from home.
- **Sleep.** Bridge mode mostly does not change [low power mode](#low-power-mode-and-bench-mode). With no CAN traffic and nobody using the UI, the module still sleeps after five minutes and drops off the home network. A browser that has the UI open through your router counts as a user, so an update over the bridge is not cut off.
- **Open networks.** A blank home-network password connects to an open network. The module accepts it; think before you do.
- **The backup tool.** `tools/openhaldex_config.py` does not send a login. Use it while joined to the module's own WiFi.

---

## Backup & Restore

A USB flash can wipe the settings, and nobody wants to re-type an Expert table. **Diagnostics > Backup & Restore** exports the Expert tune, saved map slots, the frame-block choices, every setting and the WiFi names to a JSON file, and imports them back in one go.

WiFi passwords are write-only on the device. They are never written to the file, and after an import you are asked once for any that were set. The Bluetooth pairing code is also left out of the file.

The same file works from a computer with [`tools/openhaldex_config.py`](tools/openhaldex_config.py):

```
python tools/openhaldex_config.py export backup.json
python tools/openhaldex_config.py import backup.json
python tools/openhaldex_config.py import backup.json --tune-only
python tools/openhaldex_config.py --host openhaldex.local export backup.json
```

The default address is `192.168.1.1`. The tool needs to be on the module's WiFi (see the note under [Home WiFi](#home-wifi-bridge-mode)). A backup made with Bench Mode on is imported without switching Bench Mode on while CAN is live. Export before you re-flash. Backup & Restore and Bench Mode were contributed upstream by louij2 (PR #39).

---

## DashCAN app (Bluetooth)

The module talks Bluetooth LE to the **DashCAN** phone app, so the basics are on the phone without joining the module's WiFi. The web UI stays the place for everything else (Expert map, learning, generation, frame blocks, updates). Bluetooth was contributed upstream by danati (PR #44).

**From the app:**

- **Live.** Mode buttons, controller on/off, lock target and actual, speed, throttle, rpm, boost, steering angle, CAN health and Haldex warnings.
- **Settings.** The ones you change per situation: force modes, brake and handbrake release, the speed and throttle window, steering-angle scaling, gradual lock release and its rate, live diagnostics and LED brightness. A change on the web UI shows in the app and the other way round.
- **Haldex diagnostics.** Temperatures, supply voltage, clutch current and duty (and oil pressure and estimated torque on Gen4). Needs [Live diagnostics](#live-diagnostics) switched on.
- **Gauges.** AWD Lock, AWD Lock Target, Steering and the Haldex temperatures for the app's dashboards and drive recordings.

**Connecting.** Bluetooth is on by default. The module appears as `OpenHaldex-XXXX` (the last two bytes of its address) and the app connects to the first one in range, or you pick it on the app's Bluetooth screen. Live data needs no pairing. The first change from a phone pairs it:

- The **first phone** pairs without a code (it may just ask you to confirm).
- After that, **every new phone needs a 6-digit pairing code**, shown on an already paired phone and in the web UI.

**On the module.** The Bluetooth card on the Diagnostics tab has the enable switch, a phone-connected indicator, the pairing code and **Forget Paired Phones**. Forgetting removes every paired phone, makes a new code, and lets the next phone pair without one (also forget the device in the phone's own Bluetooth settings). Bluetooth sleeps with the WiFi in [low power mode](#low-power-mode-and-bench-mode).

> [!NOTE]
> Until the first phone has paired, on a new module or right after Forget Paired Phones, anyone in Bluetooth range could be that first phone. Pair yours straight away. Firmware is never updated over Bluetooth.

The protocol is written up in [`documents/MOBILE_APP_OPENHALDEX.md`](documents/MOBILE_APP_OPENHALDEX.md), so other apps can use it.

---

## Low power mode and Bench Mode

The module is made to live on a **permanent +12 V** feed. With low power mode working, upstream's figures are about 14 mA asleep (car off, WiFi off, CAN quiet) and about 50 mA awake. Those numbers are inherited from upstream and have not been measured on Edge builds. Treat them as a guide.

Everything is on the Settings tab.

- **CAN Sleep** (on by default). After 5 minutes with no one connected to the WiFi, no one with the UI open, and CAN traffic below the wake threshold, the module shuts the WiFi access point and the LED off and lets the CPU light-sleep. The first CAN frame above the threshold brings it back. Bluetooth sleeps and wakes with the WiFi. With CAN Sleep switched off, none of this happens and the WiFi stays up.
- **CAN Sleep (Aggressive)** (off by default). Also puts the CAN transceivers in standby, drops the CPU minimum clock to 10 MHz and trims the WiFi transmit power. Waking is by interrupt on the CAN receive lines. Use it if the car sits for days and you want the lowest standby current.
- **LP Wake Threshold** (default 1100 frames per second). In an OEM install the module stays awake while the chassis bus is at or above this rate and sleeps below it. Set it above your car's parked-bus rate and below its driving rate. To find the parked rate: park and lock the car, wait until the chassis bus goes quiet, join the module's WiFi (it stays awake while you are connected), and watch the **Chassis fps** and **Haldex fps** counters on the Settings tab. In Standalone mode there is no chassis bus, so the module wakes on a fixed 50 frames per second of Haldex traffic and this slider does not apply.
- **USB.** A computer on the module's USB port keeps the WiFi up, so you can use the dashboard on the bench without a CAN source.

If your module is on a switched ignition feed, low power mode saves little and is optional.

### Bench Mode

The module cannot tell a harnessed car that is asleep from no harness at all: both look like zero CAN traffic. So on the bench with CAN Sleep on, the WiFi drops after five minutes. **Bench Mode** (Settings, under CAN Sleep) holds the WiFi up regardless.

It clears itself. The moment either CAN bus shows traffic in a power cycle, Bench Mode stops applying and normal sleep resumes, so it cannot weaken the battery protection once the unit is in the car. The UI also greys the switch out while CAN is detected, and the module ignores an attempt to turn it on while CAN is live.

---

## Live diagnostics

OpenHaldex can ask the Haldex for live measurements and show them in the web UI, as a scan tool would. This uses the module's diagnostic channel, so it is **off by default** (Settings > **Enable Live Diagnostics**). It picks the protocol from the generation you set.

- **Gen5 (0CQ, 0AY, VAQ): UDS.** Terminal voltage, control module temperature, clutch temperature, cooling fin temperature, clutch current, PWM and voltage. Clutch and fin temperatures show `--` when the decoded value is not plausible rather than showing a wrong number.
- **Gen2 and Gen4: KWP2000 over VW TP2.0.** Oil temperature, clutch plate temperature, supply voltage, oil pressure, estimated torque, clutch valve duty and current. Gen4 values are decoded; the raw measuring-block bytes are also exposed.

If VCDS, ODIS or another scan tool is connected, the module detects its requests on the bus and pauses its own polling until the tool goes quiet, so the two do not collide. If a tool still will not connect, switch Live Diagnostics off. Extra CAN traffic can cause spurious dash errors.

**Diagnostic tester reads.** Separately from the polling above, OpenHaldex answers three supplier-specific data identifiers on the Haldex address, so a tester on the OBD port (such as the Rokketek gauge) can use them: the lock command and engagement (`0xFDA0`), per-corner wheel slip (`0xFDA1`), and a write to set the drive mode (`0xFDA2`). The slip values are each wheel's speed against what the steering geometry predicts, so a straight launch and a mid-corner break-loose both read as real slip. The four slip values are also on the Dashboard.

---

## CAN sniffing (SavvyCAN / GVRET)

Switch on **Analyzer Mode** in Settings to capture CAN frames from both buses.

> [!WARNING]
> Analyzer Mode turns off active Haldex control and puts the module back to OEM pass-through. It also stops the `0x6B0` broadcast.

The analyzer speaks GVRET to SavvyCAN, over WiFi (TCP port 23) or over the USB serial port. Choose with the **SavvyCAN via WiFi** and **SavvyCAN via Serial** switches.

1. Join the module's WiFi and enable SavvyCAN via WiFi in Settings.
2. In SavvyCAN: Connection > Add New Device Connection > Network Connection (GVRET).
3. IP `192.168.1.1`, port `23`, CAN speed `500000`.

For USB, enable SavvyCAN via Serial and add a Serial Connection (GVRET) at 500000 on the module's COM port.

Receiving is always allowed. **Host-to-CAN injection** (sending frames from SavvyCAN onto the car's bus) is refused until the access point password is set, and always refused for a client that came in through the home network.

---

## Installation

> [!TIP]
> ### Optional Plug & Play Harness (Recommended)
>
> Recommended for quick installation (and removal)  -  typically **<10 minutes** on Generation 1 Controllers.
> The latest harnesses for Generation 5 are even simpler and you'll be experiencing your Haldex controller in less than 30 seconds:

- Lift the rear seat
- Split the factory 6-pin Haldex connector
- Install your new harness & OpenHaldexC6 Controller
- Drive it (you could put the seat back down too, if you want!)

> For Generation 1 Controllers:

- Remove original connector and install the long end of the harness onto the differential.
- Route the long end along with the original connector back into the boot floor via. the OEM grommet
- Install and secure the OpenHaldexC6 Controller to the new harness, pairing it with the original plug

Forbes's step-by-step instructions are in the **[OpenHaldex Installation Guide](https://openhaldex.com/docs/OpenHaldex_Installation_Guide.pdf)**. This section is carried over from the Forbes README.

Video: **Installation demo (YouTube Short):** https://youtube.com/shorts/iUkNh9NbyKY?si=IhgqLIi0WM8wXqe9

Video: **Installation demo (YouTube Short):** https://youtu.be/Wu-u-Dz1444

> [!WARNING]
> ### Manual Wiring (No Harness)
>
> Modules sold without a harness include connector pins for manual wiring. This is a little harder and more involved than using the optional harness, but following the installation guide above it can still be completed easily. 

Gen1:
- Chassis Connector: `1J0-973-714`
- Haldex Connector: `1J0-973-814`

Gen4:
- Haldex Connector  -  `VW 1J0-973-713`
- Vehicle Connector  -  `VW 1J0-973-813`

Build this as a **Y-branch** harness between the two VW 8 or 6-pin connectors, with a long tail to the MX plug.

Routing summary:

- **Permanent power (Term30) and ground must also be branched to the OpenHaldex controller**:
  - Term30 -> MX Pin 1
  - Ground -> MX Pin 2
  
- Chassis CAN is taken from the Vehicle side and sent to the controller:
  - Vehicle Pin 5 -> MX Pin 3 (Chassis CAN Low)
  - Vehicle Pin 6 -> MX Pin 4 (Chassis CAN High)

- Returned CAN from the controller then goes to the Haldex side:
  - MX Pin 5 -> Haldex Pin 5 (Haldex CAN Low)
  - MX Pin 6 -> Haldex Pin 6 (Haldex CAN High)

Generation 1 to 4:
- Vehicle Connector  -  `VW 1J0-973-714`:

| Pin | Signal | Notes |
|----|------|------|
| 1 | Term15 | Pass-through: Vehicle -> Haldex |
| 2 | Ground | Pass-through: Vehicle -> Haldex and branch to MX Pin 2 |
| 3 | Brake Light | Pass-through: Vehicle -> Haldex |
| 4 | Handbrake | Pass-through: Vehicle -> Haldex |
| 5 | K-Line | Pass-through: Vehicle -> Haldex |
| 6 | N/A | Not Used |
| 7 | Chassis Low | To MX Pin 3 (chassis side) |
| 8 | Chassis High | To MX Pin 4 (chassis side) |

- Haldex Connector  -  `VW 1J0-973-814`:

| Pin | Signal | Notes |
|----|------|------|
| 1 | Term15 | Pass-through: Vehicle -> Haldex |
| 2 | Ground | Pass-through: Vehicle -> Haldex and branch to MX Pin 2 |
| 3 | Brake Light | Pass-through: Vehicle -> Haldex |
| 4 | Handbrake | Pass-through: Vehicle -> Haldex |
| 5 | K-Line | Pass-through: Vehicle -> Haldex |
| 6 | N/A | Not Used |
| 7 | Chassis Low | To MX Pin 5 (Haldex side) |
| 8 | Chassis High | To MX Pin 6 (Haldex side) |

Generation 5:
- Vehicle Connector  -  `VW 1J0-973-813`:

| Pin | Signal | Notes |
|----|------|------|
| 1 | Term15 | Pass-through: Vehicle -> Haldex |
| 2 | Ground | Pass-through: Vehicle -> Haldex and branch to MX Pin 2 |
| 3 | Term30 | Pass-through: Vehicle -> Haldex and branch to MX Pin 1 |
| 4 | N/A | Not used |
| 5 | Chassis Low | To MX Pin 3 (chassis side) |
| 6 | Chassis High | To MX Pin 4 (chassis side) |

- Haldex Connector  -  `VW 1J0-973-713`:

| Pin | Signal | Notes |
|----|------|------|
| 1 | Term15 | Pass-through: Vehicle -> Haldex |
| 2 | Ground/MALT | Pass-through from Vehicle side |
| 3 | Term30 | Pass-through from Vehicle side |
| 4 | N/A | Not used |
| 5 | Haldex Low | From MX Pin 5 (Haldex side) |
| 6 | Haldex High | From MX Pin 6 (Haldex side) |

![Gen4/Gen5 Y-Branch Harness Diagram](/Images/Gen4_Gen5_Y_Branch_Harness.png)

### MX23A12NF Connector Pinout

| Pin | Signal | Notes |
|----|------|------|
| 1 | Vbatt | +12 V |
| 2 | Ground/MALT | Ground |
| 3 | Chassis CAN Low | To chassis/ECU side |
| 4 | Chassis CAN High | To chassis/ECU side |
| 5 | Haldex CAN Low | To Haldex differential |
| 6 | Haldex CAN High | To Haldex differential |
| 7 | Switch Mode External | +12 V to activate |
| 8 | Brake Switch In | +12 V input |
| 9 | Brake Switch Out | Gen1 / 2 differentials only |
|10 | Handbrake Switch In | +12 V input |
|11 | Handbrake Switch Out | Gen1 differentials only |

---

## Flashing and updating

### First flash over USB

Connect the module with a **data-capable USB-C cable**; power-only cables will not work.

Each tagged [release](https://github.com/Kile-Thomson/OpenHaldex-Edge/releases) attaches one merged image, `openhaldex-c6-<tag>-merged.bin`. It holds the bootloader, partition table, firmware and web UI, so it is a complete flash-from-scratch build. Flash it at offset `0x0`:

```sh
esptool.py --chip esp32c6 write_flash 0x0 openhaldex-c6-<tag>-merged.bin
```

Or drag it into a browser flasher such as [ESP Web Tools](https://web.esptool.js.org/). `SHA256SUMS.txt` is attached so you can check the download. The release also carries the separate app, littlefs, bootloader and partition binaries.

Because the merged image blanks the settings area, a USB flash resets the module's settings (see [upgrading](#before-you-flash-upgrading-from-edge-v8-or-upstream)). If you are already on v9, export a backup first.

### Updating over WiFi

Once a module is on the v9 flash layout, update it from its **Update** tab. Nothing on the module fetches from the internet itself: your browser gets the files and pushes them to the module.

**Update from GitHub.** The browser needs internet while it can still reach the module. The reliable way is [bridge mode](#home-wifi-bridge-mode): join the module to your router, put the phone on the same network, and open the address the Home WiFi card shows. (On the module's own access point most phones turn mobile data off, so that route is best effort.)

1. Press **Check for updates**. The page contacts the module first, then fetches the release list from this repository's `ota` branch on GitHub, with a mirror as a fallback. If there is no internet it reads the module's bridge status and tells you what to do: join the network the module is on, fix a dropped link, or set up Home WiFi.
2. Pick a version. Newer stable releases are listed; tick **Show beta / older versions** for the rest. The **Latest build** channel also offers the newest release again, for when its files were rebuilt in place after a fix landed. Going backwards is allowed, and the confirm warns that an older release may not have this page.
3. Press **Install**. The page shows six stages (get UI, flash UI, verify, get firmware, flash firmware, reboot) with elapsed time. The web UI goes first, then the firmware, and the page waits for the reboot.

Keep the page open and the screen on during an install.

**Update from a File.** A card on the same tab takes a file you already downloaded, with no internet needed. Give it the release's merged image (it updates firmware and web UI together) or a bare `firmware.bin` or `littlefs.bin`; it works out which from the file's first bytes. A merged image is split by flash offset, and the bootloader, partition table and settings are never touched.

**From the command line.** The module takes an HTTP upload to `/ota/update` (merged image, firmware or web UI image) or `/ota/update/fs` (web UI image only). It does not speak espota, so PlatformIO's `--upload-port <ip>` will not work.

```sh
curl -F "update=@firmware-merged.bin" http://192.168.1.1/ota/update
curl -F "filesystem=@littlefs.bin" http://192.168.1.1/ota/update/fs
```

Through the home network add `-u admin:<your WiFi password>`. `GET /ota/check` reports whether an update is allowed right now and why not.

**Safety gate.** In the bench (no CAN detected) updates are allowed any time. In the car, an update needs the car stationary, both CAN buses healthy and no Haldex temperature fault; the module drops to Stock when an update starts. A new firmware image boots in a pending state and is confirmed only after the web UI has been reached on it or after 60 seconds of uptime. If it crash-loops before then, the bootloader goes back to the previous firmware. A web UI update unmounts the filesystem while it writes, and if one fails partway the module serves a built-in recovery page at `/` where you can upload again, so a bad web UI flash does not strand the unit.

> [!NOTE]
> USB remains the way to do a first flash or to recover a module that will not boot. OTA is for modules already running v9.

---

## Building from source

```sh
pio run -e esp32c6 --target upload       # build and flash over USB
pio run -e esp32c6-release                # release build, debug output off
pio run -e esp32c6-release -t buildfs     # web UI image
python scripts/merge_firmware.py --build-dir .pio/build/esp32c6-release
pio test -e native                        # host test suite, no hardware needed
```

`tools/mock_ui_server.py` serves the web UI from `data/` against a fake API, so you can work on the UI in a desktop browser with no module attached.

CI builds, tests and packages each tagged release, and publishes the update feed that the Update tab reads.

---

## PCB and enclosure

The Gerbers, schematic, BOM and 3D model are in [`PCB/`](PCB/), and the enclosure STLs in [`Enclosure/`](Enclosure/), as published by Forbes Automotive. Use them to make your own units if you wish. Pinout and function stay the same across the supported enclosure versions.

![OpenHaldex-C6 PCB, top](/Images/BoardTop.png)
![OpenHaldex-C6 PCB, bottom](/Images/BoardBottom.png)

Assembled modules are available from Forbes Automotive: **[OpenHaldex C6 Controller](https://forbes-automotive.com/products/openhaldex-controller?utm_source=github&utm_medium=readme&utm_campaign=openhaldex)**.

---

## Acknowledgements

- **Forbes Automotive** - the OpenHaldex-C6 platform: the reverse engineering and open-source implementation for Gen2, Gen4 and Gen5 Haldex systems, the hardware, and the continued work on all of it. Edge exists because of it.
- **louij2** (Luca C) - Home WiFi bridge mode, Backup & Restore and Bench Mode (upstream PR #39).
- **danati** - Bluetooth LE for the DashCAN app, bigger OTA slots and the lock-release fix (upstream PR #44).
- **A Banging Donk** - [the original OpenHaldex project](https://github.com/ABangingDonk/OpenHaldexT4) for Gen1 vehicles.
- **Chris "meatro" (SpringfieldVW)** - [OpenHaldex-S3](https://github.com/meatro/OpenHaldex-S3) (MIT), the origin of the CAN analyzer / GVRET (SavvyCAN) tooling and parts of the map editor. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- **Arwid Vasilev** - PCB redesign (V1.02).
- **LVT Technologies** - OTA update integration (now deprecated, still appreciated).

---

## Upstream

Edge keeps Forbes's history and tracks **[Forbes-Automotive/OpenHaldex-C6](https://github.com/Forbes-Automotive/OpenHaldex-C6)** as its upstream. To pull their changes:

```sh
git remote add upstream https://github.com/Forbes-Automotive/OpenHaldex-C6.git
git fetch upstream
git merge upstream/main
```

For assembled hardware, official firmware and support, go to the upstream project and [Forbes Automotive](https://forbes-automotive.com/). Their updater page flashes their build, not Edge's.

Edge's firmware is built on upstream V9.00.0 plus its follow-up pull requests. The Forbes `Releases/` binary folder and Discord workflow are not part of this repo; Edge publishes through tagged GitHub releases. Third-party attribution from the shared lineage is in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

---

## Licensing

OpenHaldex Edge is open source under the [MIT License](LICENSE.md), the same licence as upstream. The firmware, web UI, tools and hardware design files are published so you can build, inspect, modify, redistribute and fabricate a controller for any purpose, commercial use included, as long as you keep the copyright and licence notices.

- Forbes Automotive's original code and hardware design files: MIT, copyright Forbes Automotive.
- Edge's own additions: the same MIT terms, copyright Kile Thomson.
- Portions derived from OpenHaldex-S3 (meatro): MIT, notice preserved.
- The original OpenHaldex (Gen1, ABangingDonk): see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

If you redistribute source or binaries, keep LICENSE.md and THIRD_PARTY_NOTICES.md with them.

---

## Disclaimer

> [!CAUTION]
> This device modifies Haldex behaviour and should only be used **off-road or on a closed course**.
>
> The unit may behave unpredictably and could increase drivetrain wear.
>
> **Use at your own risk.** Forbes Automotive and the Edge contributors are not responsible for damages resulting from the use of this device or software.
