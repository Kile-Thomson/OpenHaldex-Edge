// Characterization tests for the CAN bus-health failure-decision predicate.
//
// These pin the CURRENT behaviour of:
//   * can_alerts_indicate_failure(alerts, failure_mask)  (src/OpenHaldexC6_Calculations.cpp)
//   * the CAN_ALERTS_FAILURE_MASK composition             (include/OpenHaldexC6_defs.h)
//
// can_alerts_indicate_failure drives the always-on isBusFailure signal on both
// v2 CAN buses (R005/D005): a true verdict latches the failure and re-arms
// twai_initiate_recovery_v2. The mask is the production failure set — every
// enabled alert EXCEPT TWAI_ALERT_RX_DATA, which is a normal frame-received
// notification, not a fault. A red assertion here means the detection logic or
// the mask composition drifted; do NOT "fix" a golden to make a refactor pass.
//
// The predicate is pure arithmetic ((alerts & failure_mask) != 0) and references
// no TWAI driver symbols, so it is host-executable. The TWAI_ALERT_* constants
// and CAN_ALERTS_FAILURE_MASK arrive via OpenHaldexC6_Calculations.h ->
// OpenHaldexC6_defs.h -> the native driver/twai.h stub.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Calculations.h> // pulls OpenHaldexC6_defs.h (TWAI_ALERT_* + CAN_ALERTS_FAILURE_MASK)

// Real functions under test (src/OpenHaldexC6_Calculations.cpp).
extern bool can_alerts_indicate_failure(uint32_t alerts, uint32_t failure_mask);
extern bool can_alerts_indicate_recovered(uint32_t alerts, uint32_t recovered_mask);

void setUp(void) {}
void tearDown(void) {}

// ===========================================================================
// can_alerts_indicate_failure(alerts, failure_mask)
// ===========================================================================

void test_no_alerts_is_not_failure(void)
{
  // (a) Boundary: zero alerts -> never a failure.
  TEST_ASSERT_FALSE_MESSAGE(can_alerts_indicate_failure(0u, CAN_ALERTS_FAILURE_MASK),
                            "no alerts set -> not a failure");
}

void test_rx_data_alone_is_not_failure(void)
{
  // (b) Negative: a normal received-frame alert is NOT a bus failure. RX_DATA is
  // in CAN_ALERTS_ENABLE but deliberately excluded from CAN_ALERTS_FAILURE_MASK.
  TEST_ASSERT_FALSE_MESSAGE(can_alerts_indicate_failure(TWAI_ALERT_RX_DATA, CAN_ALERTS_FAILURE_MASK),
                            "RX_DATA alone -> not a failure");
}

void test_bus_off_is_failure(void)
{
  // (c) Positive: a hard bus-off alert trips detection on its own.
  TEST_ASSERT_TRUE_MESSAGE(can_alerts_indicate_failure(TWAI_ALERT_BUS_OFF, CAN_ALERTS_FAILURE_MASK),
                           "BUS_OFF -> failure");
}

void test_failure_bit_mixed_with_rx_data_is_failure(void)
{
  // (d) Real-world: a failure bit OR'd with the benign RX_DATA still trips detection.
  const uint32_t mixed = TWAI_ALERT_BUS_ERROR | TWAI_ALERT_RX_DATA;
  TEST_ASSERT_TRUE_MESSAGE(can_alerts_indicate_failure(mixed, CAN_ALERTS_FAILURE_MASK),
                           "BUS_ERROR|RX_DATA -> failure");
}

// ===========================================================================
// CAN_ALERTS_FAILURE_MASK composition — assert the mask directly so a change to
// the mask itself (not just the predicate) is caught.
// ===========================================================================

void test_mask_excludes_rx_data(void)
{
  // (e.1) RX_DATA must NOT be in the failure mask.
  TEST_ASSERT_EQUAL_HEX32_MESSAGE(0u, (CAN_ALERTS_FAILURE_MASK & TWAI_ALERT_RX_DATA),
                                  "CAN_ALERTS_FAILURE_MASK must exclude RX_DATA");
}

void test_mask_includes_bus_off(void)
{
  // (e.2) BUS_OFF must be in the failure mask.
  TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, (CAN_ALERTS_FAILURE_MASK & TWAI_ALERT_BUS_OFF),
                                "CAN_ALERTS_FAILURE_MASK must include BUS_OFF");
}

// ===========================================================================
// can_alerts_indicate_recovered(alerts, recovered_mask) — S01/T04
//
// Mirrors the failure predicate but for TWAI_ALERT_BUS_RECOVERED. A true verdict
// drives twai_start_v2 on the recovered bus and (when no fresh failure is present)
// clears the latched isBusFailure, so the device leaves the bus-off dead state
// instead of staying down until a power-cycle.
// ===========================================================================

void test_no_alerts_is_not_recovered(void)
{
  // (a) Boundary: zero alerts -> no recovery signalled.
  TEST_ASSERT_FALSE_MESSAGE(can_alerts_indicate_recovered(0u, CAN_ALERTS_RECOVERED_MASK),
                            "no alerts set -> not recovered");
}

void test_rx_data_alone_is_not_recovered(void)
{
  // (b) Negative: a normal received-frame alert is NOT a recovery event.
  TEST_ASSERT_FALSE_MESSAGE(can_alerts_indicate_recovered(TWAI_ALERT_RX_DATA, CAN_ALERTS_RECOVERED_MASK),
                            "RX_DATA alone -> not recovered");
}

void test_bus_recovered_is_recovered(void)
{
  // (c) Positive: the recovered alert trips the predicate on its own.
  TEST_ASSERT_TRUE_MESSAGE(can_alerts_indicate_recovered(TWAI_ALERT_BUS_RECOVERED, CAN_ALERTS_RECOVERED_MASK),
                           "BUS_RECOVERED -> recovered");
}

void test_bus_recovered_mixed_with_rx_data_is_recovered(void)
{
  // (d) Real-world: a recovered bit OR'd with benign RX_DATA still trips it.
  const uint32_t mixed = TWAI_ALERT_BUS_RECOVERED | TWAI_ALERT_RX_DATA;
  TEST_ASSERT_TRUE_MESSAGE(can_alerts_indicate_recovered(mixed, CAN_ALERTS_RECOVERED_MASK),
                           "BUS_RECOVERED|RX_DATA -> recovered");
}

void test_recovered_mask_includes_bus_recovered(void)
{
  // (e.3) BUS_RECOVERED must be in the recovered mask.
  TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, (CAN_ALERTS_RECOVERED_MASK & TWAI_ALERT_BUS_RECOVERED),
                                "CAN_ALERTS_RECOVERED_MASK must include BUS_RECOVERED");
}

void test_enable_includes_bus_recovered(void)
{
  // (e.4) BUS_RECOVERED must now be enabled so the alert can ever be read.
  TEST_ASSERT_NOT_EQUAL_MESSAGE(0u, (CAN_ALERTS_ENABLE & TWAI_ALERT_BUS_RECOVERED),
                                "CAN_ALERTS_ENABLE must include BUS_RECOVERED");
}

// ===========================================================================

int main(int, char **)
{
  UNITY_BEGIN();

  RUN_TEST(test_no_alerts_is_not_failure);
  RUN_TEST(test_rx_data_alone_is_not_failure);
  RUN_TEST(test_bus_off_is_failure);
  RUN_TEST(test_failure_bit_mixed_with_rx_data_is_failure);
  RUN_TEST(test_mask_excludes_rx_data);
  RUN_TEST(test_mask_includes_bus_off);

  RUN_TEST(test_no_alerts_is_not_recovered);
  RUN_TEST(test_rx_data_alone_is_not_recovered);
  RUN_TEST(test_bus_recovered_is_recovered);
  RUN_TEST(test_bus_recovered_mixed_with_rx_data_is_recovered);
  RUN_TEST(test_recovered_mask_includes_bus_recovered);
  RUN_TEST(test_enable_includes_bus_recovered);

  return UNITY_END();
}
