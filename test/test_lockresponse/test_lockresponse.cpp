// Characterization tests for the lock-target rate limiter.
//
// Pins lock_rate_limit_step() in src/OpenHaldexC6_Calculations.cpp, which
// getLockData uses to slew the lock target. Ramp times are milliseconds for a
// full 0<->100 travel: rising transitions take engage_ms, falling transitions
// take release_ms, and 0 ms = instant in that direction. These values reach the
// wire bytes the Haldex sees, so a red assertion here means a refactor changed
// the ramp behaviour - do NOT "fix" a golden to make a refactor pass.
//
// Also pins lock_ramp_ms_from_rate(), the one-time migration from the pre-ms
// %/s unit that older firmware persisted.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Calculations.h>

void setUp(void) {}
void tearDown(void) {}

// ---- rising (engage / attack) ---------------------------------------------

void test_rise_engage_zero_is_instant(void)
{
  // Engage 0 ms preserves the historical instant lock-up.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(100.0f, lock_rate_limit_step(0.0f, 100.0f, 0, 500, 0.1f),
                                  "engage=0ms -> rise instant");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, lock_rate_limit_step(10.0f, 40.0f, 0, 500, 0.0f),
                                  "engage=0ms, dt=0 -> rise instant");
}

void test_rise_is_rate_limited(void)
{
  // 1000 ms full travel over 0.1 s allows a 10-point rise (100000*0.1/1000).
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(10.0f, lock_rate_limit_step(0.0f, 100.0f, 1000, 500, 0.1f),
                                  "engage=1000ms, dt=0.1 -> +10");
  // 2000 ms full travel over 0.1 s allows a 5-point rise.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(35.0f, lock_rate_limit_step(30.0f, 100.0f, 2000, 500, 0.1f),
                                  "engage=2000ms, dt=0.1 -> +5");
}

void test_rise_clamps_at_target(void)
{
  // A step that would overshoot lands exactly on the target.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, lock_rate_limit_step(35.0f, 40.0f, 1000, 500, 0.1f),
                                  "rise within one step -> target");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(100.0f, lock_rate_limit_step(0.0f, 100.0f, 1000, 500, 5.0f),
                                  "long dt -> clamped at target");
}

void test_rise_dt_zero_holds(void)
{
  // No elapsed time, nonzero engage time -> no movement.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(20.0f, lock_rate_limit_step(20.0f, 100.0f, 1000, 500, 0.0f),
                                  "engage>0ms, dt=0 -> hold");
}

// ---- falling (release) ------------------------------------------------------

void test_fall_is_rate_limited(void)
{
  // 1000 ms full travel over 0.1 s allows a 10-point drop.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(90.0f, lock_rate_limit_step(100.0f, 0.0f, 0, 1000, 0.1f),
                                  "release=1000ms, dt=0.1 -> -10");
  // 2000 ms full travel over 0.1 s allows a 5-point drop.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(95.0f, lock_rate_limit_step(100.0f, 0.0f, 0, 2000, 0.1f),
                                  "release=2000ms, dt=0.1 -> -5");
}

void test_fall_clamps_at_target(void)
{
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, lock_rate_limit_step(45.0f, 40.0f, 0, 1000, 0.1f),
                                  "fall within one step -> target");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, lock_rate_limit_step(100.0f, 0.0f, 0, 1000, 5.0f),
                                  "long dt -> clamped at target");
}

void test_fall_release_zero_is_instant(void)
{
  // NEW semantic: release 0 ms means release immediately. The old %/s scheme
  // floored release at a nonzero rate because rate 0 meant "never releases" - a
  // stuck-locked footgun. In ms, 0 cleanly snaps open, so the step returns the
  // target in one call.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, lock_rate_limit_step(100.0f, 0.0f, 0, 0, 0.1f),
                                  "release=0ms -> instant release");
}

// ---- steady state -----------------------------------------------------------

void test_equal_returns_target(void)
{
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, lock_rate_limit_step(40.0f, 40.0f, 1000, 500, 0.1f),
                                  "current == target -> target");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, lock_rate_limit_step(0.0f, 0.0f, 0, 500, 0.1f),
                                  "both zero -> zero");
}

// ---- legacy %/s -> ms migration --------------------------------------------

void test_ramp_ms_from_rate_instant(void)
{
  // Rate 0 (or a bad negative value) migrates to instant.
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, lock_ramp_ms_from_rate(0.0f), "rate 0 -> 0 ms (instant)");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, lock_ramp_ms_from_rate(-5.0f), "rate <0 -> 0 ms (instant)");
}

void test_ramp_ms_from_rate_typical(void)
{
  // ms = 100000 / rate. 100 %/s -> 1000 ms, 200 -> 500, 1000 -> 100.
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(1000, lock_ramp_ms_from_rate(100.0f), "100 %/s -> 1000 ms");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(500, lock_ramp_ms_from_rate(200.0f), "200 %/s -> 500 ms");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(100, lock_ramp_ms_from_rate(1000.0f), "1000 %/s -> 100 ms");
}

void test_ramp_ms_from_rate_legacy_default(void)
{
  // The old release default of 120 %/s rounds to 833 ms full decay.
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(833, lock_ramp_ms_from_rate(120.0f), "120 %/s -> 833 ms");
}

void test_ramp_ms_from_rate_clamps_slow(void)
{
  // A slow legacy rate (over 1 s per full travel) clamps to the 1000 ms ceiling.
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(1000, lock_ramp_ms_from_rate(5.0f), "5 %/s -> clamp 1000 ms");
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(1000, lock_ramp_ms_from_rate(50.0f), "50 %/s -> clamp 1000 ms");
}

int main(int, char **)
{
  UNITY_BEGIN();
  RUN_TEST(test_rise_engage_zero_is_instant);
  RUN_TEST(test_rise_is_rate_limited);
  RUN_TEST(test_rise_clamps_at_target);
  RUN_TEST(test_rise_dt_zero_holds);
  RUN_TEST(test_fall_is_rate_limited);
  RUN_TEST(test_fall_clamps_at_target);
  RUN_TEST(test_fall_release_zero_is_instant);
  RUN_TEST(test_equal_returns_target);
  RUN_TEST(test_ramp_ms_from_rate_instant);
  RUN_TEST(test_ramp_ms_from_rate_typical);
  RUN_TEST(test_ramp_ms_from_rate_legacy_default);
  RUN_TEST(test_ramp_ms_from_rate_clamps_slow);
  return UNITY_END();
}
