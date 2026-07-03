// Characterization tests for the steering-gain taper.
//
// Pins steering_gain_percent() in src/OpenHaldexC6_Calculations.cpp: the
// percentage (floor..100) getLockData scales the lock target by for a given
// steering angle. 100% at or below start_deg, linear ramp down to
// floor_percent at full_deg, floor at or above it. Angles are passed in
// 0.1-degree wire units. A red assertion here means a refactor changed the
// taper the Haldex sees - do NOT "fix" a golden to make a refactor pass.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Calculations.h>

void setUp(void) {}
void tearDown(void) {}

// Default settings: start 45 deg, full 180 deg, floor 50%.

void test_at_or_below_start_is_full_gain(void)
{
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(100, steering_gain_percent(0, 45, 180, 50),    "0 deg -> 100%");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(100, steering_gain_percent(200, 45, 180, 50),  "20 deg -> 100%");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(100, steering_gain_percent(450, 45, 180, 50),  "45 deg (start) -> 100%");
}

void test_ramp_between_breakpoints(void)
{
  // Linear ramp 45..180 deg over 100% -> 50%, rounded to nearest percent.
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(83, steering_gain_percent(900, 45, 180, 50),   "90 deg -> 83%");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(75, steering_gain_percent(1125, 45, 180, 50),  "112.5 deg (midpoint) -> 75%");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(61, steering_gain_percent(1500, 45, 180, 50),  "150 deg -> 61%");
}

void test_at_or_above_full_is_floor(void)
{
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(50, steering_gain_percent(1800, 45, 180, 50),  "180 deg (full) -> floor");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(50, steering_gain_percent(5400, 45, 180, 50),  "540 deg -> floor");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(50, steering_gain_percent(7200, 45, 180, 50),  "720 deg (clamp) -> floor");
}

void test_floor_zero_can_taper_fully_open(void)
{
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, steering_gain_percent(1800, 45, 180, 0),    "floor 0 -> 0% at full");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(50, steering_gain_percent(1125, 45, 180, 0),   "floor 0 -> 50% at midpoint");
}

void test_floor_above_100_is_clamped(void)
{
  // Defensive: an out-of-range floor behaves as 100% (no taper), never wraps.
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(100, steering_gain_percent(1800, 45, 180, 250), "floor 250 -> clamped to 100%");
}

void test_degenerate_window_steps_to_floor(void)
{
  // full <= start: no ramp span. Above the start angle the gain steps straight
  // to the floor (steering_gain_percent's divide-by-zero guard), never a crash.
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(100, steering_gain_percent(450, 45, 45, 50),   "at start -> 100% even when full==start");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(50, steering_gain_percent(451, 45, 45, 50),    "past start, full==start -> floor");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(50, steering_gain_percent(1200, 100, 45, 50),  "full < start -> floor past start");
}

int main(int, char **)
{
  UNITY_BEGIN();
  RUN_TEST(test_at_or_below_start_is_full_gain);
  RUN_TEST(test_ramp_between_breakpoints);
  RUN_TEST(test_at_or_above_full_is_floor);
  RUN_TEST(test_floor_zero_can_taper_fully_open);
  RUN_TEST(test_floor_above_100_is_clamped);
  RUN_TEST(test_degenerate_window_steps_to_floor);
  return UNITY_END();
}
