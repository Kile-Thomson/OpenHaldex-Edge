// Host-tested OTA credential-policy suite.
//
// These pin the behaviour of the two pure functions that decide whether the OTA
// flash surface may proceed (src/OpenHaldexC6_Calculations.cpp):
//   * select_ota_password(nvs_pw) — resolves the effective OTA password from the
//     runtime NVS-provisioned value, treating NULL or "" as "unset". There is no
//     build-time default; a credential is only ever set at runtime via /setup.
//     Never returns NULL.
//   * ota_credential_configured(effective_pw) — true only for a non-NULL,
//     non-empty credential; the flash surface fails closed (HTTP 503) otherwise.
//
// Expected values are derived independently from the policy below, not read back
// from the implementation:
//
//   nvs_pw      -> select_ota_password
//   ----------- ----------------------
//   "nvssecret" -> "nvssecret"  (a stored credential is used)
//   NULL/""     -> ""           (fail-closed sentinel)
//
// A red assertion here means the flash-authorization policy drifted; do NOT edit
// a golden to make a refactor pass.
//
// Both functions are plain pointer logic and reference no Arduino/NVS/TWAI
// symbols, so they are host-executable via env:native.

#include <unity.h>
#include <cstring>

#include <OpenHaldexC6_Calculations.h>

// Real functions under test (src/OpenHaldexC6_Calculations.cpp).
extern const char* select_ota_password(const char* nvs_pw);
extern bool ota_credential_configured(const char* effective_pw);

void setUp(void) {}
void tearDown(void) {}

// ===========================================================================
// select_ota_password — NVS value or "" (fail-closed)
// ===========================================================================

void test_stored_credential_is_used(void)
{
  // (a) A real NVS value is used verbatim.
  const char* eff = select_ota_password("nvssecret");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("nvssecret", eff,
                                   "select_ota_password: stored NVS value must be used");
}

void test_null_nvs_returns_empty_string(void)
{
  // (b) NULL NVS -> "" sentinel, never NULL (nothing else to fall back to).
  const char* eff = select_ota_password(nullptr);
  TEST_ASSERT_NOT_NULL_MESSAGE(eff, "select_ota_password: must never return NULL");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", eff,
                                   "select_ota_password: NULL NVS must yield empty string");
}

void test_empty_nvs_returns_empty_string(void)
{
  // (c) Empty-string NVS is treated as unset -> "" fail-closed sentinel.
  const char* eff = select_ota_password("");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", eff,
                                   "select_ota_password: empty NVS must yield empty string");
}

// ===========================================================================
// ota_credential_configured — true only for a non-NULL, non-empty credential
// ===========================================================================

void test_configured_false_for_null(void)
{
  // (d) NULL -> not configured (flash must fail closed).
  TEST_ASSERT_FALSE_MESSAGE(ota_credential_configured(nullptr),
                            "ota_credential_configured: NULL must be not-configured");
}

void test_configured_false_for_empty(void)
{
  // (e) "" -> not configured (the fail-closed sentinel from select_ota_password).
  TEST_ASSERT_FALSE_MESSAGE(ota_credential_configured(""),
                            "ota_credential_configured: empty string must be not-configured");
}

void test_configured_true_for_real_credential(void)
{
  // (f) A real, non-empty credential -> configured (flash may proceed to auth).
  TEST_ASSERT_TRUE_MESSAGE(ota_credential_configured("nvssecret"),
                           "ota_credential_configured: real credential must be configured");
}

void test_configured_matches_select_output(void)
{
  // (g) Integration: the sentinel from select_ota_password with no NVS value
  // must read as not-configured, closing the loop between the two functions.
  TEST_ASSERT_FALSE_MESSAGE(ota_credential_configured(select_ota_password(nullptr)),
                            "ota_credential_configured(select_ota_password(NULL)) must be false");
  TEST_ASSERT_TRUE_MESSAGE(ota_credential_configured(select_ota_password("nvssecret")),
                           "ota_credential_configured(select_ota_password(\"nvssecret\")) must be true");
}

// ===========================================================================

int main(int, char **)
{
  UNITY_BEGIN();

  RUN_TEST(test_stored_credential_is_used);
  RUN_TEST(test_null_nvs_returns_empty_string);
  RUN_TEST(test_empty_nvs_returns_empty_string);
  RUN_TEST(test_configured_false_for_null);
  RUN_TEST(test_configured_false_for_empty);
  RUN_TEST(test_configured_true_for_real_credential);
  RUN_TEST(test_configured_matches_select_output);

  return UNITY_END();
}
