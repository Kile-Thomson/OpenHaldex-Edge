#pragma once
#include <OpenHaldexC6_defs.h>

// Current firmware version
#define FW_VERSION "8.00.14" // update this with every firmware release AND change .html version query param to force cache refresh of web UI

/*
Version Control:

*** remember and update FW_VERSION in '_defs.h' ***

V1.00.0 - basic code for testing
V1.01.0 - added in reliable mode changing and eeprom saving
V1.02.0 - confirmed Gen1 (OEM & Standalone), Gen4 (Standalone)
V1.03.0 - confirmed Gen2, updated general codebase
V1.04.0 - added feedback for incoming brake/handbrake sensors.  Added options to invert if required
V1.05.0 - brake out GPIO mapped incorrectly - changed
V1.06.0 - added 6040 split
V1.07.0 - added OTA updates (!) Thanks to Sasha!
V1.08.0 - Sorted crash when long press / WiFi reset
V1.09.0 - revised throttle AND speed setpoints to work together with enable/disable lock - thanks to Chris!
V1.10.0 - added custom mode to allow custom lock percentage (based on speed/throttle/body)
V1.11.0 - added SavvyCAN

V2.00.0 - changed to PlatformIO / VS Code
V2.01.0 - added custom UI with:
        > 'on TC, enable 5050' checkbox & 'on ext. button hold, enable 5050' checkbox
        > expert editor with 7x7 array of speed/throttle/lock
V2.02.0 - added ECU speed stale, use ABS, confirmed TC flag (Bremse_1)
V2.03.0 - added speed at Bremse_3 and 'Restore Defaults' clearing onhaldex binary state Expert without refresh.  
V2.04.0 - added Force Mode (rather than fixing lock at 100% when TC active), changed Speed/Throttle/Lock Axis text
V2.05.0 - added Lock Target to Stock - this ensures that a % is achieved when TC/ExtBtn is enabled
V2.06.0 - added Haldex State in binary to better understand the state of the Haldex 
V2.07.0 - copied actual lock to requested lock in stock mode so it looks 'cleaner'
V2.10.0 - release
V2.10.1 - fixed the CAN health - always showed Unhealthy because it wasn't updating the flag

V3.00.0 - added support for Gen5 and Gen4 GM/SAAB Gen4 (but logged under 41 as in 4.1 since it's a variant of Gen4)
V3.00.1 - added 'Disable Onboard Button' and 'Disable External Button' options to allow disabling of the onboard button & added Learn Haldex function

V4.00.0 - added support for Gen41 dual-bus (late Insignia) - standalone frames, plus feedback decoding per GMW8762 PPEI
V4.00.1 - added low power mode to disable WiFi AP when no CAN traffic for 5+ minutes and no clients connected, to reduce power consumption when used in a parked car. WiFi restarts automatically when CAN traffic returns.
V4.00.2 - added Password Protection

V5.00.0 - added 'Fix Hunting' option to switch Motor_11 to BPK packing, which can help with hunting at partial lock on 554K controllers

V6.00.0 - added interrupt-based CAN handling, revised Hazard force mode handling, added LED brightness control, adjusted sleep configuration for low power mode 
V6.01.0 - added WiFi naming and better sleep configuration for low power mode, added 'Follow Hazard' force mode option, added 'Disable Hazard Force Mode' option to allow disabling of hazard force mode when the hazard switch is active, added 'Hazard Force Mode Source' option to select which CAN signal is used to trigger hazard force mode (Blinkmodi_02 vs GATEWAY_72)

V7.00.0 - added support for Gen5 (0AY) Haldex - a mix of Gen2 and Gen4 (but ultimately Gen5 chassis).

V8.00.0 - added support for Ford (based on example CAN data, totally untested!)
V8.00.1 - added fix for password and SSID change

V8.00.2 - added fix for TC/hazards not re-enabling stock mode
V8.00.3 - single auth boundary: WiFi AP password (forced on first boot) replaces the separate HTTP login; cache-bust style.css
V8.00.4 - port dashboard UI from OpenHaldex: drive-mode drawer, pseudo-3D expert tune surface, compact glance dashboard (stat tiles + status chips + collapsible UDS)
V8.00.5 - single-line status header, OpenHaldex Edge title with themed version pill, expert-editor drag-select block edit + Smooth + selection dots on the 3D surface
V8.00.6 - phone tune multi-select behind a Select-cells toggle (tap toggles, drag paints; page no longer scroll-fights); dashboard reworked so target duty is the hero headline and clutch/module/fin temps sit permanently at the top of the glance grid; full single-row header
V8.00.7 - expert map library (built-in presets, browser-saved slots, file import/export) and user-selectable dashboard glance tiles; all client-side, no new device endpoints
V8.00.8 - expert multi-select now works on touch (tap-to-select rebuilt on plain click events; drag kept as an enhancement); removed the redundant mode badge from the header (the hero mode pill is the single source of truth and now carries the force-mode annotation)
V8.00.9 - saved map slots now live on the device instead of the phone: 5 named slots in device NVS, new /api/maps endpoints (list/get/save/delete), Save As/Load/Delete act on those slots so a tune saved from one phone is visible from any phone; removed the phone-side file Export/Import (device is now the single source of truth)
V8.00.10 - removed the firmware version pill from the header (frees space on narrow phone screens; version still shown on the Diagnostics page); shrank the Haldex Engagement gauge card so it takes less vertical room on mobile
V8.00.11 - re-added a compact current-mode badge to the header status bar (now that the version pill is gone there is room); shows the live base mode from any tab, force-trigger annotation still rides the hero pill
V8.00.12 - expert map library: removed the built-in preset templates (kept only real on-device saved slots); moved the tune-commit button directly below the 3D surface and renamed it Apply; press-and-hold a cell now enters multi-select without the toolbar button
V8.00.13 - expert page: Restore Defaults now sits beside Apply below the 3D surface; corrected the "Restore Defaults" lock table to match the firmware's actual shipped default (it was applying a different table); dropped the redundant "Saved on device" heading from the Maps dropdown; collapsed the verbose expert instructions into a one-line lead plus an expandable so the grid sits higher on a phone
V8.00.14 - Gen5 BPK (Fix Hunting) full-lock torque is now a user setting instead of a hardcoded 220 Nm: adjustable 100-500 Nm on the Settings page so a controller that never reaches full engagement can be dialled in (MQB signal max 509 Nm). Fixed the underlying uint8 math that capped BPK torque at 255 Nm regardless, and de-duplicated the standalone and CAN-passthrough Motor_11 packing into one host-tested function so the two modes can no longer drift. Also corrected the VAG no-learn-table fallback correction-factor formula (was over-delivering ~20% engagement)
*/


/*

** to do **:
        > add throttle/speed axis refresh
        > add 'ota' to match existing layout
        > add reduction in throttle/speed off
        > move CAN into interrupt based - ESP_INTR_FLAG_IRAM
*/