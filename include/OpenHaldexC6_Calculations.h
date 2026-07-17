#pragma once

#include <OpenHaldexC6_defs.h>

// Learn Haldex table
extern uint8_t haldexLearnTable[101];
extern bool haldexLearnTableValid;
extern volatile bool haldexLearnActive;
extern volatile bool haldexLearnCancel;
extern volatile uint8_t haldexLearnStep;
extern volatile uint8_t haldexLearnCF;

float get_lock_target_adjustment();

// Which force-mode value applies right now: 0..5 (Stock/FWD/5050/6040/7525/
// Expert) when an enabled force trigger's flag is active (priority picked by
// forceModesPriority), or -1 when no force mode applies. Used by
// get_lock_target_adjustment and by the inline gateway to detect "effective
// mode is Stock", which must mean untouched passthrough - never frame edits
// built from mirrored engagement (the stuck-at-100% feedback loop). Pure logic
// over the force-mode globals, host-testable.
int get_forced_mode_value();

// Slew one step of the lock-target rate limiter. Ramp times are milliseconds for
// a full 0<->100 travel: rising transitions take engage_ms, falling transitions
// take release_ms. 0 ms = instant in that direction (rising 0 is the historical
// instant lock-up; falling 0 is an instant release, replacing the old %/s scheme
// where release rate 0 meant "never releases" - a stuck-locked footgun on a car).
// A ramp of N ms moves 100000 * dt_s / N percent per step. Pure, host-testable.
float lock_rate_limit_step(float current, float target, uint16_t engage_ms, uint16_t release_ms, float dt_s);

// One-time migration of a persisted lock-ramp rate (%/s, the pre-ms unit) to the
// new full-travel time in milliseconds. rate <= 0 maps to 0 ms (instant);
// otherwise ms = round(100000 / rate), clamped to the 1000 ms slider ceiling
// (so a very slow legacy rate becomes the 1 s maximum). Pure, host-testable.
uint16_t lock_ramp_ms_from_rate(float rate_per_sec);
uint8_t steering_gain_percent(uint16_t angle_tenths, uint16_t start_deg, uint16_t full_deg, uint8_t floor_percent);

// Geometry-compensated per-corner wheel slip. Given the four raw ABS wheel-speed
// counts (any consistent unit; MQB 0x0B2 is 0.0075 km/h/LSB) in [FL, FR, RL, RR]
// order, the SIGNED steering-WHEEL angle in 0.1-deg units (negative = left,
// positive = right), the rack ratio (steering-wheel deg per road-wheel deg, e.g.
// ~15.6 for MQB), and the car geometry (wheelbase and front/rear track in mm),
// fill slip_out[0..3] with each corner's slip as whole-percent int8 (1%/LSB,
// clamped -100..+127). Slip is actual_i / expected_i - 1, where expected_i is the
// speed pure Ackermann geometry predicts for that corner's turn radius at the
// current steering angle, normalised so the four expected speeds share the
// measured mean. Positive => spinning faster than geometry allows (losing grip);
// negative => the reference/dragging corner. On a straight the radii are equal so
// it reduces to speed-vs-average. Returns false and zeroes slip_out when the mean
// speed is below min_speed_raw (too slow to trust) or an input is degenerate.
// LIMITATION: a relative method - if all four corners break loose equally it reads
// ~0 slip. Pure float/trig math, no Arduino/TWAI symbols, host-testable.
bool compute_corner_slip(const uint16_t wheel_raw[4], int16_t steer_wheel_tenths,
                         float steering_ratio, uint16_t wheelbase_mm,
                         uint16_t track_front_mm, uint16_t track_rear_mm,
                         uint16_t min_speed_raw, int8_t slip_out[4]);
static float get_expert_lock_target();
uint8_t get_lock_target_adjusted_value(uint8_t value, bool invert);
void getLockData(twai_message_t& rx_message_chs);
void startHaldexLearn();

// Scale a received Haldex engagement byte to a 0..100 percentage.
// Replaces a raw Arduino map(raw, in_min, in_max, 0, 100) at the CAN parse site:
// for valid in-window frames the result is identical, but raw bytes outside
// [in_min,in_max] are clamped (below -> 0, above -> 100) instead of extrapolated,
// so the uint8_t result can never wrap or exceed 100. Returns 0 if in_max<=in_min.
uint8_t scale_haldex_engagement(uint8_t raw, uint8_t in_min, uint8_t in_max);

// Learn-table lookup. Returns the smallest index i in 0..100 with
// table[i] >= target - the lowest correction factor whose learned engagement
// meets the requested lock target. When NO entry meets target (more lock
// requested than was ever learned), returns 100: clamp to the highest learned
// engagement instead of the old loop's fall-through 0 (zero lock delivered
// exactly when maximum lock is wanted). Pure array math, host-testable.
uint8_t lookup_learn_correction_factor(const uint8_t* table, uint8_t target);

// Reduce one CF settle window's burst of engagement samples into the value
// recorded in the learn table. Robust against the two artifacts seen on a live
// car during a learn scan:
//   * pump overshoot - near lock-up the Haldex ECU's own duty loop briefly
//     drives full PWM, so a single feedback frame decodes to ~100 before it
//     backs off (audible pump oscillation above ~60% engagement);
//   * CAN dropout - a momentary 0 frame.
// The old loop took ONE raw sample at the end of the settle and only glazed a 0,
// so a lone overshoot frame was written verbatim; scale_haldex_engagement clamps
// anything at/above the window top to a clean 100, and lookup_learn_correction_factor
// (smallest CF whose learned engagement meets target) then collapses the whole
// upper map onto that CF - one spike poisons the calibration.
// This takes the median of the window so a lone spike or dropout cannot move the
// recorded value, then clamps monotonic non-decreasing against prev_recorded:
// learned engagement can only rise with CF, so this both keeps the lookup
// coherent and subsumes the dropout-glaze (an all-zero window can't pull the
// recorded value below the previous CF's). n == 0 returns prev_recorded. Uses a
// lower median so an even spike/clean split still rejects the spike. Pure,
// host-testable (no Arduino/TWAI symbols).
uint8_t learn_reduce_samples(const uint8_t* samples, uint8_t n, uint8_t prev_recorded);

// MQB Motor_11 (0x0A7) packing selector. Returns true when the frame must use
// the DBC-correct BPK packing rather than the empirical V3 packing. V3 pins the
// primary torque fields (MO_Mom_Soll_Roh/Ist/gefiltert) at full 0xFA regardless
// of the applied lock, so a learn scan run with Fix Hunting OFF never modulates
// the dominant torque demand and the Haldex just locks fully the whole sweep -
// the "sits at 100% regardless" symptom, and a garbage flat learn table. BPK
// derives every field from the modulated torque, so the sweep is visible. The
// rule: BPK whenever the user enabled Fix Hunting OR a learn is active OR a valid
// learn table exists. The table one closes a silent mismatch: a learn always runs
// under BPK (learn_active forces it), so a learned table maps correction factor to
// engagement measured against BPK frames. If the user then drove with Fix Hunting
// off, get_lock_target_adjusted_value would apply that BPK-calibrated table to V3
// frame bytes - a different frame the calibration was never measured against. So
// once a table has been learned, drive BPK regardless of the toggle. An untuned
// user (no table) still gets the legacy V3 default. Pure boolean logic, no Arduino
// symbols, host-testable.
bool motor11_use_bpk_packing(bool fix_hunting, bool learn_active, bool learn_table_valid);

// MQB Motor_11 (0x0A7) BPK torque-spoof packer. Fills out[0..7] with the
// DBC-correct bit-packed engine-torque frame that provokes the Haldex to close
// the clutch: MO_Mom_Soll_Roh / MO_Mom_Ist_Summe / MO_Mom_Soll_gefiltert are
// 10-bit little-endian fields at 1 Nm/LSB with offset -509, verified against the
// opendbc MQB K-matrix (see vault "OpenHaldex - MQB Motor_11 Torque Spoof
// Verified Against opendbc"). `command` is the lock-modulated demand byte
// (get_lock_target_adjusted_value(0xFE,false)); it is remapped onto
// [BPK_FLOOR .. ceil_nm] Nm. `ceil_nm` is the per-car lock calibration: the Nm the
// frame claims at full command (not a strength dial - higher does not lock harder,
// it shifts the calibration), clamped to the 509 Nm signal maximum. `ist_nm`/`solf_nm` carry the slew-limited previous values in
// and the new values out, so the CALLER owns the ramp state across cycles (kept
// out of the function so it stays pure and host-testable). out[0] is left 0 for
// the caller to fill with the E2E CRC. No Arduino/TWAI symbols.
void bpk_pack_motor11(uint8_t out[8], uint8_t command, uint8_t counter,
                      uint16_t ceil_nm, uint16_t *ist_nm, uint16_t *solf_nm);

// Speed-disengage gate. Returns true when lock is permitted at the
// given speed: the vehicle must be at or ABOVE disengage_under AND at or BELOW
// disengage_above. A bound of 0 disables that side (0 = "no lower/upper cut").
// This replaces the previous inverted, default-defeated expression:
//   (under==0) || (speed<=under) || (speed>=above)
// which was always true because disengage_above defaults to 0 (speed>=0), and
// which disengaged lock in the wrong band. Only the passive drive modes
// (5050/6040/7525) consult this; expert mode bypasses this gate, while force mode
// (TC/ESP or external button) returns its lock value before lock_enabled() runs,
// so launch-control at a standstill still locks. Pure integer logic, no Arduino
// symbols, host-testable.
bool speed_disengage_ok(uint16_t speed, uint16_t disengage_under, uint16_t disengage_above);

// Hysteretic (Schmitt-trigger) form of the speed-disengage gate. speed_disengage_ok
// is a bare comparison with no memory, so a speed resting on disengage_under or
// disengage_above and dithering by one unit flips the gate every frame - the lock
// target then hunts 0 <-> target frame-to-frame (see getLockData). This adds a
// deadband: entry is sharp (identical to speed_disengage_ok), but once enabled the
// gate only drops out after speed moves `hysteresis` units PAST the bound
// (speed < disengage_under - hysteresis, or speed > disengage_above + hysteresis).
// Asymmetric on purpose: engage promptly, be reluctant to disengage, so sensor
// noise at a steady cruise cannot chatter the clutch. hysteresis 0 reduces exactly
// to speed_disengage_ok regardless of currently_enabled, so a zero band is a
// behaviour-preserving no-op. currently_enabled is the gate's previous output.
// A bound of 0 disables that side (no cut), matching speed_disengage_ok. The band
// width is a bench-tuned calibration against real speed-signal noise, so this seam
// is proven here but left unwired into getLockData until it can be tuned on the car.
// Pure integer logic, no Arduino symbols, host-testable.
bool disengage_gate_hysteresis(uint16_t speed, uint16_t disengage_under, uint16_t disengage_above,
                               uint16_t hysteresis, bool currently_enabled);

// Integrating debounce for a force-mode CAN flag (TC/ESP "passiv", hazard lights,
// external button). Today these are read with a bare bitRead every chassis frame
// (OpenHaldexC6_can.cpp:335/355/385/443) with no memory, so a single-frame edge -
// a one-frame ESP "passiv" blip, a stray hazard bit - flips force mode for exactly
// as long as the source bit is set, and (with the release ramp off, the default)
// snaps the lock target for that cycle unfiltered. This requires a raw edge to
// persist for `threshold` consecutive frames before it is accepted; a shorter blip
// is rejected and the debounced state holds. `counter` is caller-owned scratch
// state (one per flag) counting consecutive frames the raw bit has disagreed with
// the debounced state; it is reset to 0 whenever raw agrees, so a blip must be
// sustained, not merely cumulative. threshold <= 1 accepts every edge immediately,
// reducing exactly to today's undebounced bitRead, so wiring this in with a default
// threshold of 0 is a behaviour-preserving no-op. The frame count needed to reject
// real-world bus noise without adding felt latency to a genuine ESP/hazard event is
// a bench-tuned calibration, so this seam is proven here but left unwired until it
// can be tuned on the car. Pure boolean/counter logic, no Arduino/TWAI symbols,
// host-testable.
bool debounce_force_flag(bool raw, bool debounced, uint8_t &counter, uint8_t threshold);

// True when arr[0..count-1] is strictly ascending (each element greater than the
// previous). The expert 2D map (get_expert_lock_target) assumes ascending speed
// and throttle axes; a non-monotonic axis makes the interpolation bracket search
// pick the wrong pair and silently mis-interpolate. tuneIncoming rejects a tune
// whose axes fail this. count 0 or 1 is vacuously ascending. Pure, host-testable.
bool is_strictly_ascending_u16(const uint16_t* arr, uint8_t count);

// Auth policy. The WiFi AP password is the single auth boundary: anyone who can
// reach the web server or analyzer port has already joined the WPA2-protected AP.
//
// wifi_password_provisioned: true only when a WPA2-length AP password (>= 8
// chars) is set. Until then the AP runs open and the dashboard forces the
// first-run password page. This same predicate gates the fail-closed
// analyzer-injection policy: host->device CAN transmit on the analyzer port is
// authorized only when the AP is protected; passive sniffing is unaffected.
// Pure pointer logic, no Arduino/NVS/TWAI symbols, so the decision lives in one
// host-tested place.
bool wifi_password_provisioned(const char* ap_pw);

// Pure CAN bus-health predicates. Plain arithmetic ((alerts & mask) != 0), no
// TWAI driver symbols, so the always-on failure/recovery decision that drives
// isBusFailure is host-testable. can_alerts_indicate_failure: true when any
// failure bit is set. can_alerts_indicate_recovered: true when a bus that went
// off has finished recovery and can be restarted with twai_start_v2.
bool can_alerts_indicate_failure(uint32_t alerts, uint32_t failure_mask);
bool can_alerts_indicate_recovered(uint32_t alerts, uint32_t recovered_mask);

// External-diagnostic-tool detection. Pure integer logic, no Arduino/TWAI
// symbols, so the auto-pause decision that keeps our UDS polling off a busy
// diagnostic bus is host-testable.
// is_external_diag_request_id: true when a chassis-bus (Bus 0) CAN id is an
// ISO/VAG diagnostic tester request - 0x7DF (OBD-II functional request) or the
// 0x700-0x71F physical-tester range. Our own UDS polling transmits on Bus 1, so
// any of these seen inbound on Bus 0 means a real scan tool (VCDS/ODIS/OBD) is
// on the bus. TP2.0 setup id 0x200 is deliberately excluded: this fork does not
// run TP2.0, and 0x200 sits in the normal MQB broadcast-data id region where it
// would false-trigger.
bool is_external_diag_request_id(uint32_t can_id);
// external_diag_stamp: normalize a millis() reading for storage so 0 stays
// reserved as the "never seen" sentinel. millis() legitimately returns 0 at boot
// and at every 32-bit rollover; a raw 0 stored as last_seen would be misread as
// "never seen". Maps only the single 0 tick to 1 (<=1 ms error); all other
// values pass through. The stamp site must use this before writing last_seen_ms.
uint32_t external_diag_stamp(uint32_t now_ms);
// external_diag_active: true while a tester was seen within timeout_ms.
// last_seen_ms == 0 means never seen (never a real stamp - see external_diag_stamp).
// Uses wrap-safe unsigned subtraction so it survives the millis() rollover.
bool external_diag_active(uint32_t last_seen_ms, uint32_t now_ms, uint32_t timeout_ms);

// NVS init policy. Pure decision over two booleans - no Arduino/NVS symbols -
// so the readEEP first-run/migrate/seed branch lives in one host-testable place.
// Given whether the canonical namespace is already seeded and whether the
// de-facto legacy namespace still holds a previous device's data:
//   * new_ns_seeded            -> EEP_LOAD_EXISTING (normal run, just load)
//   * legacy data, not seeded  -> EEP_MIGRATE_LEGACY (copy legacy -> new, mark seeded)
//   * neither                  -> EEP_SEED_DEFAULTS  (first ever run, write defaults)
enum EepInitAction { EEP_LOAD_EXISTING, EEP_MIGRATE_LEGACY, EEP_SEED_DEFAULTS };
EepInitAction eeprom_init_action(bool new_ns_seeded, bool legacy_ns_has_data);

// HTTP request-body buffer ownership, extracted from parseJSON so the
// malloc-owned single-block contract that ESPAsyncWebServer frees with plain
// free() can be host-tested. Both reference only <cstdlib>/<cstring>, no
// Arduino/Async symbols, so they compile and test under env:native.
//
// http_body_alloc: returns a malloc(total + 1) block, fully zero-filled with a
// NUL guaranteed at index `total`. Returns nullptr when total == 0 or malloc
// fails, so the caller can fall through to its empty-body path. One allocation,
// released by a single plain free().
char* http_body_alloc(size_t total);

// http_body_write_chunk: bounds-checked copy of `len` bytes from `data` into
// `buf` at offset `index`, never writing past `total` (the NUL guard at `total`
// is preserved). No-ops when buf is null, data is null, index >= total, or len
// is zero; a chunk that would overrun `total` is truncated to fit.
void http_body_write_chunk(char* buf, const uint8_t* data, size_t len, size_t index, size_t total);

// ---- Gen5 (MQB) Haldex UDS live data ---------------------------------------
// Wire format and per-DID scaling recovered from the upstream V8.00.2 binary and
// confirmed against the author's V8.00.2 source. Both functions are pure
// byte/float arithmetic (no TWAI/Arduino symbols), so the decode feeding the
// dashboard live-data card is host-testable.
//
// uds_parse_sf_rdbi: parse an ISO-TP single-frame positive ReadDataByIdentifier
// response for `did` out of a raw CAN payload. Returns the number of data bytes
// (those after the 62 <DID_hi> <DID_lo> header) copied into `out`, or -1 when the
// frame is anything else: not a single frame, a declared length the frame does
// not actually carry, a negative response, a different service or DID, or more
// data than `out_cap` can hold. All poller responses are single frames.
int uds_parse_sf_rdbi(const uint8_t *data, uint8_t dlc, uint16_t did, uint8_t *out, uint8_t out_cap);

// uds_scale_mqb_did: apply the per-DID raw->engineering-value scaling. The u16
// byte order is mixed per DID, matching the upstream poller: temperatures
// (0x2BF1, 0x2BE4) are little-endian, clutch current/voltage (0x2BE6, 0x2BE9) are
// big-endian. Returns false for a short payload or an unknown DID, leaving `out`
// untouched.
bool uds_scale_mqb_did(uint16_t did, const uint8_t *payload, uint8_t len, float &out);

// uds_temp_plausible: true when a decoded Haldex temperature is within a
// physically possible band (-40..150 °C). The 0x2BE4/0x2BF1 scale is an
// unvalidated disassembled guess that reads an impossible ~160 °C on the fin
// under load; callers null the display when this returns false rather than
// showing a value the hardware cannot produce.
bool uds_temp_plausible(float degC);