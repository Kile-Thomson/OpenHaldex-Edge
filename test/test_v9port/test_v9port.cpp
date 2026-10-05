// Tests for the v9 merge decisions: places where upstream's v9 behaviour and
// Edge's v8 behaviour were reconciled. Each test states which side it pins.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Calculations.h>

extern float get_lock_target_adjustment();
extern uint8_t get_lock_target_adjusted_value(uint8_t value, bool invert);

static void reset(void)
{
  state.mode = MODE_5050;
  state.pedal_threshold = 0;
  extBtnForceMode = tcForceMode = hazardForceMode = false;
  extButtonForceModeFlag = tcForceModeFlag = hazardForceModeFlag = false;
  tcForceModeValue = hazardForceModeValue = extBtnForceModeValue = 2;
  forceModesPriority = 0;
  disengageUnderSpeed = 0;
  disengageAboveSpeed = 0;
  received_pedal_value = 50.0f;
  received_vehicle_speed = 50;
  received_haldex_engagement = 77; // must never leak into the command
  haldexGeneration = 50;
  steeringScaleEnabled = false;
  haldexLearnTableValid = false;
  haldexLearnActive = false;
  lock_target = 0.0f;
}

void setUp(void) { reset(); }
void tearDown(void) {}

// --- lock ramp: v9 %/s keys (web + BLE) map onto the one ms ramp -------------

void test_pct_rate_maps_to_ms(void)
{
  TEST_ASSERT_EQUAL_UINT16(200, lock_ramp_ms_from_pct_rate(500));   // fastest the BLE contract allows
  TEST_ASSERT_EQUAL_UINT16(1000, lock_ramp_ms_from_pct_rate(100));
  TEST_ASSERT_EQUAL_UINT16(833, lock_ramp_ms_from_pct_rate(120));   // the v9 default
  TEST_ASSERT_EQUAL_UINT16(20000, lock_ramp_ms_from_pct_rate(5));   // slowest
}

void test_pct_rate_is_clamped_to_the_ble_range(void)
{
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(20000, lock_ramp_ms_from_pct_rate(0), "0 clamps to 5 %/s, never divides by zero");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(200, lock_ramp_ms_from_pct_rate(60000), "huge rate clamps to 500 %/s");
}

void test_ramp_ms_reports_back_as_rate(void)
{
  TEST_ASSERT_EQUAL_UINT16(500, lock_pct_rate_from_ramp_ms(0));     // instant reads as the ceiling
  TEST_ASSERT_EQUAL_UINT16(100, lock_pct_rate_from_ramp_ms(1000));
  TEST_ASSERT_EQUAL_UINT16(500, lock_pct_rate_from_ramp_ms(200));
  TEST_ASSERT_EQUAL_UINT16(5, lock_pct_rate_from_ramp_ms(60000));   // clamps, never 0 %/s
}

void test_rate_round_trips_within_a_percent(void)
{
  for (uint16_t rate = 5; rate <= 500; rate += 7)
  {
    const uint16_t back = lock_pct_rate_from_ramp_ms(lock_ramp_ms_from_pct_rate(rate));
    const int d = (int)back - (int)rate;
    TEST_ASSERT_TRUE_MESSAGE(d >= -2 && d <= 2, "rate -> ms -> rate stays within rounding");
  }
}

void test_slow_release_ramp_slews_by_ms(void)
{
  // A 5 %/s release (20 s) falls 0.5 %% in 100 ms, never snaps open.
  const float next = lock_rate_limit_step(100.0f, 0.0f, 0, lock_ramp_ms_from_pct_rate(5), 0.1f);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 99.5f, next);
}

// --- speed window applies to every lock path (upstream v9 behaviour) ---------
// Edge v8 let Expert and force modes bypass it; v9 gates all of them, so a
// disengage-under-speed cut-off holds regardless of how lock was requested.

void test_window_gates_passive_mode(void)
{
  disengageUnderSpeed = 30;
  received_vehicle_speed = 10;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, get_lock_target_adjustment());
  received_vehicle_speed = 40;
  TEST_ASSERT_TRUE(get_lock_target_adjustment() > 0.0f);
}

void test_window_gates_expert_mode(void)
{
  state.mode = MODE_EXPERT;
  disengageAboveSpeed = 100;
  received_vehicle_speed = 150;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, get_lock_target_adjustment());
}

void test_window_gates_forced_mode(void)
{
  tcForceMode = true;
  tcForceModeFlag = true;
  tcForceModeValue = MODE_5050;
  disengageUnderSpeed = 30;
  received_vehicle_speed = 5;
  TEST_ASSERT_EQUAL_FLOAT(0.0f, get_lock_target_adjustment());
}

// --- Stock never mirrors engagement back as a command ------------------------

void test_stock_command_is_zero_not_mirrored_engagement(void)
{
  state.mode = MODE_STOCK;
  received_haldex_engagement = 77;
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, get_lock_target_adjustment(), "Stock as a command is zero forced lock");
  TEST_ASSERT_EQUAL_INT_MESSAGE(-1, get_forced_mode_value(), "no trigger -> no forced mode");
}

void test_forced_stock_wins_over_base_mode(void)
{
  // Effective mode Stock (trigger configured to Stock) must read as forced 0 so the
  // gateway passes frames through untouched.
  state.mode = MODE_5050;
  extBtnForceMode = true;
  extButtonForceModeFlag = true;
  extBtnForceModeValue = MODE_STOCK;
  TEST_ASSERT_EQUAL_INT(0, get_forced_mode_value());
}

void test_forced_mode_needs_both_feature_and_flag(void)
{
  tcForceMode = false; // feature off
  tcForceModeFlag = true; // stray flag
  TEST_ASSERT_EQUAL_INT(-1, get_forced_mode_value());
  tcForceMode = true;
  tcForceModeFlag = false; // feature on, flag clear
  TEST_ASSERT_EQUAL_INT(-1, get_forced_mode_value());
}

int main(int, char **)
{
  UNITY_BEGIN();
  RUN_TEST(test_pct_rate_maps_to_ms);
  RUN_TEST(test_pct_rate_is_clamped_to_the_ble_range);
  RUN_TEST(test_ramp_ms_reports_back_as_rate);
  RUN_TEST(test_rate_round_trips_within_a_percent);
  RUN_TEST(test_slow_release_ramp_slews_by_ms);
  RUN_TEST(test_window_gates_passive_mode);
  RUN_TEST(test_window_gates_expert_mode);
  RUN_TEST(test_window_gates_forced_mode);
  RUN_TEST(test_stock_command_is_zero_not_mirrored_engagement);
  RUN_TEST(test_forced_stock_wins_over_base_mode);
  RUN_TEST(test_forced_mode_needs_both_feature_and_flag);
  return UNITY_END();
}
