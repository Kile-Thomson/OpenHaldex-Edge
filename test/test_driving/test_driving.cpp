// Characterization tests for the received-engagement scaling helper.
//
// These pin the CURRENT behaviour of:
//   * scale_haldex_engagement(raw, in_min, in_max)  (src/OpenHaldexC6_Calculations.cpp)
//
// scale_haldex_engagement replaces the raw Arduino map(raw, in_min, in_max, 0, 100)
// that parseCAN_hdx() (src/OpenHaldexC6_can.cpp) used to feed the uint8_t global
// received_haldex_engagement. map() extrapolates linearly outside the window, so
// an out-of-window raw byte produced a negative or >100 value that wrapped/
// overflowed when stored in uint8_t (e.g. -1 -> 255) and then drove the STOCK
// lock_target and the broadcast frame. The new helper clamps first, so the
// result is always 0..100. For valid in-window frames it returns EXACTLY what
// map() returned before (no wire change) - the golden values below are derived
// independently from map()'s formula: (raw - in_min) * 100 / (in_max - in_min).
//
// Golden math for the gen1 window (in_min=128, in_max=198, span=70):
//   raw=128 -> (0)  * 100 / 70 = 0
//   raw=163 -> (35) * 100 / 70 = 50
//   raw=198 -> (70) * 100 / 70 = 100
// Out-of-window inputs (raw<128 or raw>198) clamp to 0 / 100 respectively.
//
// The helper is pure integer arithmetic and references no Arduino/TWAI symbols,
// so it is host-executable. A red assertion here means the scaling logic drifted;
// do NOT "fix" a golden to make a refactor pass.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Calculations.h>

// Real functions under test (src/OpenHaldexC6_Calculations.cpp).
extern uint8_t scale_haldex_engagement(uint8_t raw, uint8_t in_min, uint8_t in_max);
extern uint8_t lookup_learn_correction_factor(const uint8_t* table, uint8_t target);
extern uint8_t get_lock_target_adjusted_value(uint8_t value, bool invert);

void setUp(void) {}
void tearDown(void) {}

// ===========================================================================
// scale_haldex_engagement(raw, in_min, in_max) - gen1 window 128..198
// ===========================================================================

void test_below_min_clamps_to_zero(void)
{
  // Negative/under-window: a raw byte below in_min must clamp to 0%, NOT
  // extrapolate to a negative value that wraps to a large uint8_t.
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, scale_haldex_engagement(100, 128, 198),
                                  "raw below in_min -> 0 (clamped, no underflow)");
}

void test_at_min_is_zero(void)
{
  // Boundary: raw == in_min -> 0% (matches map()).
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, scale_haldex_engagement(128, 128, 198),
                                  "raw at in_min -> 0");
}

void test_midpoint_matches_map(void)
{
  // In-window: raw=163 -> (163-128)*100/70 = 50, byte-identical to old map().
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(50u, scale_haldex_engagement(163, 128, 198),
                                  "raw midpoint -> 50 (matches map())");
}

void test_at_max_is_hundred(void)
{
  // Boundary: raw == in_max -> 100% (matches map()).
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(100u, scale_haldex_engagement(198, 128, 198),
                                  "raw at in_max -> 100");
}

void test_above_max_clamps_to_hundred(void)
{
  // Over-window: a raw byte above in_max must clamp to 100%, NOT extrapolate
  // past 100 (which would overflow/exceed the percentage contract).
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(100u, scale_haldex_engagement(255, 128, 198),
                                  "raw above in_max -> 100 (clamped, no overflow)");
}

void test_zero_raw_with_high_min_is_zero_not_wrap(void)
{
  // The exact overflow the helper fixes: raw=0 with a 128 in_min. map() would
  // yield (0-128)*100/70 = -182 -> wraps to a large uint8_t. Clamp -> 0.
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, scale_haldex_engagement(0, 128, 198),
                                  "raw=0 with in_min=128 -> 0, not a wrap");
}

void test_result_never_exceeds_hundred(void)
{
  // Invariant: across the entire uint8_t input domain the result is <= 100,
  // for every production window used in parseCAN_hdx().
  for (int raw = 0; raw <= 255; raw++)
  {
    TEST_ASSERT_TRUE_MESSAGE(scale_haldex_engagement((uint8_t)raw, 128, 198) <= 100,
                             "gen1 window result must stay <= 100");
    TEST_ASSERT_TRUE_MESSAGE(scale_haldex_engagement((uint8_t)raw, 128, 255) <= 100,
                             "gen2/gen4 window result must stay <= 100");
    TEST_ASSERT_TRUE_MESSAGE(scale_haldex_engagement((uint8_t)raw, 0, 250) <= 100,
                             "gen5 window result must stay <= 100");
  }
}

void test_degenerate_window_returns_zero(void)
{
  // Defensive: in_max <= in_min has no valid span; helper must return 0 rather
  // than divide by zero or by a negative span.
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, scale_haldex_engagement(200, 198, 198),
                                  "in_max == in_min -> 0 (degenerate window)");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, scale_haldex_engagement(200, 198, 128),
                                  "in_max < in_min -> 0 (degenerate window)");
}

// ===========================================================================
// lookup_learn_correction_factor(table, target) - clamp to highest learned
// engagement.
//
// Returns the smallest index i in 0..100 with table[i] >= target. When NO entry
// meets target (more lock requested than was ever learned), returns 100 so the
// caller clamps to the highest learned engagement instead of the old inline
// loop's silent fall-through to 0 (zero lock delivered when maximum lock wanted).
// ===========================================================================

void test_lookup_reaches_target_returns_smallest_index(void)
{
  // table[i] = min(2*i, 100). Smallest i with table[i] >= 40 is i=20. This is
  // the SAME found-case behaviour as the old inline loop.
  uint8_t table[101];
  for (int i = 0; i <= 100; i++)
  {
    int v = 2 * i;
    table[i] = (uint8_t)(v > 100 ? 100 : v);
  }
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(20u, lookup_learn_correction_factor(table, 40),
                                  "smallest i with table[i]>=40 is 20");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0u, lookup_learn_correction_factor(table, 0),
                                  "target 0 met at i=0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(50u, lookup_learn_correction_factor(table, 100),
                                  "smallest i reaching the learned max (100) is 50");
}

void test_lookup_over_request_clamps_to_hundred(void)
{
  // Partially-learned table whose max (10) is below target: the bug case. The
  // old loop fell through with correction_factor==0; the helper returns 100,
  // clamping to the highest learned engagement.
  uint8_t table[101];
  for (int i = 0; i <= 100; i++)
  {
    table[i] = 10; // nothing learned ever reaches 40
  }
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(100u, lookup_learn_correction_factor(table, 40),
                                  "no entry >= 40 -> clamp to 100, not 0");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(100u, lookup_learn_correction_factor(table, 255),
                                  "no entry >= 255 -> clamp to 100");
}

void test_adjval_over_request_clamps_not_zero(void)
{
  // End-to-end through get_lock_target_adjusted_value: with a valid but
  // partially-learned table whose max < lock_target, the over-request now
  // delivers a clamped non-zero corrected value (cf=100 -> full value) instead
  // of the pre-fix 0.
  state.mode             = MODE_6040; // not EXPERT
  state.pedal_threshold  = 0;         // throttle gate open -> lock_enabled() true
  disengageUnderSpeed    = 0;         // speed gate open
  disengageAboveSpeed    = 0;
  received_pedal_value   = 0.0f;
  received_vehicle_speed = 0;
  haldexLearnActive      = false;
  haldexLearnTableValid  = true;
  lock_target            = 40.0f; // not 0, not 100 -> learn-table branch
  for (int i = 0; i <= 100; i++)
  {
    haldexLearnTable[i] = 10; // nothing learned reaches 40 -> over-request
  }
  // cf clamps to 100 -> corrected = 0xFE * 100 / 100 = 0xFE (non-zero), NOT 0.
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xFE, get_lock_target_adjusted_value(0xFE, false),
                                  "over-request clamps to full value, not zero lock");
  TEST_ASSERT_NOT_EQUAL_MESSAGE(0x00, get_lock_target_adjusted_value(0xFE, false),
                                "over-request must not deliver zero lock");
}

// ===========================================================================

int main(int, char **)
{
  UNITY_BEGIN();

  RUN_TEST(test_below_min_clamps_to_zero);
  RUN_TEST(test_at_min_is_zero);
  RUN_TEST(test_midpoint_matches_map);
  RUN_TEST(test_at_max_is_hundred);
  RUN_TEST(test_above_max_clamps_to_hundred);
  RUN_TEST(test_zero_raw_with_high_min_is_zero_not_wrap);
  RUN_TEST(test_result_never_exceeds_hundred);
  RUN_TEST(test_degenerate_window_returns_zero);

  // learn-table lookup clamp
  RUN_TEST(test_lookup_reaches_target_returns_smallest_index);
  RUN_TEST(test_lookup_over_request_clamps_to_hundred);
  RUN_TEST(test_adjval_over_request_clamps_not_zero);

  return UNITY_END();
}
