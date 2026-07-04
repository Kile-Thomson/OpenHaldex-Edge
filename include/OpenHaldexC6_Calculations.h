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