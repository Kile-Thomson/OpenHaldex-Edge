// Characterization tests for geometry-compensated per-corner slip.
//
// Pins compute_corner_slip() in src/OpenHaldexC6_Calculations.cpp: given four raw
// ABS wheel-speed counts [FL, FR, RL, RR], a signed steering-wheel angle, the rack
// ratio and car geometry, it reports each corner's slip % (int8) relative to the
// speed Ackermann geometry predicts for that corner's turn radius. A red assertion
// here means a refactor changed the slip the gauge/dash sees - do NOT edit a golden
// to make a refactor pass; work out why the number moved.

#include <unity.h>
#include <cstdint>
#include <math.h>

#include <OpenHaldexC6_Calculations.h>

void setUp(void) {}
void tearDown(void) {}

// Audi TT Mk3 (8S) / Golf 7R MQB defaults, matching globals.cpp.
#define WB 2505
#define TF 1572
#define TR 1544
#define RATIO 15.6f
#define MIN_RAW 667 // ~5 km/h in 0.0075 km/h units

void test_straight_all_equal_zero(void)
{
  const uint16_t w[4] = {2000, 2000, 2000, 2000};
  int8_t slip[4];
  bool ok = compute_corner_slip(w, 0, RATIO, WB, TF, TR, MIN_RAW, slip);
  TEST_ASSERT_TRUE(ok);
  for (int i = 0; i < 4; i++)
    TEST_ASSERT_EQUAL_INT8_MESSAGE(0, slip[i], "equal speeds on a straight -> no slip");
}

void test_straight_one_fast_reads_positive_and_is_max(void)
{
  // FL spinning faster than the other three on a straight. It must read positive
  // and be the unique maximum; the (dragging) reference wheels read slightly
  // negative because slip is measured against the four-wheel mean.
  const uint16_t w[4] = {2200, 2000, 2000, 2000};
  int8_t slip[4];
  bool ok = compute_corner_slip(w, 0, RATIO, WB, TF, TR, MIN_RAW, slip);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_TRUE_MESSAGE(slip[0] > 0, "the fast wheel reads positive slip");
  TEST_ASSERT_TRUE_MESSAGE(slip[0] > slip[1] && slip[0] > slip[2] && slip[0] > slip[3],
                           "the fast wheel is the unique max");
  TEST_ASSERT_TRUE_MESSAGE(slip[1] < 0 && slip[2] < 0 && slip[3] < 0,
                           "the reference wheels read negative vs. the mean");
}

void test_positive_slip_clamps_to_127(void)
{
  const uint16_t w[4] = {60000, 1000, 1000, 1000};
  int8_t slip[4];
  bool ok = compute_corner_slip(w, 0, RATIO, WB, TF, TR, MIN_RAW, slip);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_INT8_MESSAGE(127, slip[0], "runaway wheel clamps at +127");
}

void test_stopped_wheel_clamps_to_minus_100(void)
{
  const uint16_t w[4] = {0, 3000, 3000, 3000};
  int8_t slip[4];
  bool ok = compute_corner_slip(w, 0, RATIO, WB, TF, TR, MIN_RAW, slip);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_EQUAL_INT8_MESSAGE(-100, slip[0], "a fully stopped wheel clamps at -100");
}

void test_too_slow_returns_false_and_zeroes(void)
{
  const uint16_t w[4] = {100, 100, 100, 100}; // mean below MIN_RAW
  int8_t slip[4] = {9, 9, 9, 9};
  bool ok = compute_corner_slip(w, 0, RATIO, WB, TF, TR, MIN_RAW, slip);
  TEST_ASSERT_FALSE_MESSAGE(ok, "below the speed floor is not trusted");
  for (int i = 0; i < 4; i++)
    TEST_ASSERT_EQUAL_INT8_MESSAGE(0, slip[i], "slip zeroed when untrusted");
}

void test_degenerate_geometry_returns_false(void)
{
  const uint16_t w[4] = {2000, 2000, 2000, 2000};
  int8_t slip[4];
  TEST_ASSERT_FALSE_MESSAGE(compute_corner_slip(w, 0, RATIO, 0, TF, TR, MIN_RAW, slip),
                            "zero wheelbase is rejected");
  TEST_ASSERT_FALSE_MESSAGE(compute_corner_slip(w, 0, 0.5f, WB, TF, TR, MIN_RAW, slip),
                            "sub-unity steering ratio is rejected");
  TEST_ASSERT_FALSE_MESSAGE(compute_corner_slip(w, 0, RATIO, WB, 0, TR, MIN_RAW, slip),
                            "zero front track is rejected");
  TEST_ASSERT_FALSE_MESSAGE(compute_corner_slip(w, 0, RATIO, WB, TF, 0, MIN_RAW, slip),
                            "zero rear track is rejected");
}

void test_right_turn_equal_speeds_inner_reads_positive(void)
{
  // Turning right with all four wheels at equal speed: geometry expects the inner
  // (right) wheels to travel a shorter arc and turn slower, so equal speed means
  // the right side is over-spinning (positive slip) and the left side is dragging
  // (negative). This is the compensation doing its job. +1560 tenths steering
  // wheel / 15.6 = ~10 deg road angle.
  const uint16_t w[4] = {2000, 2000, 2000, 2000};
  int8_t slip[4];
  bool ok = compute_corner_slip(w, 1560, RATIO, WB, TF, TR, MIN_RAW, slip);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_TRUE_MESSAGE(slip[1] > 0 && slip[3] > 0, "right (inner) wheels read positive");
  TEST_ASSERT_TRUE_MESSAGE(slip[0] < 0 && slip[2] < 0, "left (outer) wheels read negative");
}

void test_left_turn_mirrors_right(void)
{
  const uint16_t w[4] = {2000, 2000, 2000, 2000};
  int8_t slip[4];
  bool ok = compute_corner_slip(w, -1560, RATIO, WB, TF, TR, MIN_RAW, slip);
  TEST_ASSERT_TRUE(ok);
  TEST_ASSERT_TRUE_MESSAGE(slip[0] > 0 && slip[2] > 0, "left (inner) wheels read positive");
  TEST_ASSERT_TRUE_MESSAGE(slip[1] < 0 && slip[3] < 0, "right (outer) wheels read negative");
}

void test_geometry_matched_speeds_read_near_zero(void)
{
  // Feed each corner the speed its own turn radius predicts (radii computed here
  // from the same Ackermann relations) - a clean steady-state corner with real
  // grip. Compensation should cancel the geometry and report ~0 on every corner.
  const float L = (float)WB, htf = TF / 2.0f, htr = TR / 2.0f;
  const float delta = (1560.0f * 0.1f / RATIO) * 3.14159265f / 180.0f; // road angle, rad
  const float R = L / tanf(delta);
  const float rad_fl = sqrtf(L * L + (R + htf) * (R + htf)); // right turn: left outer
  const float rad_fr = sqrtf(L * L + (R - htf) * (R - htf));
  const float rad_rl = R + htr;
  const float rad_rr = R - htr;
  const float base = 2000.0f;
  const float meanR = (rad_fl + rad_fr + rad_rl + rad_rr) / 4.0f;
  const uint16_t w[4] = {
      (uint16_t)(base * rad_fl / meanR + 0.5f),
      (uint16_t)(base * rad_fr / meanR + 0.5f),
      (uint16_t)(base * rad_rl / meanR + 0.5f),
      (uint16_t)(base * rad_rr / meanR + 0.5f)};
  int8_t slip[4];
  bool ok = compute_corner_slip(w, 1560, RATIO, WB, TF, TR, MIN_RAW, slip);
  TEST_ASSERT_TRUE(ok);
  for (int i = 0; i < 4; i++)
  {
    TEST_ASSERT_TRUE_MESSAGE(slip[i] >= -1 && slip[i] <= 1,
                             "geometry-matched corner reads ~0 after compensation");
  }
}

int main(int, char **)
{
  UNITY_BEGIN();
  RUN_TEST(test_straight_all_equal_zero);
  RUN_TEST(test_straight_one_fast_reads_positive_and_is_max);
  RUN_TEST(test_positive_slip_clamps_to_127);
  RUN_TEST(test_stopped_wheel_clamps_to_minus_100);
  RUN_TEST(test_too_slow_returns_false_and_zeroes);
  RUN_TEST(test_degenerate_geometry_returns_false);
  RUN_TEST(test_right_turn_equal_speeds_inner_reads_positive);
  RUN_TEST(test_left_turn_mirrors_right);
  RUN_TEST(test_geometry_matched_speeds_read_near_zero);
  UNITY_END();
  return 0;
}
