// Characterization tests for the Gen5 (MQB) Haldex UDS live-data decode:
//   * uds_parse_sf_rdbi()  - ISO-TP single-frame ReadDataByIdentifier parse
//   * uds_scale_mqb_did()  - per-DID raw -> engineering-value scaling
//
// The scaling formulas were recovered from the upstream V8.00.2 binary
// (tools/upstream-v8-decompile/FINDINGS.md, local only) and later confirmed
// against the author's V8.00.2 source; golden values below are hand-derived
// from those formulas independently of the implementation.
// The u16 byte order is mixed per DID, matching the upstream poller: the
// temperatures (0x2BF1, 0x2BE4) are little-endian, clutch current/voltage
// (0x2BE6, 0x2BE9) are big-endian.

#include <unity.h>
#include <cstdint>

#include <OpenHaldexC6_Calculations.h>

void setUp(void) {}
void tearDown(void) {}

// ===========================================================================
// uds_parse_sf_rdbi(data, dlc, did, out, out_cap)
// ===========================================================================

void test_parse_positive_u16_response(void)
{
  // 62 2B F1 + two data bytes, SF length 5, 0xAA tail padding as V8 sends.
  const uint8_t frame[8] = {0x05, 0x62, 0x2B, 0xF1, 0xBF, 0x60, 0xAA, 0xAA};
  uint8_t out[4] = {0};
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, uds_parse_sf_rdbi(frame, 8, 0x2BF1, out, sizeof(out)),
                                "parse positive u16 payload length");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xBF, out[0], "parse u16 first data byte");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x60, out[1], "parse u16 second data byte");
}

void test_parse_positive_single_byte_response(void)
{
  const uint8_t frame[8] = {0x04, 0x62, 0x02, 0x8D, 0x4B, 0xAA, 0xAA, 0xAA};
  uint8_t out[4] = {0};
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, uds_parse_sf_rdbi(frame, 8, 0x028D, out, sizeof(out)),
                                "parse positive 1-byte payload length");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x4B, out[0], "parse 1-byte data byte");
}

void test_parse_rejects_other_did(void)
{
  const uint8_t frame[8] = {0x05, 0x62, 0x2B, 0xF1, 0xBF, 0x60, 0xAA, 0xAA};
  uint8_t out[4];
  TEST_ASSERT_EQUAL_INT_MESSAGE(-1, uds_parse_sf_rdbi(frame, 8, 0x2BE4, out, sizeof(out)),
                                "response to a different DID must not match");
}

void test_parse_rejects_negative_response(void)
{
  // 7F 22 31 = ReadDataByIdentifier refused, requestOutOfRange.
  const uint8_t frame[8] = {0x03, 0x7F, 0x22, 0x31, 0xAA, 0xAA, 0xAA, 0xAA};
  uint8_t out[4];
  TEST_ASSERT_EQUAL_INT_MESSAGE(-1, uds_parse_sf_rdbi(frame, 8, 0x2BF1, out, sizeof(out)),
                                "negative response must be rejected");
}

void test_parse_rejects_session_response(void)
{
  // 50 03 = session-control positive response: SF length 2 < 3, no DID.
  const uint8_t frame[8] = {0x02, 0x50, 0x03, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA};
  uint8_t out[4];
  TEST_ASSERT_EQUAL_INT_MESSAGE(-1, uds_parse_sf_rdbi(frame, 8, 0x2BF1, out, sizeof(out)),
                                "session echo must not parse as a DID reply");
}

void test_parse_rejects_truncated_frame(void)
{
  // Declares 5 payload bytes (needs dlc >= 6) but the frame carries only 5.
  const uint8_t frame[5] = {0x05, 0x62, 0x2B, 0xF1, 0xBF};
  uint8_t out[4];
  TEST_ASSERT_EQUAL_INT_MESSAGE(-1, uds_parse_sf_rdbi(frame, 5, 0x2BF1, out, sizeof(out)),
                                "frame shorter than its declared SF length");
}

void test_parse_rejects_multi_frame_pci(void)
{
  const uint8_t frame[8] = {0x10, 0x0A, 0x62, 0x2B, 0xF1, 0xBF, 0x60, 0x00};
  uint8_t out[4];
  TEST_ASSERT_EQUAL_INT_MESSAGE(-1, uds_parse_sf_rdbi(frame, 8, 0x2BF1, out, sizeof(out)),
                                "first-frame PCI is out of scope");
}

void test_parse_rejects_undersized_out_buffer(void)
{
  const uint8_t frame[8] = {0x05, 0x62, 0x2B, 0xF1, 0xBF, 0x60, 0xAA, 0xAA};
  uint8_t out[1];
  TEST_ASSERT_EQUAL_INT_MESSAGE(-1, uds_parse_sf_rdbi(frame, 8, 0x2BF1, out, sizeof(out)),
                                "payload larger than out_cap: no partial copy");
}

void test_parse_zero_payload_returns_zero(void)
{
  // SF length exactly 3: valid header, no data bytes. Parses to 0; the scale
  // step then rejects the empty payload.
  const uint8_t frame[8] = {0x03, 0x62, 0x2B, 0xF1, 0xAA, 0xAA, 0xAA, 0xAA};
  uint8_t out[4];
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, uds_parse_sf_rdbi(frame, 8, 0x2BF1, out, sizeof(out)),
                                "header-only positive response parses to 0 bytes");
  float value = -1234.0f;
  TEST_ASSERT_FALSE_MESSAGE(uds_scale_mqb_did(0x2BF1, out, 0, value),
                            "scale rejects an empty payload");
}

// ===========================================================================
// uds_scale_mqb_did(did, payload, len, out)
// Golden values hand-derived from the recovered formulas.
// ===========================================================================

void test_scale_terminal_voltage(void)
{
  const uint8_t payload[1] = {123}; // 123 * 0.1 = 12.3 V
  float v = 0;
  TEST_ASSERT_TRUE(uds_scale_mqb_did(0x0286, payload, 1, v));
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(12.3f, v, "0x0286 terminalVoltage byte*0.1");
}

void test_scale_module_temp(void)
{
  const uint8_t payload[1] = {75}; // 75 - 55 = 20 C
  float v = 0;
  TEST_ASSERT_TRUE(uds_scale_mqb_did(0x028D, payload, 1, v));
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(20.0f, v, "0x028D moduleTemp byte-55");

  const uint8_t cold[1] = {0}; // 0 - 55 = -55 C (unsigned byte, offset only)
  TEST_ASSERT_TRUE(uds_scale_mqb_did(0x028D, cold, 1, v));
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(-55.0f, v, "0x028D moduleTemp floor");
}

void test_scale_clutch_temp(void)
{
  // u16 low-byte-first: {0xBF, 0x60} -> 0x60BF = 24767; (24767-22767)/100 = 20.0
  const uint8_t payload[2] = {0xBF, 0x60};
  float v = 0;
  TEST_ASSERT_TRUE(uds_scale_mqb_did(0x2BF1, payload, 2, v));
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(20.0f, v, "0x2BF1 clutchTemp (u16-22767)/100");
}

void test_scale_cooling_fin_temp_negative(void)
{
  // {0x2F, 0x4E} -> 0x4E2F = 20015; (20015-22767)/100 = -27.52 C
  const uint8_t payload[2] = {0x2F, 0x4E};
  float v = 0;
  TEST_ASSERT_TRUE(uds_scale_mqb_did(0x2BE4, payload, 2, v));
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(-27.52f, v, "0x2BE4 coolingFinTemp below zero");
}

void test_scale_clutch_current(void)
{
  // u16 big-endian: {0x0D, 0xAC} -> 0x0DAC = 3500; 3500 * 0.001 = 3.5 A
  const uint8_t payload[2] = {0x0D, 0xAC};
  float v = 0;
  TEST_ASSERT_TRUE(uds_scale_mqb_did(0x2BE6, payload, 2, v));
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(3.5f, v, "0x2BE6 clutchCurrent u16*0.001");
}

void test_scale_clutch_pwm_raw_byte(void)
{
  const uint8_t payload[1] = {47};
  float v = 0;
  TEST_ASSERT_TRUE(uds_scale_mqb_did(0x2BE7, payload, 1, v));
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(47.0f, v, "0x2BE7 clutchPWM raw byte");
}

void test_scale_clutch_voltage(void)
{
  // u16 big-endian: {0x30, 0xD4} -> 0x30D4 = 12500; 12500 * 0.001 = 12.5 V
  const uint8_t payload[2] = {0x30, 0xD4};
  float v = 0;
  TEST_ASSERT_TRUE(uds_scale_mqb_did(0x2BE9, payload, 2, v));
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(12.5f, v, "0x2BE9 clutchVoltage u16*0.001");
}

void test_scale_rejects_unknown_did(void)
{
  const uint8_t payload[2] = {0x00, 0x00};
  float v = -1234.0f;
  TEST_ASSERT_FALSE_MESSAGE(uds_scale_mqb_did(0x1234, payload, 2, v), "unknown DID");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(-1234.0f, v, "out untouched on unknown DID");
}

void test_scale_rejects_short_u16_payload(void)
{
  const uint8_t payload[1] = {0xBF};
  float v = -1234.0f;
  TEST_ASSERT_FALSE_MESSAGE(uds_scale_mqb_did(0x2BF1, payload, 1, v),
                            "u16 formula needs 2 bytes");
  TEST_ASSERT_EQUAL_FLOAT_MESSAGE(-1234.0f, v, "out untouched on short payload");
}

// ===========================================================================
// End-to-end: raw CAN frame -> parse -> scale, one per DID, as the poller runs.
// ===========================================================================

void test_frame_to_value_round_trip(void)
{
  struct Case
  {
    uint16_t did;
    uint8_t frame[8];
    float expected;
    const char *name;
  };
  const Case cases[] = {
      {0x0286, {0x04, 0x62, 0x02, 0x86, 123, 0xAA, 0xAA, 0xAA}, 12.3f, "terminalVoltage"},
      {0x028D, {0x04, 0x62, 0x02, 0x8D, 75, 0xAA, 0xAA, 0xAA}, 20.0f, "moduleTemp"},
      {0x2BF1, {0x05, 0x62, 0x2B, 0xF1, 0xBF, 0x60, 0xAA, 0xAA}, 20.0f, "clutchTemp"},
      {0x2BE4, {0x05, 0x62, 0x2B, 0xE4, 0x2F, 0x4E, 0xAA, 0xAA}, -27.52f, "coolingFinTemp"},
      {0x2BE6, {0x05, 0x62, 0x2B, 0xE6, 0x0D, 0xAC, 0xAA, 0xAA}, 3.5f, "clutchCurrent"},
      {0x2BE7, {0x04, 0x62, 0x2B, 0xE7, 47, 0xAA, 0xAA, 0xAA}, 47.0f, "clutchPWM"},
      {0x2BE9, {0x05, 0x62, 0x2B, 0xE9, 0x30, 0xD4, 0xAA, 0xAA}, 12.5f, "clutchVoltage"},
  };

  for (const Case &c : cases)
  {
    uint8_t payload[4] = {0};
    const int n = uds_parse_sf_rdbi(c.frame, 8, c.did, payload, sizeof(payload));
    TEST_ASSERT_MESSAGE(n > 0, c.name);
    float v = 0;
    TEST_ASSERT_TRUE_MESSAGE(uds_scale_mqb_did(c.did, payload, (uint8_t)n, v), c.name);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(c.expected, v, c.name);
  }
}

int main(int argc, char **argv)
{
  UNITY_BEGIN();

  RUN_TEST(test_parse_positive_u16_response);
  RUN_TEST(test_parse_positive_single_byte_response);
  RUN_TEST(test_parse_rejects_other_did);
  RUN_TEST(test_parse_rejects_negative_response);
  RUN_TEST(test_parse_rejects_session_response);
  RUN_TEST(test_parse_rejects_truncated_frame);
  RUN_TEST(test_parse_rejects_multi_frame_pci);
  RUN_TEST(test_parse_rejects_undersized_out_buffer);
  RUN_TEST(test_parse_zero_payload_returns_zero);

  RUN_TEST(test_scale_terminal_voltage);
  RUN_TEST(test_scale_module_temp);
  RUN_TEST(test_scale_clutch_temp);
  RUN_TEST(test_scale_cooling_fin_temp_negative);
  RUN_TEST(test_scale_clutch_current);
  RUN_TEST(test_scale_clutch_pwm_raw_byte);
  RUN_TEST(test_scale_clutch_voltage);
  RUN_TEST(test_scale_rejects_unknown_did);
  RUN_TEST(test_scale_rejects_short_u16_payload);

  RUN_TEST(test_frame_to_value_round_trip);

  return UNITY_END();
}
