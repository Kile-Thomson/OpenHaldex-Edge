# Golden Haldex Knowledge

Field-verified reference for how the MQB Gen5 Haldex actually behaves and how
OpenHaldex interacts with it. The point of this file: separate **ground truth**
(external sources, live car data, opendbc) from **inherited assumption** (Forbes /
upstream comments that assert their own correctness). Firmware inherited the Gen1-4
driving logic from upstream; the Gen5 (MQB) layer is newer and softer ground, so
several "facts" in the code are educated guesses, not spec.

Reference car for all field data below: Audi TT Mk3 8S, 02Q 6-speed manual, tuned
(~600 Nm on the dyno), Gen5 0CQ 907 554 controller, OpenHaldex wired **inline**
(passthrough/frame-editor path, not standalone synthesis).

---

## 1. OpenHaldex never commands the clutch or the pump. It spoofs engine torque.

The MQB Haldex takes **no** direct "lock X%" command. It reads the engine ECU's
torque broadcast (Motor_11, `0x0A7`) and runs its **own** control law to decide
clutch engagement and pump duty. OpenHaldex is a man-in-the-middle: it overwrites
Motor_11 (and ESP_14) with inflated torque numbers so the factory law pre-loads the
clutch harder. It never drives the pump/solenoid PWM - that stays entirely inside
the Haldex ECU. (Details: vault note "OpenHaldex - MITM CAN Spoofer, Never Drives
the Pump".)

Consequence: everything OpenHaldex does is *persuasion*, not *command*. We change
what the module believes the car is doing; the module decides the rest.

## 2. `bpkCeilingNm` is a spoof-gain knob, NOT your engine's torque. (ground truth)

The single most misunderstood setting. In BPK packing the lock command (0..254)
maps onto **10 Nm .. bpkCeilingNm Nm** (`bpk_pack_motor11`, `Calculations.cpp:1647`).
It is the torque we *claim* at full lock command - a gain on how hard the module
leans on the clutch. It is not a statement about the engine, and it is not a limit
on what the Haldex can read.

Field-verified on the reference car:
- **Ceiling 150 -> request vs actual is 1:1** across the whole lock range. This is
  the correctly-scaled gain for this coupling.
- **Ceiling 550-600 (near real engine torque) -> clips almost immediately.** Even a
  small lock% claims huge torque, so the module slams the clutch shut early. Not a
  fault - it is over-claiming into the module's non-linear zone.

So the right ceiling is NOT "match your dyno figure." Lower is not "less honest" -
it is the linear-response gain for your specific clutch. 150 is car-specific; the
firmware default stays 220 (inherited, preserves prior behaviour). Only a road-test
sweep should change the ceiling per car. See `ROAD-TEST-BPK-CEILING.md`.

Why the ceiling exists at all: V3 packing pins the torque fields near-max (~494 Nm
Ist) constantly; BPK made it proportional but capped at a guessed 220 Nm. The whole
knob is an artifact of the two-packing design. (Quantified: vault note "OpenHaldex -
MQB Motor_11 Torque Spoof Verified Against opendbc".)

## 3. V3 vs BPK ("reduce oscillation" / Fix Hunting toggle) - what it really is

The toggle picks the **recipe for one CAN frame** (Motor_11), nothing more:

- **V3 (OFF):** empirical packing. Pins three torque fields at `0xFA` (~423/494 Nm),
  only bytes 6/7 modulate. Trial-and-error, "what works." Confirmed on 554C/D/H.
- **BPK (ON):** DBC-correct packing. Every field scales together with the command,
  slew-limited. Factory-shaped frame.

Externally validated (upstream README, independent of this fork): **certain Gen5
controllers - specifically the 554K variant - use a different torque model, and the
standard Learn will not produce accurate results on them; use Fix Hunt instead.** So
both modes are genuinely real - different 554 variants want different torque models.
Which one your car wants is decided by the `X` letter in `0CQ 907 554 X`, or by the
partial-lock behaviour: if partial lock hunts, you need BPK.

Reference car: BPK at ceiling 150 gives clean 1:1 with no hunting, so this car is
happy on BPK. It only hunts when the ceiling is over-claimed (see #2), NOT because
of the packing itself.

**Honest gap:** the "V3 pins fields at 0xFA," "554K hunts," and exact byte recipes
are fork-internal assumptions - there is no public VW/BorgWarner Haldex control
spec. The opendbc torque *field* definitions (below) are the only externally-proven
part.

## 4. Learn: what it calibrates, and the packing-provenance trap

Learn sweeps correction factor 0->100 and records the engagement the clutch reports
back at each step, building `target -> CF` so "20% target" yields "20% *reported
engagement*." It is a **clutch-engagement** calibration.

Two traps:
- **Engagement % is NOT torque-split %** (see #6). Learn does not, and cannot,
  prove that 20% engagement = 20% of torque to the rear.
- **A learn table is recorded under one packing and must be replayed under the
  same one.** During any learn the passthrough path forces BPK (`Calculations.cpp:1115`,
  `motor11_use_bpk_packing` returns `fixHunting || haldexLearnActive`), so on current
  firmware a table is always a BPK-language artifact. Drive it in V3 and the lookup
  hands V3 a CF that means something else -> stuck-at-100 at idle in Expert. This is
  the "learn with the switch off, then it sits at 100%" symptom. (Fix under
  discussion: gate the table to the packing it was recorded under.)

**Standalone path asymmetry (open bug):** the standalone frame generator
(`StandaloneCAN.cpp:1869`) uses raw `if (!fixHunting)` and does NOT force BPK during
learn, so a standalone learn with Fix Hunting off records a flat ~100% garbage
table. The passthrough path is protected; standalone is not.

## 5. Clutch pump PWM ~60% at full lock is NORMAL and healthy. (ground truth)

The "clutch PWM" on the Rokketek gauge is VCDS parameter **IDE05708 "Haldex clutch
pump PWM signal"** - the duty the Haldex ECU sends to its own oil pump. OpenHaldex
never sets or caps this; it is 100% inside the module.

Why ~60% at full lock is the *reassuring* reading, not a limit:
- Gen5 is hydraulic. The pump builds pressure to the **44 bar relief valve**, then
  only has to *hold* it. Holding takes far less duty than building.
- The Haldex runs a **pump-learn** (VCDS basic-settings, engine idling, ~1 min) that
  measures how much current/PWM *your* pump needs to reach pressure. A healthy pump
  needs less -> sits lower. A worn pump sits *higher*.
- PWM duty and clamp force are not linear. 60% duty can be full clamp.

The warning sign is the opposite: PWM pinned near 100% and *staying* there under
load = pump wear or a pressure leak. Steady ~60% is good.
Sources: haldexrepairs.co.uk Gen5 fault-finding guide, jcr-leeds.com Gen5 servicing,
vdveer-engineering.nl pump-learn.

## 6. Engagement % is not torque-split %, and OEM does it differently. (ground truth)

OpenHaldex commands *engine torque* (Motor_11) and lets the Haldex infer engagement.
The **factory ESP** instead sends the Haldex a **rear-axle torque request in Nm**
(`BR_Vorg_Allrad_Max`, "100% = 2000 Nm", referenced `Calculations.cpp:1152`). So the
OEM-correct control axis is rear-axle Nm, closed-loop - not an engagement percentage
spoofed through the engine-torque channel. OpenHaldex's approach works but is a
side-door, and it is the honest answer to "is this how VW/Audi would design a
performance controller?": no - they command rear Nm directly.

**Correction to an earlier claim:** the rear-axle torque feedback in Nm
(`received_sec_axle_torque_nm`, frame `0x1CC`) is decoded **only** on the Gen41
GM/Vauxhall FDCM path (`can.cpp:851`, `case 41`). It is **not** decoded on the Gen5
MQB path (`case 50`). So on an MQB car there is currently **no** rear-axle Nm ground
truth in the firmware - the only feedback is engagement % from `0x118`. Exposing a
real rear-Nm signal on MQB would require finding and decoding the equivalent MQB
frame; it is not already captured.

## 7. The 16-bit temperature decode is an UNVALIDATED guess and reads impossible values. (assumption, being fixed)

Reported temps come from UDS DIDs read off the Haldex module:
- `0x028D` module temp: `payload[0] - 55` degC (simple 1-byte offset, trusted)
- `0x2BF1` clutch temp, `0x2BE4` cooling-fin temp: `(rawLE - 22767) / 100` degC
  (`Calculations.cpp:1884-1891`)

**The `(raw - 22767)/100` formula is Forbes' disassembled guess, never checked
against a known temperature, and it is almost certainly wrong.** Field evidence
(reference car): at idle all three temps read ~30; under sustained ~100% pump duty
the **fin** climbs to ~160 while clutch and module stay flat at ~30. A ~160 degC
"cooling fin" is physically impossible - that metal cannot reach boiling point from
clutch/pump heat, and the module would fault out long before. So the number is a
decode artifact, not a real temperature.

Two things make the formula suspect independently:
- It has **no ceiling**, so it can run away to 160. The only *published* Haldex temp
  decode is `((A*256)+B)/1024` (big-endian), which hard-caps at ~64 degC - the real
  Haldex fluid band. Ours is a different byte order and different math.
- `0x2BE4` is not documented anywhere public (searched: VCDS/ODX label DBs, bri3d
  VW_Flash ODX route, jazdw/vag-blocks, OpenHaldexT4, Torque PID forums). Only
  `0x2BF1` is, and it contradicts us.

**Why the desk cannot just supply the right formula:** a linear decode has two
unknowns (offset, scale). Idle=30 is one equation; a full-duty point has an unknown
true temp, so it is not a second equation. Underdetermined - you cannot solve it
from the displayed numbers alone.

**Deterministic fix (needs one drive, no thermometer):** module temp (`0x028D`) uses
a trusted simple-offset formula, so it is a reliable reference for the whole unit.
Capture the raw `0x2BE4`/`0x2BF1` bytes (now exposed as `coolingFinTempRaw` /
`clutchTempRaw` in `/api` JSON) at two different soak temperatures - e.g. a cold
start where module reads ~12, and again warm-soaked at ~30. Two (raw, trueT) pairs
solve offset and scale exactly for a linear decode. A single idle capture already
discriminates candidates: `/1024` vs `(raw-22767)/100` predict *different* raw values
for the same known ~30, so one reading tells you which is right (or that neither is).

Until validated, firmware nulls any temp outside -40..150 degC rather than publish
the impossible 160 (`uds_temp_plausible`, `API.cpp`); the UI shows "--". The raw is
always exposed for the capture. Strong prior: the corrected fin value lands under
~64 degC.

## 8. opendbc torque field definitions (the externally-proven part)

Motor_11 = `0x0A7` = 167, per commaai/opendbc `vw_mqb.dbc` (two mirrors agree):
`MO_Mom_Soll_Roh`, `MO_Mom_Ist_Summe`, `MO_Mom_Traegheit_Summe`,
`MO_Mom_Soll_gefiltert` are **10-bit little-endian, 1 Nm/LSB, offset -509**, range
-509..+509 Nm; encode `raw = Nm + 509`. The firmware BPK packer matches this exactly
- the "+509 / 1 Nm-per-bit / 10-bit" packing is verifiably correct, not magic. The
Haldex engagement frame `0x118` is NOT in public opendbc (it is on the Haldex-local
segment), so its 0.4-factor decode is internally consistent but third-party
unconfirmed.

---

## Ground truth vs assumption, at a glance

| Claim | Status |
|---|---|
| Motor_11 torque field layout (10-bit, -509 offset, +-509 Nm) | Ground truth (opendbc) |
| Haldex derives lock from torque broadcast, not a lock command | Ground truth (vendor docs) |
| Car is Gen5 0CQ 907 554 | Ground truth (platform) |
| 554K needs Fix Hunt; standard Learn inaccurate on it | External (upstream README) |
| ~60% pump PWM at full lock is normal/healthy | Ground truth (Gen5 service docs) |
| Module temp `0x028D` (`raw-55`) decode | Ground truth (simple offset, sane values) |
| Fin/clutch temp `(raw-22767)/100` decode | Assumption - disassembled guess, reads impossible 160, being fixed |
| OEM commands rear-axle Nm (BR_Vorg_Allrad_Max), not engagement % | Ground truth (frame ref) |
| bpkCeilingNm is a spoof gain; 150 = 1:1 on the reference car | Ground truth (live car) |
| "V3 pins fields at 0xFA", exact byte recipes | Fork assumption (no external spec) |
| Engagement % equals torque-split % | Unproven - almost certainly false |
| `0x118` engagement 0.4-factor decode | Fork-internal, unconfirmed |
