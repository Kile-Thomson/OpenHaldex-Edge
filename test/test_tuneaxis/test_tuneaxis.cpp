// Characterization tests for the expert-map axis validator.
//
// Pins is_strictly_ascending_u16 in src/OpenHaldexC6_Calculations.cpp, which
// tuneIncoming (src/OpenHaldexC6_API.cpp) uses to reject a tune whose speed or
// throttle axis is not strictly ascending. get_expert_lock_target's bracket
// search assumes ascending axes; an out-of-order axis silently mis-interpolates.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Calculations.h>

extern bool is_strictly_ascending_u16(const uint16_t* arr, uint8_t count);

void setUp(void) {}
void tearDown(void) {}

void test_default_axes_are_ascending(void)
{
  const uint16_t speed[7] = {0, 30, 60, 90, 120, 160, 180};
  const uint16_t throttle[7] = {0, 15, 30, 45, 60, 75, 90};
  TEST_ASSERT_TRUE_MESSAGE(is_strictly_ascending_u16(speed, 7), "default speed axis ascending");
  TEST_ASSERT_TRUE_MESSAGE(is_strictly_ascending_u16(throttle, 7), "default throttle axis ascending");
}

void test_out_of_order_rejected(void)
{
  const uint16_t bad[7] = {0, 30, 20, 90, 120, 160, 180};
  TEST_ASSERT_FALSE_MESSAGE(is_strictly_ascending_u16(bad, 7), "descending step -> not ascending");
}

void test_equal_adjacent_rejected(void)
{
  // Strictly ascending: equal neighbours are not allowed (a zero interpolation
  // denominator band).
  const uint16_t dup[7] = {0, 30, 30, 90, 120, 160, 180};
  TEST_ASSERT_FALSE_MESSAGE(is_strictly_ascending_u16(dup, 7), "equal adjacent -> not strictly ascending");
}

void test_short_arrays_are_vacuously_ascending(void)
{
  const uint16_t one[1] = {42};
  TEST_ASSERT_TRUE_MESSAGE(is_strictly_ascending_u16(one, 1), "single element -> ascending");
  TEST_ASSERT_TRUE_MESSAGE(is_strictly_ascending_u16(one, 0), "empty -> ascending");
}

int main(int, char **)
{
  UNITY_BEGIN();
  RUN_TEST(test_default_axes_are_ascending);
  RUN_TEST(test_out_of_order_rejected);
  RUN_TEST(test_equal_adjacent_rejected);
  RUN_TEST(test_short_arrays_are_vacuously_ascending);
  return UNITY_END();
}
