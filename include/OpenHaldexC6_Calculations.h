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