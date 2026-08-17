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

// ---- a generation change must never alter the stored drive mode --------------
// Pins last_mode_after_generation_change(), the seam settingsIncoming() routes
// lastMode through when the haldexGeneration is set. This is the regression at
// its source: reintroducing `lastMode = generation` means editing this seam to
// return `generation`, which reddens these assertions instead of shipping
// silently. Every valid generation number is checked against every stored mode.

void test_generation_change_preserves_mode(void)
{
  const int generations[] = {1, 2, 4, 41, 50, 51};
  for (uint8_t mode = 0; mode <= 5; mode++)
  {
    for (unsigned g = 0; g < sizeof(generations) / sizeof(generations[0]); g++)
    {
      TEST_ASSERT_EQUAL_UINT8_MESSAGE(
          mode, last_mode_after_generation_change(mode, generations[g]),
          "setting the haldex generation must leave the stored drive mode unchanged");
    }
  }
}

// ---- the boot mapping composed with the write-back self-heals a corrupt byte -
// EEP.cpp does `state.mode = mode_from_last_mode(lastMode); lastMode = state.mode;`.
// Compose the same two steps here: a corrupt stored byte (a generation number)
// must both boot into a valid mode AND be normalized to a valid 0-5 value that
// the next writeEEP persists - so the corruption cannot leak through
// settingsOutgoing (data["mode"] = lastMode) or the standalone mode-0 cast.

void test_corrupt_byte_normalizes_on_boot(void)
{
  const uint8_t corrupt[] = {41, 50, 51, 6, 255};
  for (unsigned i = 0; i < sizeof(corrupt) / sizeof(corrupt[0]); i++)
  {
    uint8_t stored = corrupt[i];
    openhaldex_mode_t booted = mode_from_last_mode(stored); // boot decode
    stored = (uint8_t)booted;                               // write-back
    TEST_ASSERT_EQUAL_MESSAGE(MODE_FWD, booted,
                              "a corrupt stored byte must boot into MODE_FWD");
    TEST_ASSERT_TRUE_MESSAGE(stored <= 5,
                             "the written-back lastMode must be a valid 0-5 value");
    // And the normalized byte must round-trip to the same mode it booted into.
    TEST_ASSERT_EQUAL_MESSAGE(booted, mode_from_last_mode(stored),
                              "normalized byte must round-trip to the same mode");
  }
}

int main(int, char **)
{
  UNITY_BEGIN();
  RUN_TEST(test_valid_modes_map_exactly);
  RUN_TEST(test_generation_values_fall_back);
  RUN_TEST(test_out_of_range_falls_back);
  RUN_TEST(test_generation_change_preserves_mode);
  RUN_TEST(test_corrupt_byte_normalizes_on_boot);
  return UNITY_END();
}
