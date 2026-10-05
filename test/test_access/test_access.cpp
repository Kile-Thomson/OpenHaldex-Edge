// Tests for the web access boundary (include/OpenHaldexC6_Access.h):
// AP-interface requests are trusted, STA-interface requests need Basic auth,
// nothing but setup is reachable before a password exists.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Access.h>

static const uint32_t AP_IP  = 0xC0A80101; // 192.168.1.1
static const uint32_t STA_IP = 0xC0A80A32; // 192.168.10.50

void setUp(void) {}
void tearDown(void) {}

void test_ap_interface_is_not_sta(void) {
  TEST_ASSERT_FALSE(access_via_sta(AP_IP, AP_IP));
}

void test_other_local_address_is_sta(void) {
  TEST_ASSERT_TRUE(access_via_sta(STA_IP, AP_IP));
}

void test_unknown_addresses_fail_closed(void) {
  TEST_ASSERT_TRUE(access_via_sta(0, AP_IP));
  TEST_ASSERT_TRUE(access_via_sta(AP_IP, 0));
  TEST_ASSERT_TRUE(access_via_sta(0, 0));
}

void test_provisioned_ap_client_allowed_without_credentials(void) {
  TEST_ASSERT_EQUAL(ACCESS_ALLOW, access_decide(false, true, false, false));
}

void test_provisioned_sta_without_credentials_challenged(void) {
  TEST_ASSERT_EQUAL(ACCESS_CHALLENGE, access_decide(true, true, false, false));
  TEST_ASSERT_EQUAL(ACCESS_CHALLENGE, access_decide(true, true, false, true)); // setup path gives no bypass
}

void test_provisioned_sta_with_credentials_allowed(void) {
  TEST_ASSERT_EQUAL(ACCESS_ALLOW, access_decide(true, true, true, false));
}

void test_unprovisioned_sta_always_denied(void) {
  TEST_ASSERT_EQUAL(ACCESS_DENY_UNPROVISIONED, access_decide(true, false, false, false));
  TEST_ASSERT_EQUAL(ACCESS_DENY_UNPROVISIONED, access_decide(true, false, true, true)); // creds mean nothing yet
}

void test_unprovisioned_ap_only_setup_paths(void) {
  TEST_ASSERT_EQUAL(ACCESS_SETUP_REDIRECT, access_decide(false, false, false, false));
  TEST_ASSERT_EQUAL(ACCESS_ALLOW, access_decide(false, false, false, true));
}

void test_setup_paths(void) {
  TEST_ASSERT_TRUE(access_is_setup_path("/setup"));
  TEST_ASSERT_TRUE(access_is_setup_path("/setup?x=1"));
  TEST_ASSERT_TRUE(access_is_setup_path("/api/wifi"));
  TEST_ASSERT_FALSE(access_is_setup_path("/api/wifi/ssid"));
  TEST_ASSERT_FALSE(access_is_setup_path("/api/wifi/reset"));
  TEST_ASSERT_FALSE(access_is_setup_path("/setup2"));
  TEST_ASSERT_FALSE(access_is_setup_path("/ota/update"));
  TEST_ASSERT_FALSE(access_is_setup_path("/"));
  TEST_ASSERT_FALSE(access_is_setup_path(nullptr));
}

void test_analyzer_injection_gate(void) {
  TEST_ASSERT_TRUE(access_analyzer_injection(true, false));
  TEST_ASSERT_FALSE(access_analyzer_injection(false, false)); // open AP
  TEST_ASSERT_FALSE(access_analyzer_injection(true, true));   // home network client
  TEST_ASSERT_FALSE(access_analyzer_injection(false, true));
}

void test_ap_password_validity(void) {
  TEST_ASSERT_FALSE(access_ap_password_valid(nullptr));
  TEST_ASSERT_FALSE(access_ap_password_valid(""));
  TEST_ASSERT_FALSE(access_ap_password_valid("1234567"));
  TEST_ASSERT_TRUE(access_ap_password_valid("12345678"));
  TEST_ASSERT_TRUE(access_ap_password_valid("0123456789012345678901234567890123456789012345678901234567890123")); // 64
  TEST_ASSERT_FALSE(access_ap_password_valid("01234567890123456789012345678901234567890123456789012345678901234")); // 65
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_ap_interface_is_not_sta);
  RUN_TEST(test_other_local_address_is_sta);
  RUN_TEST(test_unknown_addresses_fail_closed);
  RUN_TEST(test_provisioned_ap_client_allowed_without_credentials);
  RUN_TEST(test_provisioned_sta_without_credentials_challenged);
  RUN_TEST(test_provisioned_sta_with_credentials_allowed);
  RUN_TEST(test_unprovisioned_sta_always_denied);
  RUN_TEST(test_unprovisioned_ap_only_setup_paths);
  RUN_TEST(test_setup_paths);
  RUN_TEST(test_analyzer_injection_gate);
  RUN_TEST(test_ap_password_validity);
  return UNITY_END();
}
