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