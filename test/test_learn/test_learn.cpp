// Characterization + regression tests for the Haldex learn-table sample reducer.
//
// Pins learn_reduce_samples() (src/OpenHaldexC6_Calculations.cpp), the pure seam
// the learn task (OpenHaldexC6_tasks.cpp) uses to fold each CF settle window into
// one recorded engagement. The bug it fixes, observed on a live MQB (Gen5) car:
// near lock-up the Haldex ECU's own PWM loop briefly overshoots to full duty, so
// a single feedback frame decodes to a clean 100. The old loop wrote that raw
// spike straight into haldexLearnTable[cf]; lookup_learn_correction_factor then
// collapses every higher target onto that CF, corrupting the calibration.
//
// These tests feed spiked / dropout sample windows and assert (a) the reducer
// rejects the artifact and (b) a spike no longer collapses the downstream lookup.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Calculations.h>

extern uint8_t learn_reduce_samples(const uint8_t *samples, uint8_t n, uint8_t prev_recorded);
extern uint8_t lookup_learn_correction_factor(const uint8_t *table, uint8_t target);
extern bool motor11_use_bpk_packing(bool fix_hunting, bool learn_active, bool learn_table_valid);

void setUp(void) {}
void tearDown(void) {}

// --- median rejects a lone overshoot spike ---------------------------------

void test_single_spike_rejected(void)
{
  // One 100 frame in an otherwise-40 window -> median 40, not the spike.
  const uint8_t s[8] = {40, 40, 40, 100, 40, 40, 40, 40};
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(40, learn_reduce_samples(s, 8, 40), "lone spike must not survive");
}

void test_two_spikes_rejected(void)
{
  const uint8_t s[8] = {62, 100, 62, 62, 100, 62, 62, 62};
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(62, learn_reduce_samples(s, 8, 60), "two spikes still rejected");
}

void test_even_split_takes_lower_median(void)
{
  // Clean 4/4 split: lower median rejects the spike cluster (only a strict
  // majority of high frames - i.e. sustained, real engagement - should win).
  const uint8_t s[8] = {50, 50, 50, 50, 100, 100, 100, 100};
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(50, learn_reduce_samples(s, 8, 40), "even split -> lower median");
}

void test_sustained_high_is_accepted(void)
{
  // A real majority-high window IS the engagement - do not reject it.
  const uint8_t s[8] = {40, 100, 100, 100, 100, 100, 100, 40};
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(100, learn_reduce_samples(s, 8, 40), "sustained high accepted");
}

// --- dropout glaze via the monotonic clamp ---------------------------------

void test_all_zero_window_holds_previous(void)
{
  // Whole window dropped to 0 -> hold the last good reading, never record a dip.
  const uint8_t s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(55, learn_reduce_samples(s, 8, 55), "all-zero window glazes to prev");
}

void test_partial_dropout_median_survives(void)
{
  // A couple of 0 dropouts amid valid frames -> median is still the valid value.
  const uint8_t s[8] = {48, 0, 48, 48, 0, 48, 48, 48};
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(48, learn_reduce_samples(s, 8, 40), "partial dropout rejected");
}

void test_cf0_zero_baseline_recorded(void)
{
  // At CF 0 with no prior (prev 0) a genuine 0 baseline is recorded as 0.
  const uint8_t s[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, learn_reduce_samples(s, 8, 0), "cf0 zero baseline");
}

// --- monotonic non-decreasing clamp ----------------------------------------

void test_dip_below_previous_clamped_up(void)
{
  // Median 55 but previous CF already recorded 60 -> clamp to 60 (engagement
  // cannot fall as CF rises).
  const uint8_t s[8] = {55, 55, 55, 55, 55, 55, 55, 55};
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(60, learn_reduce_samples(s, 8, 60), "monotonic clamp up");
}

void test_normal_rise_passes_through(void)
{
  const uint8_t s[8] = {62, 62, 62, 62, 62, 62, 62, 62};
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(62, learn_reduce_samples(s, 8, 40), "normal rise recorded");
}

// --- guards ----------------------------------------------------------------

void test_zero_count_returns_prev(void)
{
  const uint8_t s[1] = {77};
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(33, learn_reduce_samples(s, 0, 33), "n=0 holds prev");
}

void test_null_samples_returns_prev(void)
{
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(21, learn_reduce_samples(nullptr, 8, 21), "null holds prev");
}

// --- end-to-end: a spike must not collapse the downstream lookup -----------

void test_spike_does_not_collapse_lookup(void)
{
  // Simulate a full CF 0..100 scan of a clean linear clutch (engagement ~= CF)
  // where the window at CF 62 is corrupted by a pump-overshoot spike. Build the
  // table through the reducer exactly as the learn task does, then assert the
  // lookup did NOT collapse the upper map onto CF 62.
  uint8_t table[101];
  uint8_t prev = 0;
  for (int cf = 0; cf <= 100; cf++)
  {
    uint8_t window[8];
    uint8_t truth = (uint8_t)cf; // clean linear engagement
    for (int s = 0; s < 8; s++)
    {
      window[s] = truth;
    }
    if (cf == 62)
    {
      window[3] = 100; // one overshoot frame in this settle window
    }
    prev = learn_reduce_samples(window, 8, prev);
    table[cf] = prev;
  }

  // CF 62 records ~62, not 100 - the spike was rejected.
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(62, table[62], "spiked window still records true engagement");

  // A target of 90 must resolve near CF 90, NOT collapse down to CF 62.
  uint8_t cf90 = lookup_learn_correction_factor(table, 90);
  TEST_ASSERT_UINT8_WITHIN_MESSAGE(2, 90, cf90, "target 90 must not collapse onto the spike CF");
}

void test_old_behaviour_would_have_collapsed(void)
{
  // Contrast case: a table where the raw spike WAS written (the old bug) does
  // collapse. This pins WHY the reducer matters - it is the guard, not cosmetics.
  uint8_t table[101];
  for (int i = 0; i <= 100; i++)
  {
    table[i] = (uint8_t)i;
  }
  table[62] = 100; // raw overshoot written verbatim, as the old loop did
  // Old lookup returns the smallest CF whose engagement >= target, so 90 lands
  // on 62 - the collapse this fix prevents.
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(62, lookup_learn_correction_factor(table, 90),
                                  "raw-spike table collapses (documents the bug)");
}

// --- Motor_11 packing selector: learn must force BPK ------------------------
// V3 packing pins the torque fields at full, so a learn on V3 records a flat
// ~100% table (the "sits at 100% regardless" symptom on the live MQB car). The
// selector forces BPK whenever Fix Hunting is on, a learn is active, OR a valid
// learn table exists (a learned table was measured under BPK, so it must be
// applied under BPK - never against a V3 frame).

void test_bpk_off_when_idle_and_toggle_off(void)
{
  // Normal driving, Fix Hunting off, no table -> V3 packing (legacy default).
  TEST_ASSERT_FALSE_MESSAGE(motor11_use_bpk_packing(false, false, false), "idle + toggle off + no table -> V3");
}

void test_bpk_on_when_toggle_on(void)
{
  // User enabled Fix Hunting -> BPK, learn or not.
  TEST_ASSERT_TRUE_MESSAGE(motor11_use_bpk_packing(true, false, false), "toggle on -> BPK");
}

void test_learn_forces_bpk_even_with_toggle_off(void)
{
  // A learn scan uses BPK regardless of the toggle, so it can never silently
  // record a flat 100% table.
  TEST_ASSERT_TRUE_MESSAGE(motor11_use_bpk_packing(false, true, false), "learn active -> BPK despite toggle off");
}

void test_learn_and_toggle_both_on_still_bpk(void)
{
  TEST_ASSERT_TRUE_MESSAGE(motor11_use_bpk_packing(true, true, false), "learn + toggle on -> BPK");
}

void test_valid_table_forces_bpk_with_toggle_off(void)
{
  // The fix: once a table has been learned (measured under BPK), driving with
  // Fix Hunting off must still use BPK so the calibration is applied against the
  // frame it was measured against - not a V3 frame this mode would otherwise send.
  TEST_ASSERT_TRUE_MESSAGE(motor11_use_bpk_packing(false, false, true), "valid table -> BPK despite toggle off");
}

void test_no_table_toggle_off_stays_v3(void)
{
  // An untuned user (no learn table, Fix Hunting off) keeps the legacy V3 path.
  TEST_ASSERT_FALSE_MESSAGE(motor11_use_bpk_packing(false, false, false), "no table + toggle off -> V3");
}

int main(int, char **)
{
  UNITY_BEGIN();

  RUN_TEST(test_single_spike_rejected);
  RUN_TEST(test_two_spikes_rejected);
  RUN_TEST(test_even_split_takes_lower_median);
  RUN_TEST(test_sustained_high_is_accepted);

  RUN_TEST(test_all_zero_window_holds_previous);
  RUN_TEST(test_partial_dropout_median_survives);
  RUN_TEST(test_cf0_zero_baseline_recorded);

  RUN_TEST(test_dip_below_previous_clamped_up);
  RUN_TEST(test_normal_rise_passes_through);

  RUN_TEST(test_zero_count_returns_prev);
  RUN_TEST(test_null_samples_returns_prev);

  RUN_TEST(test_spike_does_not_collapse_lookup);
  RUN_TEST(test_old_behaviour_would_have_collapsed);

  RUN_TEST(test_bpk_off_when_idle_and_toggle_off);
  RUN_TEST(test_bpk_on_when_toggle_on);
  RUN_TEST(test_learn_forces_bpk_even_with_toggle_off);
  RUN_TEST(test_learn_and_toggle_both_on_still_bpk);
  RUN_TEST(test_valid_table_forces_bpk_with_toggle_off);
  RUN_TEST(test_no_table_toggle_off_stays_v3);

  return UNITY_END();
}
