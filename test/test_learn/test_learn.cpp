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

// --- clamp when target exceeds every learned entry --------------------------
// Regression for the stuck-at-100% field bug: when more lock is requested than
// the sweep ever measured, the lookup must clamp to the CF of the highest
// learned engagement (argmax), NOT a hardcoded 100. Returning 100 makes
// get_lock_target_adjusted_value compute value * 100 / 100 = the full frame
// value (full bpkCeilingNm) for a target the car never learned.

void test_in_range_target_still_resolves_normally(void)
{
  // Guard: the argmax tracking must not disturb the normal in-range path. A
  // rising table where the target IS reachable still returns the smallest CF
  // meeting it (unchanged behaviour).
  uint8_t table[101];
  for (int i = 0; i <= 100; i++)
  {
    table[i] = (i / 4 < 25) ? (uint8_t)(i / 4) : (uint8_t)25; // rises to 25, then flat
  }
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(80, lookup_learn_correction_factor(table, 20),
                                  "in-range target resolves to first CF meeting it");
}

void test_partial_sweep_clamps_to_max_index_not_100(void)
{
  // Interrupted / partial sweep: only CF 0..30 learned (rising to 25%), CF 31..100
  // still 0. Max learned engagement is 25 at CF 30. A target of 40 meets no entry
  // and must clamp to CF 30 (argmax), NOT 100 - commanding CF 100 here would drive
  // the full frame against un-learned cells.
  uint8_t table[101] = {0};
  for (int cf = 0; cf <= 30; cf++)
  {
    table[cf] = (uint8_t)(cf * 25 / 30); // 0..25 across CF 0..30
  }
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(30, lookup_learn_correction_factor(table, 40),
                                  "unreachable target clamps to argmax CF, not 100");
  // Sanity: an unreachable target on this table must never return 100.
  TEST_ASSERT_TRUE_MESSAGE(lookup_learn_correction_factor(table, 90) != 100,
                           "partial-table over-target must not command full lock");
}

void test_argmax_prefers_lowest_cf_at_plateau(void)
{
  // Plateau at the top: engagement rises to 50 by CF 60 then holds 50 to CF 100.
  // Argmax is the FIRST CF reaching the max (60) - command the least frame value
  // that reaches peak learned engagement, not CF 100.
  uint8_t table[101];
  for (int i = 0; i <= 100; i++)
  {
    table[i] = (i < 60) ? (uint8_t)(i * 50 / 60) : (uint8_t)50;
  }
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(60, lookup_learn_correction_factor(table, 80),
                                  "over-target clamps to first CF at the plateau max");
}

void test_all_zero_table_yields_zero_not_full_lock(void)
{
  // Degenerate all-zero table (should be rejected upstream, but defend anyway):
  // no learned engagement -> argmax stays index 0 -> zero lock, never full lock.
  uint8_t table[101] = {0};
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, lookup_learn_correction_factor(table, 50),
                                  "all-zero table commands zero, not full lock");
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

// --- learn_finalize: interrupted sweeps restore the pre-learn calibration ----
// Regression for the cancel-wipes-calibration bug: startHaldexLearn wipes the
// table before the sweep, so a cancel (or the speed interlock firing) used to
// leave no table at all - silently reverting the user to the default CF formula
// and flipping Motor_11 packing back to V3. learn_finalize is the seam the task
// calls under stateMutex; these tests pin both restore paths and both backup
// states, plus the completed-sweep validation it also owns.

extern uint8_t learn_finalize(uint8_t *table, bool *valid,
                              const uint8_t *backup, bool backup_valid,
                              bool cancelled, bool speed_aborted, uint8_t current_step);

void test_cancel_restores_valid_backup(void)
{
  // A good calibration existed before the sweep; cancel at CF=5 must bring it
  // back exactly, valid flag included, and leave the step untouched.
  uint8_t backup[101];
  for (int i = 0; i <= 100; i++) { backup[i] = (uint8_t)i; }
  uint8_t table[101] = {0}; // wiped by startHaldexLearn, partially rescanned
  table[0] = 1; table[1] = 2;
  bool valid = false;

  uint8_t step = learn_finalize(table, &valid, backup, true, true, false, 5);

  TEST_ASSERT_EQUAL_UINT8_MESSAGE(5, step, "plain cancel keeps the current step");
  TEST_ASSERT_TRUE_MESSAGE(valid, "valid backup restores the valid flag");
  TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(backup, table, 101, "cancel restores the snapshot verbatim");
}

void test_cancel_with_no_prior_table_stays_invalid(void)
{
  // First-ever learn cancelled: the backup is the empty pre-learn state, so the
  // restore must leave the table invalid - not resurrect garbage as valid.
  uint8_t backup[101] = {0};
  uint8_t table[101] = {0};
  table[3] = 40; // partial sweep data that must be discarded
  bool valid = true; // poisoned on purpose; finalize must overwrite it

  learn_finalize(table, &valid, backup, false, true, false, 3);

  TEST_ASSERT_FALSE_MESSAGE(valid, "invalid backup restores invalid");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, table[3], "partial sweep data discarded");
}

void test_speed_abort_restores_and_reports_103(void)
{
  uint8_t backup[101];
  for (int i = 0; i <= 100; i++) { backup[i] = (uint8_t)(i / 2); }
  uint8_t table[101] = {0};
  bool valid = false;

  uint8_t step = learn_finalize(table, &valid, backup, true, false, true, 42);

  TEST_ASSERT_EQUAL_UINT8_MESSAGE(103, step, "speed abort reports step 103");
  TEST_ASSERT_TRUE_MESSAGE(valid, "speed abort restores the valid flag");
  TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(backup, table, 101, "speed abort restores the snapshot");
}

void test_completed_sweep_with_data_is_valid_101(void)
{
  uint8_t backup[101] = {0};
  uint8_t table[101] = {0};
  for (int i = 0; i <= 100; i++) { table[i] = (uint8_t)i; }
  bool valid = false;

  uint8_t step = learn_finalize(table, &valid, backup, false, false, false, 100);

  TEST_ASSERT_EQUAL_UINT8_MESSAGE(101, step, "completed sweep reports 101");
  TEST_ASSERT_TRUE_MESSAGE(valid, "non-zero table marks valid");
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(50, table[50], "completed sweep keeps the scanned table");
}

void test_completed_all_zero_sweep_is_invalid_102(void)
{
  // A sweep that recorded nothing (e.g. Haldex feedback dead) must not replace
  // the calibration with a valid all-zero table.
  uint8_t backup[101] = {0};
  uint8_t table[101] = {0};
  bool valid = true; // poisoned; finalize must clear it

  uint8_t step = learn_finalize(table, &valid, backup, false, false, false, 100);

  TEST_ASSERT_EQUAL_UINT8_MESSAGE(102, step, "empty completed sweep reports 102");
  TEST_ASSERT_FALSE_MESSAGE(valid, "all-zero completed sweep stays invalid");
}

void test_task_create_failure_restores_backup(void)
{
  // startHaldexLearn wipes the table BEFORE spawning the sweep task. If
  // xTaskCreate fails, nothing will ever republish, so it rolls back through
  // the same finalize path as a cancel (cancelled=true, step untouched) and
  // clears haldexLearnActive itself. Pin that rollback: the wiped table and
  // valid flag must return exactly to the pre-learn snapshot.
  uint8_t backup[101];
  for (int i = 0; i <= 100; i++) { backup[i] = (uint8_t)(100 - i); }
  uint8_t table[101] = {0}; // already wiped, no CF scanned yet (step 0)
  bool valid = false;       // cleared by the wipe

  uint8_t step = learn_finalize(table, &valid, backup, true, true, false, 0);

  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, step, "create-failure rollback keeps step 0");
  TEST_ASSERT_TRUE_MESSAGE(valid, "create-failure rollback restores the valid flag");
  TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(backup, table, 101, "create-failure rollback restores the snapshot");
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

  RUN_TEST(test_in_range_target_still_resolves_normally);
  RUN_TEST(test_partial_sweep_clamps_to_max_index_not_100);
  RUN_TEST(test_argmax_prefers_lowest_cf_at_plateau);
  RUN_TEST(test_all_zero_table_yields_zero_not_full_lock);

  RUN_TEST(test_bpk_off_when_idle_and_toggle_off);
  RUN_TEST(test_bpk_on_when_toggle_on);
  RUN_TEST(test_learn_forces_bpk_even_with_toggle_off);
  RUN_TEST(test_learn_and_toggle_both_on_still_bpk);
  RUN_TEST(test_valid_table_forces_bpk_with_toggle_off);
  RUN_TEST(test_no_table_toggle_off_stays_v3);

  RUN_TEST(test_cancel_restores_valid_backup);
  RUN_TEST(test_cancel_with_no_prior_table_stays_invalid);
  RUN_TEST(test_speed_abort_restores_and_reports_103);
  RUN_TEST(test_completed_sweep_with_data_is_valid_101);
  RUN_TEST(test_completed_all_zero_sweep_is_invalid_102);
  RUN_TEST(test_task_create_failure_restores_backup);

  return UNITY_END();
}
