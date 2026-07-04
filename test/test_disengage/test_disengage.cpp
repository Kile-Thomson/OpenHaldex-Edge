// Characterization tests for the speed-disengage gate.
//
// Pins the behaviour of speed_disengage_ok(speed, under, above) in
// src/OpenHaldexC6_Calculations.cpp, which lock_enabled() consults for the
// passive drive modes (5050/6040/7525); expert mode bypasses this gate. Lock is
// permitted only while the vehicle speed is at or ABOVE `under` AND at or BELOW
// `above`; a bound of 0 disables that side.
//
// The old expression was
//   (under==0) || (speed<=under) || (speed>=above)
// which (a) was ALWAYS true because `above` defaults to 0 (speed>=0), so setting
// only an under-speed cut did nothing, and (b) disengaged lock in the wrong band.
// The regression test below (under set, above=0) proves the under-cut now bites.
//
// Force mode is out of scope here: get_lock_target_adjustment() returns the forced
// lock value before lock_enabled() runs, so launch-control at a standstill is not
// gated by this predicate.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Calculations.h>

extern bool speed_disengage_ok(uint16_t speed, uint16_t disengage_under, uint16_t disengage_above);

void setUp(void) {}
void tearDown(void) {}

// ---- feature off: both bounds 0 -> always permitted ----------------------
void test_both_bounds_zero_always_ok(void)
{
  TEST_ASSERT_TRUE_MESSAGE(speed_disengage_ok(0, 0, 0), "0/0/0 -> ok");
  TEST_ASSERT_TRUE_MESSAGE(speed_disengage_ok(120, 0, 0), "120/0/0 -> ok");
  TEST_ASSERT_TRUE_MESSAGE(speed_disengage_ok(300, 0, 0), "300/0/0 -> ok");
}

// ---- REGRESSION: only under-speed cut set (above defaults to 0) -----------
// This is the exact case the old code defeated. Below `under` -> not ok;
// at/above `under` -> ok, regardless of the (unset) upper bound.
void test_only_under_cut_bites(void)
{
  TEST_ASSERT_FALSE_MESSAGE(speed_disengage_ok(5, 10, 0), "5 < under=10 -> not ok");
  TEST_ASSERT_FALSE_MESSAGE(speed_disengage_ok(9, 10, 0), "9 < under=10 -> not ok");
  TEST_ASSERT_TRUE_MESSAGE(speed_disengage_ok(10, 10, 0), "10 == under -> ok (boundary)");
  TEST_ASSERT_TRUE_MESSAGE(speed_disengage_ok(50, 10, 0), "50 >= under=10 -> ok");
  TEST_ASSERT_TRUE_MESSAGE(speed_disengage_ok(300, 10, 0), "300 >= under=10 -> ok");
}

// ---- only above-speed cut set (under = 0) --------------------------------
void test_only_above_cut(void)
{
  TEST_ASSERT_TRUE_MESSAGE(speed_disengage_ok(0, 0, 160), "0 <= above=160 -> ok");
  TEST_ASSERT_TRUE_MESSAGE(speed_disengage_ok(160, 0, 160), "160 == above -> ok (boundary)");
  TEST_ASSERT_FALSE_MESSAGE(speed_disengage_ok(161, 0, 160), "161 > above=160 -> not ok");
  TEST_ASSERT_FALSE_MESSAGE(speed_disengage_ok(300, 0, 160), "300 > above=160 -> not ok");
}

// ---- both bounds: engaged only inside the window [under, above] -----------
void test_window_between_bounds(void)
{
  const uint16_t under = 10, above = 160;
  TEST_ASSERT_FALSE_MESSAGE(speed_disengage_ok(5, under, above), "below window -> not ok");
  TEST_ASSERT_TRUE_MESSAGE(speed_disengage_ok(10, under, above), "lower boundary -> ok");
  TEST_ASSERT_TRUE_MESSAGE(speed_disengage_ok(80, under, above), "inside window -> ok");
  TEST_ASSERT_TRUE_MESSAGE(speed_disengage_ok(160, under, above), "upper boundary -> ok");
  TEST_ASSERT_FALSE_MESSAGE(speed_disengage_ok(200, under, above), "above window -> not ok");
}

// ---- inverted window (under > above, both active) -> gate false everywhere -
// With both bounds active but lower > upper the gate can never be satisfied, so
// passive-mode lock would never engage at any speed. The API rejects that
// ordering at the settings boundary for exactly this reason.
void test_inverted_window_never_ok(void)
{
  const uint16_t under = 160, above = 10; // inverted
  TEST_ASSERT_FALSE_MESSAGE(speed_disengage_ok(0, under, above), "inverted: 0 -> not ok");
  TEST_ASSERT_FALSE_MESSAGE(speed_disengage_ok(80, under, above), "inverted: 80 -> not ok");
  TEST_ASSERT_FALSE_MESSAGE(speed_disengage_ok(160, under, above), "inverted: 160 -> not ok");
  TEST_ASSERT_FALSE_MESSAGE(speed_disengage_ok(300, under, above), "inverted: 300 -> not ok");
}

int main(int, char **)
{
  UNITY_BEGIN();
  RUN_TEST(test_both_bounds_zero_always_ok);
  RUN_TEST(test_only_under_cut_bites);
  RUN_TEST(test_only_above_cut);
  RUN_TEST(test_window_between_bounds);
  RUN_TEST(test_inverted_window_never_ok);
  return UNITY_END();
}
