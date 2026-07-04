// Host-tested OTA credential-policy suite (S04/T01, per D006).
//
// These pin the behaviour of the two pure functions that decide whether the OTA
// flash surface may proceed (src/OpenHaldexC6_Calculations.cpp):
//   * select_ota_password(nvs_pw, build_default) — resolves the effective OTA
//     password with NVS-over-build-default-over-empty precedence, treating NULL
//     or "" at either source as "unset". Never returns NULL.
//   * ota_credential_configured(effective_pw)   — true only for a non-NULL,
//     non-empty credential; the flash surface fails closed (HTTP 503) otherwise.
//
// Expected values are derived independently from the precedence table below, not
// read back from the implementation:
//
//   nvs_pw      build_default   -> select_ota_password
//   ----------- --------------- ----------------------
//   "nvssecret" (any)           -> "nvssecret"   (NVS wins)
//   NULL/""     "builtdefault"  -> "builtdefault"(falls back to build default)
//   NULL/""     NULL/""         -> ""            (fail-closed sentinel)
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
extern const char* select_ota_password(const char* nvs_pw, const char* build_default);
extern bool ota_credential_configured(const char* effective_pw);

void setUp(void) {}
void tearDown(void) {}

// ===========================================================================
// select_ota_password — precedence: NVS > build-default > "" (fail-closed)
// ===========================================================================

void test_nvs_password_wins_over_build_default(void)
{
  // (a) A real NVS value takes precedence over any build-time default.
  const char* eff = select_ota_password("nvssecret", "builtdefault");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("nvssecret", eff,
                                   "select_ota_password: NVS value must win over build default");
}

void test_nvs_password_wins_even_when_build_default_null(void)
{
  // (b) NVS value used even if no build default is compiled in.
  const char* eff = select_ota_password("nvssecret", nullptr);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("nvssecret", eff,
                                   "select_ota_password: NVS value must be used when build default is NULL");
}

void test_null_nvs_falls_back_to_build_default(void)
{
  // (c) NULL NVS -> the build-time default supplies the credential.
  const char* eff = select_ota_password(nullptr, "builtdefault");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("builtdefault", eff,
                                   "select_ota_password: NULL NVS must fall back to build default");
}

void test_empty_nvs_falls_back_to_build_default(void)
{
  // (d) Empty-string NVS is treated as unset -> build default supplies it.
  const char* eff = select_ota_password("", "builtdefault");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("builtdefault", eff,
                                   "select_ota_password: empty NVS must fall back to build default");
}

void test_both_null_returns_empty_string(void)
{
  // (e) Boundary/fail-closed: neither source set -> "" sentinel, never NULL.
  const char* eff = select_ota_password(nullptr, nullptr);
  TEST_ASSERT_NOT_NULL_MESSAGE(eff, "select_ota_password: must never return NULL");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", eff,
                                   "select_ota_password: both sources unset must yield empty string");
}

void test_both_empty_returns_empty_string(void)
{
  // (f) Both empty strings -> still the "" fail-closed sentinel.
  const char* eff = select_ota_password("", "");
  TEST_ASSERT_EQUAL_STRING_MESSAGE("", eff,
                                   "select_ota_password: both empty must yield empty string");
}

// ===========================================================================
// ota_credential_configured — true only for a non-NULL, non-empty credential
// ===========================================================================

void test_configured_false_for_null(void)
{
  // (g) NULL -> not configured (flash must fail closed).
  TEST_ASSERT_FALSE_MESSAGE(ota_credential_configured(nullptr),
                            "ota_credential_configured: NULL must be not-configured");
}

void test_configured_false_for_empty(void)
{
  // (h) "" -> not configured (the fail-closed sentinel from select_ota_password).
  TEST_ASSERT_FALSE_MESSAGE(ota_credential_configured(""),
                            "ota_credential_configured: empty string must be not-configured");
}

void test_configured_true_for_real_credential(void)
{
  // (i) A real, non-empty credential -> configured (flash may proceed to auth).
  TEST_ASSERT_TRUE_MESSAGE(ota_credential_configured("nvssecret"),
                           "ota_credential_configured: real credential must be configured");
}

void test_configured_matches_select_output(void)
{
  // (j) Integration: the sentinel from select_ota_password with no sources set
  // must read as not-configured, closing the loop between the two functions.
  TEST_ASSERT_FALSE_MESSAGE(ota_credential_configured(select_ota_password(nullptr, nullptr)),
                            "ota_credential_configured(select_ota_password(NULL,NULL)) must be false");
  TEST_ASSERT_TRUE_MESSAGE(ota_credential_configured(select_ota_password("nvssecret", nullptr)),
                           "ota_credential_configured(select_ota_password(\"nvssecret\",NULL)) must be true");
}

// ===========================================================================

int main(int, char **)
{
  UNITY_BEGIN();

  RUN_TEST(test_nvs_password_wins_over_build_default);
  RUN_TEST(test_nvs_password_wins_even_when_build_default_null);
  RUN_TEST(test_null_nvs_falls_back_to_build_default);
  RUN_TEST(test_empty_nvs_falls_back_to_build_default);
  RUN_TEST(test_both_null_returns_empty_string);
  RUN_TEST(test_both_empty_returns_empty_string);
  RUN_TEST(test_configured_false_for_null);
  RUN_TEST(test_configured_false_for_empty);
  RUN_TEST(test_configured_true_for_real_credential);
  RUN_TEST(test_configured_matches_select_output);

  return UNITY_END();
}
