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
float lock_rate_limit_step(float current, float target, float engage_rate_per_sec, float release_rate_per_sec, float dt_s);
uint8_t steering_gain_percent(uint16_t angle_tenths, uint16_t start_deg, uint16_t full_deg, uint8_t floor_percent);
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

// True when arr[0..count-1] is strictly ascending (each element greater than the
// previous). The expert 2D map (get_expert_lock_target) assumes ascending speed
// and throttle axes; a non-monotonic axis makes the interpolation bracket search
// pick the wrong pair and silently mis-interpolate. tuneIncoming rejects a tune
// whose axes fail this. count 0 or 1 is vacuously ascending. Pure, host-testable.
bool is_strictly_ascending_u16(const uint16_t* arr, uint8_t count);

// OTA credential policy. Pure pointer logic, no Arduino/NVS symbols, so the
// flash-authorization decision is a single host-testable definition.
//
// select_ota_password: resolves the effective OTA password from the runtime
// NVS-provisioned value; an empty ("") or NULL value is treated as "unset".
// Returns "" when no non-empty credential is stored, so the caller fails closed
// rather than falling back to a hardcoded literal. There is no build-time
// default - a credential is only ever set at runtime via /setup.
const char* select_ota_password(const char* nvs_pw);

// ota_credential_configured: true only when the effective password is non-NULL
// and non-empty. The flash surface must refuse (HTTP 503, fail-closed) whenever
// this is false.
bool ota_credential_configured(const char* effective_pw);

// analyzer_injection_allowed: single host-testable decision for the fail-closed
// analyzer-injection policy, delegating to ota_credential_configured. host->device
// CAN transmit on the analyzer port is authorized only when a credential is set;
// the analyzer transmit paths MUST drop the frame otherwise (passive sniffing is
// unaffected). Pure pointer logic, no Arduino/NVS/TWAI symbols.
bool analyzer_injection_allowed(const char* effective_pw);

// Pure CAN bus-health predicates. Plain arithmetic ((alerts & mask) != 0), no
// TWAI driver symbols, so the always-on failure/recovery decision that drives
// isBusFailure is host-testable. can_alerts_indicate_failure: true when any
// failure bit is set. can_alerts_indicate_recovered: true when a bus that went
// off has finished recovery and can be restarted with twai_start_v2.
bool can_alerts_indicate_failure(uint32_t alerts, uint32_t failure_mask);
bool can_alerts_indicate_recovered(uint32_t alerts, uint32_t recovered_mask);

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