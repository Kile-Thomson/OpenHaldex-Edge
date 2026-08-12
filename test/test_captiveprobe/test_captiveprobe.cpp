// Tests for is_captive_probe() in src/OpenHaldexC6_Calculations.cpp.
//
// The device is an offline car AP with no uplink. When a phone joins it, the
// phone OS fires a connectivity probe ("is there internet here?"). If the web
// server answers that probe wrong - a 204 (says "internet works") or a redirect
// (looks like a captive portal) - the phone either routes everything through us
// and goes dark, or shows "Sign in to WiFi". Either way the user loses cellular
// messages, calls, and the ability to fetch OTA files.
//
// is_captive_probe() picks those probe URLs out of the onNotFound handler so it
// can answer them with a bare 404 ("no internet, not captive"), which keeps the
// phone's cellular alive. These tests pin exactly which paths count as probes
// and the match rules (case-insensitive, query-string tolerant, no prefix
// false-positives), since a drift there silently reintroduces the dark-phone bug.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Calculations.h>

void setUp(void) {}
void tearDown(void) {}

// ---- the known probe paths all match ---------------------------------------

void test_android_generate_204(void)
{
  TEST_ASSERT_TRUE_MESSAGE(is_captive_probe("/generate_204"),
                           "Android /generate_204 must be treated as a probe");
}

void test_android_gen_204(void)
{
  TEST_ASSERT_TRUE_MESSAGE(is_captive_probe("/gen_204"),
                           "Android /gen_204 must be treated as a probe");
}

void test_apple_hotspot_detect(void)
{
  TEST_ASSERT_TRUE_MESSAGE(is_captive_probe("/hotspot-detect.html"),
                           "Apple /hotspot-detect.html must be treated as a probe");
}

void test_apple_library_success(void)
{
  TEST_ASSERT_TRUE_MESSAGE(is_captive_probe("/library/test/success.html"),
                           "Apple /library/test/success.html must be treated as a probe");
}

void test_windows_ncsi(void)
{
  TEST_ASSERT_TRUE_MESSAGE(is_captive_probe("/ncsi.txt"),
                           "Windows /ncsi.txt must be treated as a probe");
}

void test_windows_connecttest(void)
{
  TEST_ASSERT_TRUE_MESSAGE(is_captive_probe("/connecttest.txt"),
                           "Windows /connecttest.txt must be treated as a probe");
}

void test_firefox_canonical(void)
{
  TEST_ASSERT_TRUE_MESSAGE(is_captive_probe("/canonical.html"),
                           "Firefox /canonical.html must be treated as a probe");
}

// ---- match rules ------------------------------------------------------------

void test_case_insensitive(void)
{
  TEST_ASSERT_TRUE_MESSAGE(is_captive_probe("/GENERATE_204"),
                           "probe match must be case-insensitive");
  TEST_ASSERT_TRUE_MESSAGE(is_captive_probe("/Hotspot-Detect.HTML"),
                           "mixed-case probe path must still match");
}

void test_query_string_tolerated(void)
{
  TEST_ASSERT_TRUE_MESSAGE(is_captive_probe("/generate_204?foo=bar"),
                           "a trailing query string must not defeat the match");
}

// ---- non-probe paths must NOT match (guards the app's real routes) ----------

void test_prefix_does_not_false_match(void)
{
  // A real route that merely starts with a probe name must not be swallowed.
  TEST_ASSERT_FALSE_MESSAGE(is_captive_probe("/generate_204_extra"),
                            "a longer path sharing the probe prefix must not match");
}

void test_real_routes_not_probes(void)
{
  TEST_ASSERT_FALSE_MESSAGE(is_captive_probe("/"),
                            "the dashboard root is not a probe");
  TEST_ASSERT_FALSE_MESSAGE(is_captive_probe("/index.html"),
                            "the dashboard page is not a probe");
  TEST_ASSERT_FALSE_MESSAGE(is_captive_probe("/api/dashboard"),
                            "an API route is not a probe");
  TEST_ASSERT_FALSE_MESSAGE(is_captive_probe("/setup"),
                            "the first-run setup page is not a probe");
}

void test_null_is_not_probe(void)
{
  TEST_ASSERT_FALSE_MESSAGE(is_captive_probe(nullptr),
                            "a NULL path must be safe and not a probe");
}

int main(int, char **)
{
  UNITY_BEGIN();
  RUN_TEST(test_android_generate_204);
  RUN_TEST(test_android_gen_204);
  RUN_TEST(test_apple_hotspot_detect);
  RUN_TEST(test_apple_library_success);
  RUN_TEST(test_windows_ncsi);
  RUN_TEST(test_windows_connecttest);
  RUN_TEST(test_firefox_canonical);
  RUN_TEST(test_case_insensitive);
  RUN_TEST(test_query_string_tolerated);
  RUN_TEST(test_prefix_does_not_false_match);
  RUN_TEST(test_real_routes_not_probes);
  RUN_TEST(test_null_is_not_probe);
  return UNITY_END();
}
