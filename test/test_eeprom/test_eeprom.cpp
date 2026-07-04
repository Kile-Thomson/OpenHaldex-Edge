// Host-tested NVS init-policy suite (S04/T03).
//
// Pins the behaviour of the single pure predicate that decides what readEEP
// must do at boot (src/OpenHaldexC6_Calculations.cpp):
//   * eeprom_init_action(new_ns_seeded, legacy_ns_has_data) — given whether the
//     canonical "openhaldex" namespace already carries a "seeded" sentinel and
//     whether the de-facto legacy namespace ("learnTable") still holds a prior
//     install's data, returns LOAD / MIGRATE / SEED. The "seeded" check wins, so
//     a seeded device never re-reads legacy data even if it is still present.
//
// Expected values are derived independently from the policy, not read back from
// the implementation:
//
//   new_ns_seeded  legacy_ns_has_data -> eeprom_init_action
//   -------------  ------------------    -------------------
//   true           false              -> EEP_LOAD_EXISTING (normal seeded run)
//   true           true               -> EEP_LOAD_EXISTING (seeded wins over legacy)
//   false          true               -> EEP_MIGRATE_LEGACY (carry old device forward)
//   false          false              -> EEP_SEED_DEFAULTS  (first ever run, blank)
//
// A red assertion here means the first-run/migrate/seed policy drifted; do NOT
// edit a golden to make a refactor pass.
//
// The predicate is plain boolean logic and references no Arduino/NVS symbols, so
// it is host-executable via env:native.

#include <unity.h>

#include <OpenHaldexC6_Calculations.h>

// Real function under test (src/OpenHaldexC6_Calculations.cpp).
extern EepInitAction eeprom_init_action(bool new_ns_seeded, bool legacy_ns_has_data);

void setUp(void) {}
void tearDown(void) {}

// ===========================================================================
// eeprom_init_action — seeded sentinel wins; else migrate legacy; else seed
// ===========================================================================

void test_seeded_loads_existing(void)
{
  // (a) Canonical namespace already seeded, no legacy -> just load.
  TEST_ASSERT_EQUAL_MESSAGE(EEP_LOAD_EXISTING, eeprom_init_action(true, false),
                            "seeded + no legacy must LOAD existing");
}

void test_seeded_wins_over_legacy(void)
{
  // (b) Seeded sentinel present AND legacy data present -> seeded wins, no
  // re-migration of a device that has already been migrated/seeded.
  TEST_ASSERT_EQUAL_MESSAGE(EEP_LOAD_EXISTING, eeprom_init_action(true, true),
                            "seeded must win over legacy data and LOAD existing");
}

void test_unseeded_with_legacy_migrates(void)
{
  // (c) Not seeded but legacy holds a prior install -> migrate it forward.
  TEST_ASSERT_EQUAL_MESSAGE(EEP_MIGRATE_LEGACY, eeprom_init_action(false, true),
                            "unseeded + legacy data must MIGRATE legacy");
}

void test_blank_device_seeds_defaults(void)
{
  // (d) Neither seeded nor any legacy data -> first ever run, write defaults.
  TEST_ASSERT_EQUAL_MESSAGE(EEP_SEED_DEFAULTS, eeprom_init_action(false, false),
                            "blank device must SEED defaults");
}

// ===========================================================================

int main(int, char **)
{
  UNITY_BEGIN();

  RUN_TEST(test_seeded_loads_existing);
  RUN_TEST(test_seeded_wins_over_legacy);
  RUN_TEST(test_unseeded_with_legacy_migrates);
  RUN_TEST(test_blank_device_seeds_defaults);

  return UNITY_END();
}
