// Characterization tests for the low-power wake decision seam.
//
// Pins the behaviour of lpCanActive(isStandalone, lpHaldexFps, lpChassisFps,
// lpWakeThresholdFps) in include/OpenHaldexC6_lowpower.h, the pure seam that
// the LP_WATCHING/LP_SLEEPING state machine in src/OpenHaldexC6_IO.cpp calls to
// decide whether CAN traffic is active enough to keep the unit awake.
//
//   - Standalone: our own Haldex frame generator runs at a fixed 50 fps, so the
//     wake threshold is the constant 50 fps on the Haldex fps count. The
//     configurable chassis threshold is intentionally ignored in this mode.
//   - OEM: the vehicle drives the chassis bus, so wake is gated on the
//     configurable chassis fps threshold (lpWakeThresholdFps, UI slider,
//     default 50).
//
// The seam is header-only inline, so this test includes it directly and shares
// one source of truth with the shipped firmware — no mirrored copy to drift.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_lowpower.h>

void setUp(void) {}
void tearDown(void) {}

// ---- standalone: fixed 50 fps on Haldex fps, chassis/threshold ignored -----
void test_standalone_fixed_50_boundary(void)
{
  // chassis fps and threshold are deliberately set to values that would flip an
  // OEM decision, to prove standalone ignores them entirely.
  TEST_ASSERT_FALSE_MESSAGE(lpCanActive(true, 49U, 1000U, 0U),
                            "standalone: haldex 49 < 50 -> sleep (chassis/threshold ignored)");
  TEST_ASSERT_TRUE_MESSAGE(lpCanActive(true, 50U, 0U, 1000U),
                           "standalone: haldex 50 == 50 -> active (boundary, chassis/threshold ignored)");
  TEST_ASSERT_TRUE_MESSAGE(lpCanActive(true, 51U, 0U, 1000U),
                           "standalone: haldex 51 > 50 -> active");
}

// ---- standalone: zero Haldex fps -> asleep ---------------------------------
void test_standalone_zero_fps_sleeps(void)
{
  TEST_ASSERT_FALSE_MESSAGE(lpCanActive(true, 0U, 0U, 0U),
                            "standalone: haldex 0 fps -> sleep");
}

// ---- OEM: default 50 fps chassis threshold, at/below/above -----------------
void test_oem_default_threshold_boundary(void)
{
  const uint32_t threshold = 50U;
  TEST_ASSERT_FALSE_MESSAGE(lpCanActive(false, 1000U, 49U, threshold),
                            "OEM: chassis 49 < 50 -> sleep (haldex fps ignored)");
  TEST_ASSERT_TRUE_MESSAGE(lpCanActive(false, 0U, 50U, threshold),
                           "OEM: chassis 50 == 50 -> active (boundary, haldex fps ignored)");
  TEST_ASSERT_TRUE_MESSAGE(lpCanActive(false, 0U, 51U, threshold),
                           "OEM: chassis 51 > 50 -> active");
}

// ---- OEM: a non-default configurable threshold still bites -----------------
// Proves the decision tracks lpWakeThresholdFps (the UI slider), not a constant.
void test_oem_custom_threshold_boundary(void)
{
  const uint32_t threshold = 120U;
  TEST_ASSERT_FALSE_MESSAGE(lpCanActive(false, 1000U, 119U, threshold),
                            "OEM: chassis 119 < 120 -> sleep");
  TEST_ASSERT_TRUE_MESSAGE(lpCanActive(false, 0U, 120U, threshold),
                           "OEM: chassis 120 == 120 -> active (boundary)");
  TEST_ASSERT_TRUE_MESSAGE(lpCanActive(false, 0U, 121U, threshold),
                           "OEM: chassis 121 > 120 -> active");
}

// ---- OEM: zero threshold -> always active (any chassis fps >= 0) -----------
void test_oem_zero_threshold_always_active(void)
{
  TEST_ASSERT_TRUE_MESSAGE(lpCanActive(false, 0U, 0U, 0U),
                           "OEM: chassis 0 >= threshold 0 -> active");
  TEST_ASSERT_TRUE_MESSAGE(lpCanActive(false, 0U, 5U, 0U),
                           "OEM: chassis 5 >= threshold 0 -> active");
}

int main(int, char **)
{
  UNITY_BEGIN();
  RUN_TEST(test_standalone_fixed_50_boundary);
  RUN_TEST(test_standalone_zero_fps_sleeps);
  RUN_TEST(test_oem_default_threshold_boundary);
  RUN_TEST(test_oem_custom_threshold_boundary);
  RUN_TEST(test_oem_zero_threshold_always_active);
  return UNITY_END();
}
