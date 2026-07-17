// Characterization tests for the external-diagnostic-tool auto-pause predicates.
//
// These pin the CURRENT behaviour of:
//   * is_external_diag_request_id(can_id)                  (src/OpenHaldexC6_Calculations.cpp)
//   * external_diag_active(last_seen_ms, now_ms, timeout)  (src/OpenHaldexC6_Calculations.cpp)
//   * the EXTERNAL_DIAG_TIMEOUT_MS constant                (include/OpenHaldexC6_defs.h)
//
// Together these drive the auto-pause of udsMQBTask: parseCAN_chs stamps
// externalDiagLastMs whenever is_external_diag_request_id() matches an inbound
// Bus 0 frame, and externalDiagActive() (a millis() wrapper over
// external_diag_active) keeps our UDS polling off the bus while a real scan tool
// (VCDS/ODIS/OBD) owns the diagnostic session. Both predicates are pure integer
// logic with no Arduino/TWAI symbols, so they run on host. A red assertion here
// means the detection id set or the timeout window drifted; do NOT loosen an
// assertion to make a refactor pass.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Calculations.h> // pulls OpenHaldexC6_defs.h (EXTERNAL_DIAG_TIMEOUT_MS)

// Real functions under test (src/OpenHaldexC6_Calculations.cpp).
extern bool is_external_diag_request_id(uint32_t can_id);
extern bool external_diag_active(uint32_t last_seen_ms, uint32_t now_ms, uint32_t timeout_ms);

void setUp(void) {}
void tearDown(void) {}

// ===========================================================================
// is_external_diag_request_id(can_id)
// Matches 0x7DF (OBD-II functional) and the 0x700-0x71F physical-tester range.
// ===========================================================================

void test_obd_functional_request_is_diag(void)
{
  // 0x7DF: ISO 15765-4 OBD functional broadcast request - always a tool.
  TEST_ASSERT_TRUE_MESSAGE(is_external_diag_request_id(0x7DFu), "0x7DF -> external diag");
}

void test_physical_tester_range_bounds_are_diag(void)
{
  // Inclusive 0x700-0x71F range - test both edges and an interior id. 0x70F,
  // 0x710 and 0x71D are the tester ids our own gateway responds to, so a real
  // scanner using them on Bus 0 is exactly what we must back off from.
  TEST_ASSERT_TRUE_MESSAGE(is_external_diag_request_id(0x700u), "0x700 (low edge) -> external diag");
  TEST_ASSERT_TRUE_MESSAGE(is_external_diag_request_id(0x70Fu), "0x70F -> external diag");
  TEST_ASSERT_TRUE_MESSAGE(is_external_diag_request_id(0x71Fu), "0x71F (high edge) -> external diag");
}

void test_ids_outside_range_are_not_diag(void)
{
  // Just below and just above the range must not match.
  TEST_ASSERT_FALSE_MESSAGE(is_external_diag_request_id(0x6FFu), "0x6FF (below range) -> not diag");
  TEST_ASSERT_FALSE_MESSAGE(is_external_diag_request_id(0x720u), "0x720 (above range) -> not diag");
}

void test_our_own_response_id_is_not_diag(void)
{
  // 0x779 is the Haldex ECU's UDS RESPONSE id (routed into our rx queue). It is
  // not a tester request and must never trip detection, or we would pause on our
  // own poll responses.
  TEST_ASSERT_FALSE_MESSAGE(is_external_diag_request_id(0x779u), "0x779 (UDS response) -> not diag");
}

void test_tp20_setup_id_is_not_diag_on_this_fork(void)
{
  // 0x200 is Adam's TP2.0 channel-setup id. This fork does not run TP2.0 and
  // 0x200 sits in the normal MQB broadcast-data region, so it is deliberately
  // excluded to avoid false-triggering on ordinary chassis traffic.
  TEST_ASSERT_FALSE_MESSAGE(is_external_diag_request_id(0x200u), "0x200 (TP2.0 setup) -> not diag on this fork");
}

// ===========================================================================
// external_diag_active(last_seen_ms, now_ms, timeout_ms)
// ===========================================================================

void test_never_seen_is_not_active(void)
{
  // last_seen_ms == 0 is the sentinel for "no tester ever seen".
  TEST_ASSERT_FALSE_MESSAGE(external_diag_active(0u, 5000u, EXTERNAL_DIAG_TIMEOUT_MS),
                            "last_seen 0 -> never active");
}

void test_within_window_is_active(void)
{
  // Seen 1 s ago with a 4 s window -> still active.
  TEST_ASSERT_TRUE_MESSAGE(external_diag_active(1000u, 2000u, EXTERNAL_DIAG_TIMEOUT_MS),
                           "1 s elapsed < 4 s window -> active");
}

void test_at_timeout_boundary_is_not_active(void)
{
  // Exactly timeout_ms elapsed -> not active (strict < window).
  TEST_ASSERT_FALSE_MESSAGE(external_diag_active(1000u, 1000u + EXTERNAL_DIAG_TIMEOUT_MS, EXTERNAL_DIAG_TIMEOUT_MS),
                            "elapsed == window -> not active");
}

void test_past_window_is_not_active(void)
{
  // Well past the window -> tool gone quiet, polling resumes.
  TEST_ASSERT_FALSE_MESSAGE(external_diag_active(1000u, 10000u, EXTERNAL_DIAG_TIMEOUT_MS),
                            "9 s elapsed > 4 s window -> not active");
}

void test_millis_rollover_still_active(void)
{
  // Tester seen just before the millis() rollover; now wrapped past zero. The
  // wrap-safe unsigned subtraction must report the true 100 ms elapsed, not a
  // ~4.29e9 ms gap. last=UINT32_MAX-99, now=0 -> elapsed 100 ms.
  const uint32_t last = 0xFFFFFFFFu - 99u;
  TEST_ASSERT_TRUE_MESSAGE(external_diag_active(last, 0u, EXTERNAL_DIAG_TIMEOUT_MS),
                           "100 ms across rollover -> still active");
}

void test_timeout_constant_is_expected(void)
{
  // Pin the window so a change to the constant is a deliberate, visible edit.
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(4000u, EXTERNAL_DIAG_TIMEOUT_MS,
                                   "EXTERNAL_DIAG_TIMEOUT_MS expected 4000");
}

// ===========================================================================

int main(int, char **)
{
  UNITY_BEGIN();

  RUN_TEST(test_obd_functional_request_is_diag);
  RUN_TEST(test_physical_tester_range_bounds_are_diag);
  RUN_TEST(test_ids_outside_range_are_not_diag);
  RUN_TEST(test_our_own_response_id_is_not_diag);
  RUN_TEST(test_tp20_setup_id_is_not_diag_on_this_fork);

  RUN_TEST(test_never_seen_is_not_active);
  RUN_TEST(test_within_window_is_active);
  RUN_TEST(test_at_timeout_boundary_is_not_active);
  RUN_TEST(test_past_window_is_not_active);
  RUN_TEST(test_millis_rollover_still_active);
  RUN_TEST(test_timeout_constant_is_expected);

  return UNITY_END();
}
