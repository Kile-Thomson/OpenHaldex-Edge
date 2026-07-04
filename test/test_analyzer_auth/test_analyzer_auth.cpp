// Host-tested analyzer-injection authorization suite (S03/T01).
//
// Pins the behaviour of the single pure predicate that decides whether the
// analyzer TCP port (GVRET/SLCAN) may inject host->device CAN frames
// (src/OpenHaldexC6_Calculations.cpp):
//   * analyzer_injection_allowed(effective_pw) — true only when a credential is
//     configured. It composes ota_credential_configured(effective_pw), so the
//     fail-closed injection policy has one source of truth. When false, the
//     analyzer transmit paths MUST drop the frame; passive sniffing is unaffected.
//
// Expected values are derived independently from the policy, not read back from
// the implementation:
//
//   effective_pw                       -> analyzer_injection_allowed
//   ---------------------------------- -----------------------------
//   NULL                               -> false (fail-closed: no credential)
//   ""                                 -> false (empty == unset == unprovisioned)
//   "nvssecret"                        -> true  (a real credential authorizes)
//   select_ota_password(NULL)          -> false (sentinel "" from the OTA policy)
//
// A red assertion here means the fail-closed injection policy drifted; do NOT
// edit a golden to make a refactor pass.
//
// The predicate is plain pointer logic and references no Arduino/NVS/TWAI
// symbols, so it is host-executable via env:native.

#include <unity.h>
#include <cstring>

#include <OpenHaldexC6_Calculations.h>

// Real functions under test (src/OpenHaldexC6_Calculations.cpp).
extern bool analyzer_injection_allowed(const char* effective_pw);
extern const char* select_ota_password(const char* nvs_pw);

void setUp(void) {}
void tearDown(void) {}

// ===========================================================================
// analyzer_injection_allowed — true only for a configured credential
// ===========================================================================

void test_injection_refused_for_null(void)
{
  // (a) NULL -> no credential -> injection must be refused (fail closed).
  TEST_ASSERT_FALSE_MESSAGE(analyzer_injection_allowed(nullptr),
                            "analyzer_injection_allowed: NULL must refuse injection");
}

void test_injection_refused_for_empty(void)
{
  // (b) "" -> unprovisioned sentinel -> injection must be refused.
  TEST_ASSERT_FALSE_MESSAGE(analyzer_injection_allowed(""),
                            "analyzer_injection_allowed: empty string must refuse injection");
}

void test_injection_allowed_for_real_credential(void)
{
  // (c) A real, non-empty credential -> injection authorized.
  TEST_ASSERT_TRUE_MESSAGE(analyzer_injection_allowed("nvssecret"),
                           "analyzer_injection_allowed: real credential must allow injection");
}

void test_injection_refused_for_unprovisioned_select_output(void)
{
  // (d) Integration: the "" sentinel returned by select_ota_password when no
  // credential is provisioned must read as injection-refused, closing the loop
  // between the OTA credential policy and the analyzer injection gate.
  TEST_ASSERT_FALSE_MESSAGE(analyzer_injection_allowed(select_ota_password(nullptr)),
                            "analyzer_injection_allowed(select_ota_password(NULL)) must be false");
}

// ===========================================================================

int main(int, char **)
{
  UNITY_BEGIN();

  RUN_TEST(test_injection_refused_for_null);
  RUN_TEST(test_injection_refused_for_empty);
  RUN_TEST(test_injection_allowed_for_real_credential);
  RUN_TEST(test_injection_refused_for_unprovisioned_select_output);

  return UNITY_END();
}
