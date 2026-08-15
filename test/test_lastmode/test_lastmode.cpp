// Host-tested boot-mode mapping suite.
//
// Pins mode_from_last_mode() in src/OpenHaldexC6_Calculations.cpp - the pure
// mapping readEEP() uses at boot to turn the persisted lastMode byte into the
// runtime drive-mode enum.
//
// Background / the bug this guards: a stored lastMode is meant to be 0..5
// (Stock/FWD/5050/6040/7525/Expert). The settings API once wrote the
// haldexGeneration number (1/2/4/41/50/51) into lastMode by mistake. On the next
// boot, a value like 41 or 50 is not a valid mode, so the device silently reset
// the driver's selected mode to MODE_FWD ("forgets its drive mode on reboot").
// The API bug is fixed at the source, and these tests pin the boot mapping so a
// stray out-of-range byte can never masquerade as a real mode again.
//
// The mapping references no Arduino/NVS symbols, so it is host-executable via
// env:native.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Calculations.h>

void setUp(void) {}
void tearDown(void) {}

// ---- the six valid stored values map 1:1 to their enum -----------------------

void test_valid_modes_map_exactly(void)
{
  TEST_ASSERT_EQUAL_MESSAGE(MODE_STOCK, mode_from_last_mode(0), "0 must map to MODE_STOCK");
  TEST_ASSERT_EQUAL_MESSAGE(MODE_FWD, mode_from_last_mode(1), "1 must map to MODE_FWD");
  TEST_ASSERT_EQUAL_MESSAGE(MODE_5050, mode_from_last_mode(2), "2 must map to MODE_5050");
  TEST_ASSERT_EQUAL_MESSAGE(MODE_6040, mode_from_last_mode(3), "3 must map to MODE_6040");
  TEST_ASSERT_EQUAL_MESSAGE(MODE_7525, mode_from_last_mode(4), "4 must map to MODE_7525");
  TEST_ASSERT_EQUAL_MESSAGE(MODE_EXPERT, mode_from_last_mode(5), "5 must map to MODE_EXPERT");
}

// ---- generation-namespace values must NOT masquerade as a mode ---------------
// These are the exact haldexGeneration numbers the old bug wrote into lastMode.
// Each must fall back to MODE_FWD, never be treated as a valid stored mode.

void test_generation_values_fall_back(void)
{
  TEST_ASSERT_EQUAL_MESSAGE(MODE_FWD, mode_from_last_mode(41),
                            "haldexGeneration 41 must not be a valid mode");
  TEST_ASSERT_EQUAL_MESSAGE(MODE_FWD, mode_from_last_mode(50),
                            "haldexGeneration 50 must not be a valid mode");
  TEST_ASSERT_EQUAL_MESSAGE(MODE_FWD, mode_from_last_mode(51),
                            "haldexGeneration 51 must not be a valid mode");
}

// ---- other out-of-range bytes also fall back safely --------------------------

void test_out_of_range_falls_back(void)
{
  TEST_ASSERT_EQUAL_MESSAGE(MODE_FWD, mode_from_last_mode(6),
                            "6 is just past the valid range and must fall back");
  TEST_ASSERT_EQUAL_MESSAGE(MODE_FWD, mode_from_last_mode(255),
                            "an erased-NVS 0xFF byte must fall back to MODE_FWD");
}

int main(int, char **)
{
  UNITY_BEGIN();
  RUN_TEST(test_valid_modes_map_exactly);
  RUN_TEST(test_generation_values_fall_back);
  RUN_TEST(test_out_of_range_falls_back);
  return UNITY_END();
}
