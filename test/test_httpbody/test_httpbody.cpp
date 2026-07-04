// Host-tested HTTP request-body buffer suite (S02/T01).
//
// These pin the behaviour of the two pure functions that own the malloc-backed
// request body buffer that parseJSON hands to ESPAsyncWebServer
// (src/OpenHaldexC6_Calculations.cpp):
//   * http_body_alloc(total)               — allocates one malloc'd block of
//     total + 1 bytes, fully zeroed with a NUL guaranteed at index `total`;
//     returns NULL for total == 0 (or on malloc failure) so the caller can fall
//     through to an empty-body path. A single plain free() releases it — this is
//     the whole point: it matches the library destructor's free(_tempObject),
//     unlike the old `new String()` (new/free mismatch + leaked heap buffer).
//   * http_body_write_chunk(buf,data,len,index,total) — bounds-checked memcpy of
//     a body chunk at `index`, never writing past `total` (the NUL guard is
//     preserved). No-ops for a null buffer/data, zero len, or index >= total.
//
// Expected values are derived independently from the contract above, not read
// back from the implementation: a two-chunk body must reconstruct the exact
// original string; an out-of-bounds chunk must leave the buffer byte-identical.
//
// A red assertion here means the body-buffer ownership contract drifted; do NOT
// edit a golden to make a refactor pass. Both functions are plain
// <cstdlib>/<cstring> logic and reference no Arduino/Async/TWAI symbols, so they
// are host-executable via env:native. The free() calls below run on the real
// helper's pointer to demonstrate the malloc/free match concretely.

#include <unity.h>
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include <OpenHaldexC6_Calculations.h>

// Real functions under test (src/OpenHaldexC6_Calculations.cpp).
extern char* http_body_alloc(size_t total);
extern void http_body_write_chunk(char* buf, const uint8_t* data, size_t len, size_t index, size_t total);

void setUp(void) {}
void tearDown(void) {}

// ===========================================================================
// http_body_alloc — single malloc'd, zeroed, NUL-terminated block
// ===========================================================================

void test_alloc_zero_returns_null(void)
{
  // (a) A zero-length body owns no buffer -> NULL so the caller can short-circuit.
  TEST_ASSERT_NULL_MESSAGE(http_body_alloc(0),
                           "http_body_alloc(0) must return NULL (no buffer for an empty body)");
}

void test_alloc_nonzero_is_nonnull_zeroed_and_terminated(void)
{
  // (b) A real length -> non-null block, every byte zero, NUL guard at index N.
  const size_t N = 16;
  char* buf = http_body_alloc(N);
  TEST_ASSERT_NOT_NULL_MESSAGE(buf, "http_body_alloc(N) must return a non-null block");

  for (size_t i = 0; i <= N; i++)
  {
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, (uint8_t)buf[i],
                                   "http_body_alloc: every byte through index total must be zero");
  }
  // The terminator slot at index `total` must be a NUL so the body reads as a
  // C-string even with no chunks written.
  TEST_ASSERT_EQUAL_CHAR_MESSAGE('\0', buf[N],
                                 "http_body_alloc: index total must hold the NUL terminator");

  free(buf); // single plain free() releases the whole malloc'd block
}

// ===========================================================================
// http_body_write_chunk — ordered chunks reconstruct the body
// ===========================================================================

void test_two_ordered_chunks_reconstruct_body(void)
{
  // (c) Two ordered chunks copied into the buffer must reconstruct the exact
  // original string, NUL-terminated at total.
  const char* part1 = "hello, ";   // 7 bytes
  const char* part2 = "world!";    // 6 bytes
  const size_t len1 = strlen(part1);
  const size_t len2 = strlen(part2);
  const size_t total = len1 + len2; // 13

  char* buf = http_body_alloc(total);
  TEST_ASSERT_NOT_NULL_MESSAGE(buf, "alloc for reconstruction test must succeed");

  http_body_write_chunk(buf, (const uint8_t*)part1, len1, 0, total);
  http_body_write_chunk(buf, (const uint8_t*)part2, len2, len1, total);

  TEST_ASSERT_EQUAL_STRING_MESSAGE("hello, world!", buf,
                                   "two ordered chunks must reconstruct the full body string");
  TEST_ASSERT_EQUAL_CHAR_MESSAGE('\0', buf[total],
                                 "reconstructed body must remain NUL-terminated at index total");

  free(buf);
}

// ===========================================================================
// http_body_write_chunk — out-of-bounds writes are ignored
// ===========================================================================

void test_out_of_bounds_chunk_is_ignored(void)
{
  // (d) A chunk whose index is at/after total must be dropped, leaving the
  // previously-written buffer byte-identical.
  const char* body = "abcd";       // 4 bytes
  const size_t total = strlen(body);

  char* buf = http_body_alloc(total);
  TEST_ASSERT_NOT_NULL_MESSAGE(buf, "alloc for out-of-bounds test must succeed");

  http_body_write_chunk(buf, (const uint8_t*)body, total, 0, total);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("abcd", buf, "in-bounds chunk must populate the buffer");

  // index == total -> at the NUL guard; must be a no-op.
  const char* overrun = "XYZ";
  http_body_write_chunk(buf, (const uint8_t*)overrun, strlen(overrun), total, total);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("abcd", buf,
                                   "chunk at index == total must be ignored (buffer unchanged)");

  // index beyond total -> still a no-op.
  http_body_write_chunk(buf, (const uint8_t*)overrun, strlen(overrun), total + 5, total);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("abcd", buf,
                                   "chunk at index > total must be ignored (buffer unchanged)");
  TEST_ASSERT_EQUAL_CHAR_MESSAGE('\0', buf[total],
                                 "NUL guard at index total must survive out-of-bounds writes");

  free(buf);
}

// ===========================================================================
// http_body_write_chunk — an overrunning chunk is truncated, not overflowed
// ===========================================================================

void test_overrunning_chunk_is_truncated(void)
{
  // (e) A chunk whose len would run past total must be truncated to fit, leaving
  // the NUL guard intact rather than writing outside the allocation.
  const size_t total = 4;
  char* buf = http_body_alloc(total);
  TEST_ASSERT_NOT_NULL_MESSAGE(buf, "alloc for truncation test must succeed");

  // Write 6 bytes starting at index 2: only 2 (indices 2,3) may land.
  const char* data = "ABCDEF";
  http_body_write_chunk(buf, (const uint8_t*)data, strlen(data), 2, total);

  // Indices 0,1 stay zero; 2,3 hold 'A','B'; the NUL guard at index 4 survives.
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, (uint8_t)buf[0], "byte 0 must stay zero");
  TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, (uint8_t)buf[1], "byte 1 must stay zero");
  TEST_ASSERT_EQUAL_CHAR_MESSAGE('A', buf[2], "byte 2 must hold the first written char");
  TEST_ASSERT_EQUAL_CHAR_MESSAGE('B', buf[3], "byte 3 must hold the second written char");
  TEST_ASSERT_EQUAL_CHAR_MESSAGE('\0', buf[total],
                                 "NUL guard at index total must survive a truncated chunk");

  free(buf);
}

// ===========================================================================
// http_body_write_chunk — null/empty arguments are safe no-ops
// ===========================================================================

void test_null_and_empty_args_are_noops(void)
{
  // (f) A null buffer or null data must not crash; a zero-length chunk must not
  // alter the buffer.
  const uint8_t src[3] = {0x11, 0x22, 0x33};

  // Null buffer: must simply return (no dereference).
  http_body_write_chunk(nullptr, src, sizeof(src), 0, 8);

  const size_t total = 3;
  char* buf = http_body_alloc(total);
  TEST_ASSERT_NOT_NULL_MESSAGE(buf, "alloc for null/empty test must succeed");

  // Null data into a real buffer: no-op.
  http_body_write_chunk(buf, nullptr, sizeof(src), 0, total);
  // Zero-length chunk: no-op.
  http_body_write_chunk(buf, src, 0, 0, total);

  for (size_t i = 0; i <= total; i++)
  {
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00, (uint8_t)buf[i],
                                   "null-data / zero-len chunk must leave the buffer untouched");
  }

  free(buf);
}

// ===========================================================================
// Allocation/free match — the single-block, plain-free() contract
// ===========================================================================

void test_single_block_released_by_plain_free(void)
{
  // (g) The whole allocation is a single block released by plain free() — the
  // exact match ESPAsyncWebServer's destructor relies on. Allocate, fill the
  // full body, and free() the real helper's pointer to demonstrate the match.
  const size_t total = 32;
  char* buf = http_body_alloc(total);
  TEST_ASSERT_NOT_NULL_MESSAGE(buf, "alloc for free()-match test must succeed");

  const char* data = "0123456789abcdef0123456789abcdef"; // exactly 32 bytes
  http_body_write_chunk(buf, (const uint8_t*)data, total, 0, total);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("0123456789abcdef0123456789abcdef", buf,
                                   "full-length body must populate the entire block");

  free(buf); // matches the library's free(_tempObject); no leak, no mismatch
  TEST_PASS_MESSAGE("malloc'd body buffer released cleanly by a single plain free()");
}

// ===========================================================================

int main(int, char **)
{
  UNITY_BEGIN();

  RUN_TEST(test_alloc_zero_returns_null);
  RUN_TEST(test_alloc_nonzero_is_nonnull_zeroed_and_terminated);
  RUN_TEST(test_two_ordered_chunks_reconstruct_body);
  RUN_TEST(test_out_of_bounds_chunk_is_ignored);
  RUN_TEST(test_overrunning_chunk_is_truncated);
  RUN_TEST(test_null_and_empty_args_are_noops);
  RUN_TEST(test_single_block_released_by_plain_free);

  return UNITY_END();
}
