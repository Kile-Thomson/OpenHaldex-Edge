// Tests for the sleep auto-setup seams in include/OpenHaldexC6_lowpower.h
// (sleepCalParked, sleepCalCounts, sleepCalThreshold). The 15 min parked-bus
// measurement in src/OpenHaldexC6_IO.cpp only counts a second when all three
// conditions hold, and turns the average into a wake threshold.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_lowpower.h>

void setUp(void) {}
void tearDown(void) {}

void test_parked_needs_stopped_engine_off_ignition_off(void)
{
  TEST_ASSERT_TRUE(sleepCalParked(0, 0, true, false));
  TEST_ASSERT_FALSE_MESSAGE(sleepCalParked(5, 0, true, false), "moving");
  TEST_ASSERT_FALSE_MESSAGE(sleepCalParked(0, 800, true, false), "engine running");
  TEST_ASSERT_FALSE_MESSAGE(sleepCalParked(0, 0, true, true), "ignition on");
}

void test_parked_ignores_stale_rpm_without_chassis_bus(void)
{
  // No chassis frames: the last rpm is not trusted.
  TEST_ASSERT_TRUE(sleepCalParked(0, 800, false, false));
  // But KL15 still decides.
  TEST_ASSERT_FALSE(sleepCalParked(0, 800, false, true));
}

void test_counts_only_when_alone_parked_and_real_can_seen(void)
{
  TEST_ASSERT_TRUE(sleepCalCounts(true, true, true));
  TEST_ASSERT_FALSE_MESSAGE(sleepCalCounts(false, true, true), "client connected");
  TEST_ASSERT_FALSE_MESSAGE(sleepCalCounts(true, false, true), "not parked");
  TEST_ASSERT_FALSE_MESSAGE(sleepCalCounts(true, true, false), "bench unit, no CAN yet");
}

void test_threshold_adds_margin_and_rounds_up_to_10(void)
{
  TEST_ASSERT_EQUAL_UINT16(400, sleepCalThreshold(100, 300));
  TEST_ASSERT_EQUAL_UINT16(410, sleepCalThreshold(101, 300));
  TEST_ASSERT_EQUAL_UINT16(410, sleepCalThreshold(110, 300));
}

void test_threshold_stays_in_slider_range(void)
{
  TEST_ASSERT_EQUAL_UINT16(100, sleepCalThreshold(0, 0));
  TEST_ASSERT_EQUAL_UINT16(2000, sleepCalThreshold(5000, 300));
}

int main(int, char **)
{
  UNITY_BEGIN();
  RUN_TEST(test_parked_needs_stopped_engine_off_ignition_off);
  RUN_TEST(test_parked_ignores_stale_rpm_without_chassis_bus);
  RUN_TEST(test_counts_only_when_alone_parked_and_real_can_seen);
  RUN_TEST(test_threshold_adds_margin_and_rounds_up_to_10);
  RUN_TEST(test_threshold_stays_in_slider_range);
  return UNITY_END();
}
