// Characterization tests for the OpenHaldex pure-function checksum core.
//
// These pin the CURRENT byte-level outputs of crc8_autosar() and
// calcChecksum() — the values the Haldex actually sees on the wire today.
// They are a refactor safety net: any change that alters a wire byte turns
// a named assertion red with the diverging expected-vs-actual value.
//
// Golden values are CHARACTERIZATION (the functions' present output), not a
// spec — they were recorded by running the real functions and pinning the
// result. Do not "fix" a golden to make a refactor pass; a red here means the
// refactor changed what the Haldex receives.

#include <unity.h>
#include <cstdint>

// Real functions under test — defined in src/OpenHaldexC6_globals.cpp,
// compiled into the native binary alongside this test.
extern uint8_t crc8_autosar(uint8_t *data, uint8_t len);
extern uint8_t calcChecksum(uint8_t *frame, const uint8_t *idSeq);

// Real DataID identification sequences (src/OpenHaldexC6_globals.cpp).
extern const uint8_t ID_SEQ_0A8[16]; // Motor_12
extern const uint8_t ID_SEQ_0A7[16]; // Motor_11
extern const uint8_t ID_SEQ_08A[16]; // ESP_14
extern const uint8_t ID_SEQ_106[16]; // ESP_05
extern const uint8_t ID_SEQ_116[16]; // ESP_10
extern const uint8_t ID_SEQ_65d[16]; // ESP_20

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// crc8_autosar — raw CRC8/AUTOSAR (poly 0x2F, init 0xFF, xorout 0xFF)
// ---------------------------------------------------------------------------

void test_crc8_empty_is_init_xor(void) {
  // len 0: no bytes processed -> 0xFF ^ 0xFF.
  uint8_t crc = crc8_autosar(nullptr, 0);
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, crc, "crc8_autosar({}, 0) wire byte changed");
}

void test_crc8_single_00(void) {
  uint8_t d[] = {0x00};
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xBD, crc8_autosar(d, 1),
                                 "crc8_autosar({0x00}) wire byte changed");
}

void test_crc8_single_FF(void) {
  uint8_t d[] = {0xFF};
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFF, crc8_autosar(d, 1),
                                 "crc8_autosar({0xFF}) wire byte changed");
}

void test_crc8_ascending_8(void) {
  uint8_t d[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xFC, crc8_autosar(d, 8),
                                 "crc8_autosar(0..7) wire byte changed");
}

void test_crc8_frame_like_8(void) {
  uint8_t d[] = {0x52, 0x70, 0x00, 0x00, 0x00, 0x64, 0x0F, 0xAE};
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x82, crc8_autosar(d, 8),
                                 "crc8_autosar(frame-like) wire byte changed");
}

void test_crc8_all_FF_8(void) {
  uint8_t d[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xC6, crc8_autosar(d, 8),
                                 "crc8_autosar({0xFF x8}) wire byte changed");
}

// ---------------------------------------------------------------------------
// calcChecksum — prepends idSeq[frame[1]&0x0F] then crc8_autosar over 8 bytes.
// Exercises several real ID sequences and several alive-counter values.
// ---------------------------------------------------------------------------

void test_checksum_0A8_counter0(void) {
  uint8_t f[] = {0x00, 0x70, 0x00, 0x00, 0x00, 0x64, 0x0F, 0xAE};
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x82, calcChecksum(f, ID_SEQ_0A8),
                                 "calcChecksum Motor_12 (0x0A8) wire byte changed");
}

void test_checksum_0A7_counter0(void) {
  uint8_t f[] = {0x00, 0x40, 0xFA, 0xFA, 0x00, 0xFA, 0x12, 0x34};
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x5F, calcChecksum(f, ID_SEQ_0A7),
                                 "calcChecksum Motor_11 (0x0A7) wire byte changed");
}

void test_checksum_0A7_counterA(void) {
  // frame[1]&0x0F == 0x0A -> indexes idSeq[10]; pins counter-dependent path.
  uint8_t f[] = {0x00, 0x4A, 0xFA, 0xFA, 0x00, 0xFA, 0xC8, 0xC8};
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x32, calcChecksum(f, ID_SEQ_0A7),
                                 "calcChecksum Motor_11 counter=0x0A wire byte changed");
}

void test_checksum_08A_counter0(void) {
  uint8_t f[] = {0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE};
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xDE, calcChecksum(f, ID_SEQ_08A),
                                 "calcChecksum ESP_14 (0x08A) wire byte changed");
}

void test_checksum_106_counter5(void) {
  uint8_t f[] = {0x00, 0x85, 0x64, 0xC0, 0x00, 0x00, 0xFD, 0x00};
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xE4, calcChecksum(f, ID_SEQ_106),
                                 "calcChecksum ESP_05 (0x106) wire byte changed");
}

void test_checksum_116_counter3(void) {
  uint8_t f[] = {0x00, 0x03, 0x01, 0x04, 0x00, 0x40, 0x00, 0x55};
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x80, calcChecksum(f, ID_SEQ_116),
                                 "calcChecksum ESP_10 (0x116) wire byte changed");
}

void test_checksum_65d_counterF(void) {
  uint8_t f[] = {0x00, 0x3F, 0x2B, 0x10, 0x00, 0x00, 0xE2, 0x79};
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x68, calcChecksum(f, ID_SEQ_65d),
                                 "calcChecksum ESP_20 (0x65d) wire byte changed");
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_crc8_empty_is_init_xor);
  RUN_TEST(test_crc8_single_00);
  RUN_TEST(test_crc8_single_FF);
  RUN_TEST(test_crc8_ascending_8);
  RUN_TEST(test_crc8_frame_like_8);
  RUN_TEST(test_crc8_all_FF_8);
  RUN_TEST(test_checksum_0A8_counter0);
  RUN_TEST(test_checksum_0A7_counter0);
  RUN_TEST(test_checksum_0A7_counterA);
  RUN_TEST(test_checksum_08A_counter0);
  RUN_TEST(test_checksum_106_counter5);
  RUN_TEST(test_checksum_116_counter3);
  RUN_TEST(test_checksum_65d_counterF);
  return UNITY_END();
}
