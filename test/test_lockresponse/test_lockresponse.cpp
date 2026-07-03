// Characterization tests for the lock-target rate limiter.
//
// Pins lock_rate_limit_step() in src/OpenHaldexC6_Calculations.cpp, which
// getLockData uses to slew the lock target: rising transitions are capped at
// engage_rate %/s (0 = instantaneous, the historical behaviour) and falling
// transitions at release_rate %/s. These values reach the wire bytes the
// Haldex sees, so a red assertion here means a refactor changed the ramp
// behaviour - do NOT "fix" a golden to make a refactor pass.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Calculations.h>

void setUp(void) {}
void tearDown(void) {}

// ---- rising (engage / attack) ---------------------------------------------

void test_rise_engage_zero_is_instant(void)
{
  // Engage rate 0 preserves the historical instant lock-up.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(100.0f, lock_rate_limit_step(0.0f, 100.0f, 0.0f, 120.0f, 0.1f),
                                  "engage=0 -> rise instant");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, lock_rate_limit_step(10.0f, 40.0f, 0.0f, 120.0f, 0.0f),
                                  "engage=0, dt=0 -> rise instant");
}

void test_rise_is_rate_limited(void)
{
  // 100 %/s over 0.1 s allows a 10-point rise.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(10.0f, lock_rate_limit_step(0.0f, 100.0f, 100.0f, 120.0f, 0.1f),
                                  "engage=100, dt=0.1 -> +10");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(35.0f, lock_rate_limit_step(30.0f, 100.0f, 50.0f, 120.0f, 0.1f),
                                  "engage=50, dt=0.1 -> +5");
}

void test_rise_clamps_at_target(void)
{
  // A step that would overshoot lands exactly on the target.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, lock_rate_limit_step(35.0f, 40.0f, 100.0f, 120.0f, 0.1f),
                                  "rise within one step -> target");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(100.0f, lock_rate_limit_step(0.0f, 100.0f, 100.0f, 120.0f, 5.0f),
                                  "long dt -> clamped at target");
}

void test_rise_dt_zero_holds(void)
{
  // No elapsed time, nonzero engage rate -> no movement.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(20.0f, lock_rate_limit_step(20.0f, 100.0f, 100.0f, 120.0f, 0.0f),
                                  "engage>0, dt=0 -> hold");
}

// ---- falling (release) ------------------------------------------------------

void test_fall_is_rate_limited(void)
{
  // 120 %/s over 0.1 s allows a 12-point drop.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(88.0f, lock_rate_limit_step(100.0f, 0.0f, 0.0f, 120.0f, 0.1f),
                                  "release=120, dt=0.1 -> -12");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(95.0f, lock_rate_limit_step(100.0f, 0.0f, 0.0f, 50.0f, 0.1f),
                                  "release=50, dt=0.1 -> -5");
}

void test_fall_clamps_at_target(void)
{
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, lock_rate_limit_step(45.0f, 40.0f, 0.0f, 120.0f, 0.1f),
                                  "fall within one step -> target");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, lock_rate_limit_step(100.0f, 0.0f, 0.0f, 120.0f, 5.0f),
                                  "long dt -> clamped at target");
}

void test_fall_release_zero_holds(void)
{
  // Release rate 0 can never drop: the API floor of 5 %/s exists so a client
  // cannot configure this, but the step itself must stay put, not jump.
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(100.0f, lock_rate_limit_step(100.0f, 0.0f, 0.0f, 0.0f, 0.1f),
                                  "release=0 -> hold");
}

// ---- steady state -----------------------------------------------------------

void test_equal_returns_target(void)
{
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(40.0f, lock_rate_limit_step(40.0f, 40.0f, 100.0f, 120.0f, 0.1f),
                                  "current == target -> target");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.0f, lock_rate_limit_step(0.0f, 0.0f, 0.0f, 120.0f, 0.1f),
                                  "both zero -> zero");
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
  RUN_TEST(test_fall_release_zero_holds);
  RUN_TEST(test_equal_returns_target);
  return UNITY_END();
}
