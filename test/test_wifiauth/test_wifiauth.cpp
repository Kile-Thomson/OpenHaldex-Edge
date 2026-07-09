// Tests for the single auth boundary: the WiFi AP password.
//
// The device has one credential now - the WiFi AP password. There is no separate
// HTTP login. wifi_password_provisioned() in src/OpenHaldexC6_Calculations.cpp is
// the one host-testable decision behind both:
//   * isDeviceProvisioned()      - dashboard serves normally vs. forces the
//                                  first-run /setup page while the AP is open.
//   * analyzerInjectionPermitted() - host->device CAN transmit on the analyzer
//                                  port is dropped while the AP is open.
//
// A WPA2 AP password must be >= 8 chars. Anything shorter (including "" / NULL)
// leaves the AP open, so the device is unprovisioned. These tests pin that
// threshold exactly - the softAP fallback and the /api/wifi validator both use
// the same >= 8 rule, so a drift here is a real fail-open regression.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Calculations.h>

void setUp(void) {}
void tearDown(void) {}

// ---- unset / open AP -> not provisioned ------------------------------------

void test_null_is_not_provisioned(void)
{
  TEST_ASSERT_FALSE_MESSAGE(wifi_password_provisioned(nullptr),
                            "NULL password must be unprovisioned (fail-closed)");
}

void test_empty_is_not_provisioned(void)
{
  TEST_ASSERT_FALSE_MESSAGE(wifi_password_provisioned(""),
                            "empty password (open AP) must be unprovisioned");
}

// ---- the >= 8 threshold is the load-bearing boundary ------------------------

void test_seven_chars_is_not_provisioned(void)
{
  // 7 chars is below WPA2 minimum: the softAP falls back to open, so the device
  // must read as unprovisioned rather than fail open with a half-length secret.
  TEST_ASSERT_FALSE_MESSAGE(wifi_password_provisioned("1234567"),
                            "7 chars < WPA2 minimum -> unprovisioned");
}

void test_eight_chars_is_provisioned(void)
{
  TEST_ASSERT_TRUE_MESSAGE(wifi_password_provisioned("12345678"),
                           "exactly 8 chars -> provisioned");
}

void test_long_password_is_provisioned(void)
{
  TEST_ASSERT_TRUE_MESSAGE(wifi_password_provisioned("a-much-longer-ap-password"),
                           "well over 8 chars -> provisioned");
}

int main(int, char **)
{
  UNITY_BEGIN();
  RUN_TEST(test_null_is_not_provisioned);
  RUN_TEST(test_empty_is_not_provisioned);
  RUN_TEST(test_seven_chars_is_not_provisioned);
  RUN_TEST(test_eight_chars_is_provisioned);
  RUN_TEST(test_long_password_is_provisioned);
  return UNITY_END();
}
