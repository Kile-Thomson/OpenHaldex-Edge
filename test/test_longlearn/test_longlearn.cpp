// Tests for the Long Learn comparison seams and the Danger Zone ceiling
// (src/OpenHaldexC6_Calculations.cpp): ll_compare_points, ll_compare_curves,
// ll_judge and bpk_effective_ceiling_nm.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include <OpenHaldexC6_Calculations.h>

void setUp(void) {}
void tearDown(void) {}

void test_points_equal_reads_have_no_deviation(void)
{
  const uint8_t a[LL_NPTS] = {10, 30, 60, 100};
  uint8_t dev = 99;
  int8_t md = 99;
  ll_compare_points(a, a, dev, md);
  TEST_ASSERT_EQUAL_UINT8(0, dev);
  TEST_ASSERT_EQUAL_INT8(0, md);
}

void test_points_report_worst_point_and_direction(void)
{
  const uint8_t ref[LL_NPTS] = {10, 30, 60, 100};
  const uint8_t low[LL_NPTS] = {10, 20, 50, 100}; // -10, -10 at two points
  uint8_t dev = 0;
  int8_t md = 0;
  ll_compare_points(low, ref, dev, md);
  TEST_ASSERT_EQUAL_UINT8(10, dev);
  TEST_ASSERT_EQUAL_INT8(-5, md); // -20 / 4
  const uint8_t high[LL_NPTS] = {10, 30, 70, 100};
  ll_compare_points(high, ref, dev, md);
  TEST_ASSERT_EQUAL_UINT8(10, dev);
  TEST_ASSERT_TRUE(md > 0);
}

void test_curves_worst_deviation_and_mean(void)
{
  uint8_t ref[101], t[101];
  memset(ref, 50, sizeof(ref));
  memcpy(t, ref, sizeof(t));
  uint8_t dev = 0;
  int8_t md = 0;
  ll_compare_curves(t, ref, dev, md);
  TEST_ASSERT_EQUAL_UINT8(0, dev);
  t[40] = 30; // one dip of 20
  ll_compare_curves(t, ref, dev, md);
  TEST_ASSERT_EQUAL_UINT8(20, dev);
  TEST_ASSERT_EQUAL_INT8(0, md); // -20 / 101 truncates to 0
  for (int i = 0; i <= 100; i++)
    t[i] = 20; // whole curve 30 lower
  ll_compare_curves(t, ref, dev, md);
  TEST_ASSERT_EQUAL_UINT8(30, dev);
  TEST_ASSERT_EQUAL_INT8(-30, md);
}

void test_judge_within_tolerance_is_removed(void)
{
  TEST_ASSERT_EQUAL_UINT8(LLB_REMOVED, ll_judge(true, 4, 0, 4));
  TEST_ASSERT_EQUAL_UINT8(LLB_REMOVED, ll_judge(true, 0, -3, 4));
}

void test_judge_lower_or_not_smooth_is_needed(void)
{
  TEST_ASSERT_EQUAL_UINT8(LLB_NEEDED, ll_judge(true, 5, -2, 4));
  TEST_ASSERT_EQUAL_UINT8(LLB_NEEDED, ll_judge(true, 5, 0, 4));
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(LLB_NEEDED, ll_judge(false, 0, 0, 4), "lost smoothness");
}

void test_judge_higher_without_it_is_harmful(void)
{
  TEST_ASSERT_EQUAL_UINT8(LLB_HARMFUL, ll_judge(true, 12, 6, 4));
}

void test_judge_wide_noise_tolerance_widens_the_removed_band(void)
{
  // Same read: harmful at the default threshold, no effect once the measured
  // reference noise raised the threshold to 10.
  TEST_ASSERT_EQUAL_UINT8(LLB_HARMFUL, ll_judge(true, 8, 3, LL_TOLERANCE));
  TEST_ASSERT_EQUAL_UINT8(LLB_REMOVED, ll_judge(true, 8, 3, 10));
}

void test_bpk_ceiling_unchanged_when_danger_off(void)
{
  TEST_ASSERT_EQUAL_UINT16(220, bpk_effective_ceiling_nm(220, false, 320));
}

void test_bpk_ceiling_raised_by_danger_zone_only_if_higher(void)
{
  TEST_ASSERT_EQUAL_UINT16(320, bpk_effective_ceiling_nm(220, true, 320));
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(400, bpk_effective_ceiling_nm(400, true, 320), "user value already higher");
}

void test_bpk_ceiling_never_exceeds_signal_max(void)
{
  TEST_ASSERT_EQUAL_UINT16(509, bpk_effective_ceiling_nm(600, false, 320));
  TEST_ASSERT_EQUAL_UINT16(509, bpk_effective_ceiling_nm(220, true, 700));
}

int main(int, char **)
{
  UNITY_BEGIN();
  RUN_TEST(test_points_equal_reads_have_no_deviation);
  RUN_TEST(test_points_report_worst_point_and_direction);
  RUN_TEST(test_curves_worst_deviation_and_mean);
  RUN_TEST(test_judge_within_tolerance_is_removed);
  RUN_TEST(test_judge_lower_or_not_smooth_is_needed);
  RUN_TEST(test_judge_higher_without_it_is_harmful);
  RUN_TEST(test_judge_wide_noise_tolerance_widens_the_removed_band);
  RUN_TEST(test_bpk_ceiling_unchanged_when_danger_off);
  RUN_TEST(test_bpk_ceiling_raised_by_danger_zone_only_if_higher);
  RUN_TEST(test_bpk_ceiling_never_exceeds_signal_max);
  return UNITY_END();
}
