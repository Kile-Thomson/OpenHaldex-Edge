// Regression pin for the ESP_10 (0x116) E2E checksum DataID fix.
//
// Concern #4 in the upstream review: V8.00.2 filled ID_SEQ_116 with a verbatim
// copy of the EPB_01 (0x104) table - all 0x05 - instead of the VW MQB E2E DataID
// for ESP_10, which is 0xac. The 0x116 frame is the one that actually transmits
// (Calculations.cpp), so on a Gen5 (MQB) standalone spoof the receiving ECU
// rejected the frame for a checksum mismatch and the spoofed signal never landed.
//
// test_crc pins calcChecksum's *byte* output for 0x116, which is a refactor safety
// net: it goes red on any wire-byte change but does not say WHAT changed. This
// suite pins the fix at the DATA level: ID_SEQ_116 must hold the corrected 0xac
// values and must NOT be the 0x05 EPB_01 copy it was cloned from. A revert to the
// wrong table fails here by name, pointing straight at the DataID rather than at a
// diverging checksum golden.
//
// Bench note: 0xac is derived from the MQB frame layout and is not yet confirmed
// accepted on a real Gen5 bus (see the fork CHANGELOG / README). This test pins the
// intended corrected value, not a bus-observed one - if a bench capture ever shows
// a different DataID, update the expected constant here deliberately.

#include <unity.h>
#include <cstdint>

// Real DataID tables, defined in src/OpenHaldexC6_globals.cpp and compiled into
// the native binary alongside this test.
extern const uint8_t ID_SEQ_116[16]; // ESP_10  - corrected to the MQB 0xac DataID
extern const uint8_t ID_SEQ_104[16]; // EPB_01  - all 0x05, the table 0x116 was cloned from

void setUp(void) {}
void tearDown(void) {}

// ---- the fix: every alive-counter slot carries the MQB ESP_10 DataID 0xac ----
void test_esp10_dataid_is_0xac(void)
{
  for (uint8_t i = 0; i < 16; i++)
  {
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xac, ID_SEQ_116[i],
                                   "ID_SEQ_116 slot must be the MQB ESP_10 DataID 0xac");
  }
}

// ---- the regression: it must not be the EPB_01 (0x104) copy again -------------
// The upstream bug was a verbatim clone of the 0x104 table (all 0x05). Guard the
// exact revert: 0x116 and 0x104 must differ, and no 0x116 slot may be 0x05.
void test_esp10_dataid_is_not_the_epb01_copy(void)
{
  bool identical = true;
  for (uint8_t i = 0; i < 16; i++)
  {
    if (ID_SEQ_116[i] != ID_SEQ_104[i]) { identical = false; }
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0x05, ID_SEQ_116[i],
                                  "ID_SEQ_116 slot reverted to the 0x05 EPB_01 value");
  }
  TEST_ASSERT_FALSE_MESSAGE(identical,
                            "ID_SEQ_116 is a verbatim copy of ID_SEQ_104 again (the upstream bug)");
}

// ---- sanity: the table it was copied from is still the all-0x05 EPB_01 table --
// If this ever changes, the "not a copy" test above needs re-reading, so pin it.
void test_epb01_dataid_still_all_0x05(void)
{
  for (uint8_t i = 0; i < 16; i++)
  {
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x05, ID_SEQ_104[i],
                                   "ID_SEQ_104 (EPB_01) baseline changed");
  }
}

int main(int, char **)
{
  UNITY_BEGIN();
  RUN_TEST(test_esp10_dataid_is_0xac);
  RUN_TEST(test_esp10_dataid_is_not_the_epb01_copy);
  RUN_TEST(test_epb01_dataid_still_all_0x05);
  return UNITY_END();
}
