// Characterization + fix tests for the speed-disengage gate chatter.
//
// speed_disengage_ok() in src/OpenHaldexC6_Calculations.cpp is a bare comparison
// with no memory. getLockData re-evaluates it every chassis frame, so a speed
// signal resting on disengageUnderSpeed or disengageAboveSpeed and dithering by a
// single unit flips the gate frame-to-frame, and the lock target hunts 0 <-> target.
// The first test makes that mechanism concrete; the rest pin disengage_gate_hysteresis(),
// the Schmitt-trigger fix, including that a zero band is a behaviour-preserving no-op.
// A red assertion here means a refactor changed the gate - do NOT "fix" a golden to
// make a refactor pass.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Calculations.h>

void setUp(void) {}
void tearDown(void) {}

// ---- the mechanism: raw gate has no hysteresis -----------------------------

void test_raw_gate_chatters_at_lower_bound(void)
{
  // disengage_under = 20, no upper cut. Adjacent speeds either side of the bound
  // give opposite answers: a 1 km/h dither at a steady 20 km/h cruise flips it.
  TEST_ASSERT_TRUE_MESSAGE(speed_disengage_ok(20, 20, 0), "at bound -> enabled");
  TEST_ASSERT_FALSE_MESSAGE(speed_disengage_ok(19, 20, 0), "1 below bound -> disabled");
  TEST_ASSERT_TRUE_MESSAGE(speed_disengage_ok(21, 20, 0), "1 above bound -> enabled");
}

void test_raw_gate_chatters_at_upper_bound(void)
{
  // disengage_above = 100, no lower cut. Same sharp edge at the top of the band.
  TEST_ASSERT_TRUE_MESSAGE(speed_disengage_ok(100, 0, 100), "at bound -> enabled");
  TEST_ASSERT_FALSE_MESSAGE(speed_disengage_ok(101, 0, 100), "1 above bound -> disabled");
}

// ---- the fix: hysteresis holds through the dither ---------------------------

void test_hysteresis_holds_at_lower_bound(void)
{
  const uint16_t under = 20, above = 0, hys = 3;

  bool en = disengage_gate_hysteresis(25, under, above, hys, false); // clearly above -> engage
  TEST_ASSERT_TRUE_MESSAGE(en, "25 -> engage");

  // Dither around the bound: exit threshold is under - hys = 17, so 18/19/20 all hold.
  en = disengage_gate_hysteresis(19, under, above, hys, en);
  TEST_ASSERT_TRUE_MESSAGE(en, "19 within band -> hold");
  en = disengage_gate_hysteresis(20, under, above, hys, en);
  TEST_ASSERT_TRUE_MESSAGE(en, "20 back at bound -> hold");
  en = disengage_gate_hysteresis(18, under, above, hys, en);
  TEST_ASSERT_TRUE_MESSAGE(en, "18 == under-2 within band -> hold");

  // Drop a full band below the bound: now it disengages.
  en = disengage_gate_hysteresis(16, under, above, hys, en); // 16 < 17 -> out
  TEST_ASSERT_FALSE_MESSAGE(en, "16 == under-4 past band -> disengage");

  // Re-entry is sharp at the nominal bound, not at under-hys.
  en = disengage_gate_hysteresis(19, under, above, hys, en);
  TEST_ASSERT_FALSE_MESSAGE(en, "19 still below under -> stays off");
  en = disengage_gate_hysteresis(20, under, above, hys, en);
  TEST_ASSERT_TRUE_MESSAGE(en, "20 at bound -> re-engage");
}

void test_hysteresis_holds_at_upper_bound(void)
{
  const uint16_t under = 0, above = 100, hys = 3;

  bool en = disengage_gate_hysteresis(90, under, above, hys, false);
  TEST_ASSERT_TRUE_MESSAGE(en, "90 -> engage");

  // Exit threshold is above + hys = 103, so 101/102/103 hold.
  en = disengage_gate_hysteresis(101, under, above, hys, en);
  TEST_ASSERT_TRUE_MESSAGE(en, "101 within band -> hold");
  en = disengage_gate_hysteresis(103, under, above, hys, en);
  TEST_ASSERT_TRUE_MESSAGE(en, "103 == above+3 within band -> hold");
  en = disengage_gate_hysteresis(104, under, above, hys, en);
  TEST_ASSERT_FALSE_MESSAGE(en, "104 past band -> disengage");
}

// ---- the fix reduces to today's behaviour when the band is 0 ----------------

void test_hysteresis_zero_is_the_sharp_gate(void)
{
  // With hys = 0 the hysteretic gate must equal speed_disengage_ok for every
  // speed, whatever the previous state - so wiring it in with a default band of 0
  // changes nothing until a nonzero band is deliberately configured.
  const uint16_t under = 20, above = 100;
  for (uint16_t s = 0; s <= 130; s++)
  {
    bool sharp = speed_disengage_ok(s, under, above);
    TEST_ASSERT_EQUAL_MESSAGE(sharp, disengage_gate_hysteresis(s, under, above, 0, false),
                              "hys=0, prev=false matches sharp gate");
    TEST_ASSERT_EQUAL_MESSAGE(sharp, disengage_gate_hysteresis(s, under, above, 0, true),
                              "hys=0, prev=true matches sharp gate");
  }
}

// ---- a small band never underflows near speed 0 -----------------------------

void test_hysteresis_no_underflow_near_zero(void)
{
  // under = 2, hys = 5: exit threshold under-hys would be -3 in signed math. The
  // unsigned `speed + hys >= under` form must not wrap; speed 0 stays engaged.
  bool en = disengage_gate_hysteresis(10, 2, 0, 5, false);
  TEST_ASSERT_TRUE_MESSAGE(en, "10 -> engage");
  en = disengage_gate_hysteresis(0, 2, 0, 5, en); // 0 + 5 >= 2 -> hold, no wrap
  TEST_ASSERT_TRUE_MESSAGE(en, "speed 0 within band, no underflow -> hold");
}

int main(int, char **)
{
  UNITY_BEGIN();
  RUN_TEST(test_raw_gate_chatters_at_lower_bound);
  RUN_TEST(test_raw_gate_chatters_at_upper_bound);
  RUN_TEST(test_hysteresis_holds_at_lower_bound);
  RUN_TEST(test_hysteresis_holds_at_upper_bound);
  RUN_TEST(test_hysteresis_zero_is_the_sharp_gate);
  RUN_TEST(test_hysteresis_no_underflow_near_zero);
  return UNITY_END();
}
