// Steering lock taper. v9 has one implementation: the 5-point breakpoint curve
// (steering_curve_percent). Edge's three-knob taper (start / full / floor) is a
// front door onto it: steering_curve_from_taper builds the curve and
// steering_taper_from_curve reads the three knobs back for display.
//
// The golden values below are the ones Edge's steering_gain_percent() produced
// (100% up to start, straight line to floor at full, floor beyond), so the
// settings a v8 user had still mean the same thing. A red assertion here means a
// refactor changed the taper the Haldex sees - do NOT "fix" a golden to make a
// refactor pass.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Calculations.h>

void setUp(void) {}
void tearDown(void) {}

static uint16_t arr[steeringArrayCount];
static uint8_t scl[steeringArrayCount];

static int gain(float deg_angle) // percent, rounded like the lock path rounds
{
  return (int)(steering_curve_percent(deg_angle, arr, scl, steeringArrayCount) + 0.5f);
}

// v8 defaults: start 45 deg, full 180 deg, floor 50%.
static void v8_defaults(void) { steering_curve_from_taper(45, 180, 50, arr, scl); }

void test_default_v9_curve_points(void)
{
  const uint16_t a[5] = {0, 45, 90, 180, 360};
  const uint8_t s[5] = {100, 100, 80, 50, 20};
  TEST_ASSERT_EQUAL_FLOAT(100.0f, steering_curve_percent(0, a, s, 5));
  TEST_ASSERT_EQUAL_FLOAT(100.0f, steering_curve_percent(45, a, s, 5));
  TEST_ASSERT_EQUAL_FLOAT(80.0f, steering_curve_percent(90, a, s, 5));
  TEST_ASSERT_EQUAL_FLOAT(50.0f, steering_curve_percent(180, a, s, 5));
  TEST_ASSERT_EQUAL_FLOAT(20.0f, steering_curve_percent(360, a, s, 5));
  TEST_ASSERT_EQUAL_FLOAT(20.0f, steering_curve_percent(720, a, s, 5));   // past the end holds the last value
  TEST_ASSERT_EQUAL_FLOAT(90.0f, steering_curve_percent(67.5f, a, s, 5)); // midpoint of 45..90
}

void test_taper_at_or_below_start_is_full_gain(void)
{
  v8_defaults();
  TEST_ASSERT_EQUAL_INT_MESSAGE(100, gain(0), "0 deg -> 100%");
  TEST_ASSERT_EQUAL_INT_MESSAGE(100, gain(20), "20 deg -> 100%");
  TEST_ASSERT_EQUAL_INT_MESSAGE(100, gain(45), "45 deg (start) -> 100%");
}

void test_taper_ramp_between_breakpoints(void)
{
  // Linear 45..180 deg over 100% -> 50%. The curve's middle breakpoint sits at the
  // midpoint, so the line is exact at start, midpoint and full.
  v8_defaults();
  TEST_ASSERT_EQUAL_INT_MESSAGE(75, gain(112.5f), "112.5 deg (midpoint) -> 75%");
  TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.2f, 87.5f, steering_curve_percent(78.75f, arr, scl, steeringArrayCount),
                                   "78.75 deg (quarter) -> 87.5%");
}

void test_taper_at_or_above_full_is_floor(void)
{
  v8_defaults();
  TEST_ASSERT_EQUAL_INT_MESSAGE(50, gain(180), "180 deg (full) -> floor");
  TEST_ASSERT_EQUAL_INT_MESSAGE(50, gain(540), "540 deg -> floor");
  TEST_ASSERT_EQUAL_INT_MESSAGE(50, gain(720), "720 deg -> floor");
}

void test_taper_floor_zero_can_taper_fully_open(void)
{
  steering_curve_from_taper(45, 180, 0, arr, scl);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, gain(180), "floor 0 -> 0% at full");
  TEST_ASSERT_EQUAL_INT_MESSAGE(50, gain(112.5f), "floor 0 -> 50% at midpoint");
}

void test_taper_floor_above_100_is_clamped(void)
{
  // Defensive: an out-of-range floor behaves as 100% (no taper), never wraps.
  steering_curve_from_taper(45, 180, 250, arr, scl);
  TEST_ASSERT_EQUAL_INT_MESSAGE(100, gain(180), "floor 250 -> clamped to 100");
  TEST_ASSERT_EQUAL_INT_MESSAGE(100, gain(720), "floor 250 -> still 100 beyond");
}

void test_taper_degenerate_window_steps_to_floor(void)
{
  // full <= start: no ramp span. Past the start angle the gain steps to the floor
  // within a degree, never a divide by zero.
  steering_curve_from_taper(45, 45, 50, arr, scl);
  TEST_ASSERT_EQUAL_INT_MESSAGE(100, gain(45), "at start -> 100% even when full==start");
  TEST_ASSERT_EQUAL_INT_MESSAGE(50, gain(47), "past start, full==start -> floor");
  steering_curve_from_taper(100, 45, 50, arr, scl); // full < start
  TEST_ASSERT_EQUAL_INT_MESSAGE(100, gain(100), "full < start: still 100% at start");
  TEST_ASSERT_EQUAL_INT_MESSAGE(50, gain(120), "full < start -> floor past start");
}

void test_curve_is_never_above_100_or_below_0(void)
{
  v8_defaults();
  for (int d = 0; d <= 720; d += 5)
  {
    const float g = steering_curve_percent((float)d, arr, scl, steeringArrayCount);
    TEST_ASSERT_TRUE(g >= 0.0f && g <= 100.0f);
  }
}

void test_taper_round_trips_through_the_curve(void)
{
  uint16_t st, fu;
  uint8_t fl;
  steering_curve_from_taper(60, 200, 35, arr, scl);
  steering_taper_from_curve(arr, scl, steeringArrayCount, st, fu, fl);
  TEST_ASSERT_EQUAL_UINT16(60, st);
  TEST_ASSERT_EQUAL_UINT16(200, fu);
  TEST_ASSERT_EQUAL_UINT8(35, fl);

  steering_curve_from_taper(45, 180, 50, arr, scl); // v8 defaults
  steering_taper_from_curve(arr, scl, steeringArrayCount, st, fu, fl);
  TEST_ASSERT_EQUAL_UINT16(45, st);
  TEST_ASSERT_EQUAL_UINT16(180, fu);
  TEST_ASSERT_EQUAL_UINT8(50, fl);
}

int main(int, char **)
{
  UNITY_BEGIN();
  RUN_TEST(test_default_v9_curve_points);
  RUN_TEST(test_taper_at_or_below_start_is_full_gain);
  RUN_TEST(test_taper_ramp_between_breakpoints);
  RUN_TEST(test_taper_at_or_above_full_is_floor);
  RUN_TEST(test_taper_floor_zero_can_taper_fully_open);
  RUN_TEST(test_taper_floor_above_100_is_clamped);
  RUN_TEST(test_taper_degenerate_window_steps_to_floor);
  RUN_TEST(test_curve_is_never_above_100_or_below_0);
  RUN_TEST(test_taper_round_trips_through_the_curve);
  return UNITY_END();
}
