// Tests for the OTA upload decision core (include/OpenHaldexC6_OTARoute.h):
// image classification from the first upload chunk, and the chunk-to-region
// routing used to split a merged flash image into its app and filesystem
// segments. A wrong answer here writes to the wrong flash partition, so every
// boundary is pinned. Layout constants mirror src/partitions_4mb.csv.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include <OpenHaldexC6_OTARoute.h>

// Flash layout from src/partitions_4mb.csv (4 MB part).
static const size_t APP_SLOT_SIZE = 0x1A0000; // ota_0 / ota_1 size
static const size_t OTA0_ADDR     = 0x10000;
static const size_t FS_ADDR       = 0x350000;
static const size_t FS_SIZE       = 0xB0000;

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// ota_image_is_littlefs - superblock magic "littlefs" at byte offset 8
// ---------------------------------------------------------------------------

// First 16 bytes of a real mklittlefs image built by this project.
static const uint8_t LFS_HEAD[16] = {
  0x01, 0x00, 0x00, 0x00, 0xF0, 0x0F, 0xFF, 0xF7,
  'l', 'i', 't', 't', 'l', 'e', 'f', 's'
};

void test_littlefs_magic_accepted(void) {
  TEST_ASSERT_TRUE(ota_image_is_littlefs(LFS_HEAD, sizeof(LFS_HEAD)));
}

void test_littlefs_short_buffer_rejected(void) {
  TEST_ASSERT_FALSE(ota_image_is_littlefs(LFS_HEAD, 15)); // magic incomplete
  TEST_ASSERT_FALSE(ota_image_is_littlefs(LFS_HEAD, 0));
  TEST_ASSERT_FALSE(ota_image_is_littlefs(nullptr, sizeof(LFS_HEAD)));
}

void test_littlefs_wrong_magic_rejected(void) {
  uint8_t head[16];
  memcpy(head, LFS_HEAD, sizeof(head));
  head[8] = 'L'; // case matters
  TEST_ASSERT_FALSE(ota_image_is_littlefs(head, sizeof(head)));
}

void test_firmware_image_is_not_littlefs(void) {
  uint8_t head[16] = {0xE9};
  TEST_ASSERT_FALSE(ota_image_is_littlefs(head, sizeof(head)));
}

// ---------------------------------------------------------------------------
// ota_classify_upload
// ---------------------------------------------------------------------------

void test_classify_littlefs_image(void) {
  // Size does not matter for a littlefs image - the magic decides.
  TEST_ASSERT_EQUAL(OTA_KIND_FS,
    ota_classify_upload(LFS_HEAD, sizeof(LFS_HEAD), FS_SIZE + 500, APP_SLOT_SIZE, FS_ADDR));
}

void test_classify_bare_firmware(void) {
  uint8_t head[16] = {0xE9};
  // Typical firmware upload: ~1.2 MB + multipart framing, under the slot size.
  TEST_ASSERT_EQUAL(OTA_KIND_APP,
    ota_classify_upload(head, sizeof(head), 1224507 + 400, APP_SLOT_SIZE, FS_ADDR));
  // Exactly slot-sized still counts as firmware.
  TEST_ASSERT_EQUAL(OTA_KIND_APP,
    ota_classify_upload(head, sizeof(head), APP_SLOT_SIZE, APP_SLOT_SIZE, FS_ADDR));
}

void test_classify_merged_image(void) {
  uint8_t head[16] = {0xE9}; // merged image starts with the bootloader (0xE9)
  // A real merged image ends at the end of the fs segment: 4 MB flat.
  TEST_ASSERT_EQUAL(OTA_KIND_MERGED,
    ota_classify_upload(head, sizeof(head), FS_ADDR + FS_SIZE + 400, APP_SLOT_SIZE, FS_ADDR));
  // Lower bound: anything at least reaching the fs segment offset.
  TEST_ASSERT_EQUAL(OTA_KIND_MERGED,
    ota_classify_upload(head, sizeof(head), FS_ADDR, APP_SLOT_SIZE, FS_ADDR));
}

void test_classify_esp_image_in_dead_zone_rejected(void) {
  // Too big for the app slot, too short to contain the fs segment:
  // a truncated merged file or a wrong-chip build. Must not flash.
  uint8_t head[16] = {0xE9};
  TEST_ASSERT_EQUAL(OTA_KIND_INVALID,
    ota_classify_upload(head, sizeof(head), APP_SLOT_SIZE + 0x1000, APP_SLOT_SIZE, FS_ADDR));
  TEST_ASSERT_EQUAL(OTA_KIND_INVALID,
    ota_classify_upload(head, sizeof(head), FS_ADDR - 1, APP_SLOT_SIZE, FS_ADDR));
}

void test_classify_garbage_rejected(void) {
  uint8_t head[16] = {0x50, 0x4B, 0x03, 0x04}; // a zip, say
  TEST_ASSERT_EQUAL(OTA_KIND_INVALID,
    ota_classify_upload(head, sizeof(head), 500000, APP_SLOT_SIZE, FS_ADDR));
  TEST_ASSERT_EQUAL(OTA_KIND_INVALID,
    ota_classify_upload(nullptr, 0, 500000, APP_SLOT_SIZE, FS_ADDR));
}

void test_classify_no_fs_partition_never_merged(void) {
  // mergedMin = SIZE_MAX when the fs partition is missing: an oversized ESP
  // image can then never classify as merged.
  uint8_t head[16] = {0xE9};
  TEST_ASSERT_EQUAL(OTA_KIND_INVALID,
    ota_classify_upload(head, sizeof(head), 0x400000, APP_SLOT_SIZE, SIZE_MAX));
}

// ---------------------------------------------------------------------------
// ota_region_overlap - chunk vs flash-region intersection
// ---------------------------------------------------------------------------

static const size_t APP_START = OTA0_ADDR;                 // 0x10000
static const size_t APP_END   = OTA0_ADDR + APP_SLOT_SIZE; // 0x1B0000
static const size_t FS_START  = FS_ADDR;                   // 0x350000
static const size_t FS_END    = FS_ADDR + FS_SIZE;         // 0x400000

void test_overlap_chunk_entirely_before_region(void) {
  size_t s = 99, d = 99;
  // Bootloader/partition-table bytes: chunk [0, 0x1000) vs app region.
  TEST_ASSERT_EQUAL_size_t(0, ota_region_overlap(0, 0x1000, APP_START, APP_END, &s, &d));
}

void test_overlap_chunk_entirely_after_region(void) {
  size_t s = 99, d = 99;
  // fs-segment bytes vs the app region: must not reach esp_ota_write.
  TEST_ASSERT_EQUAL_size_t(0, ota_region_overlap(FS_START, 0x1000, APP_START, APP_END, &s, &d));
}

void test_overlap_chunk_fully_inside_region(void) {
  size_t s = 99, d = 99;
  size_t n = ota_region_overlap(APP_START + 0x5000, 0x1000, APP_START, APP_END, &s, &d);
  TEST_ASSERT_EQUAL_size_t(0x1000, n);
  TEST_ASSERT_EQUAL_size_t(0, s);        // whole chunk is region bytes
  TEST_ASSERT_EQUAL_size_t(0x5000, d);   // lands 0x5000 into the region
}

void test_overlap_chunk_straddles_region_start(void) {
  size_t s = 99, d = 99;
  // Chunk covers the last 0x400 pre-app bytes plus the first 0xC00 app bytes.
  size_t n = ota_region_overlap(APP_START - 0x400, 0x1000, APP_START, APP_END, &s, &d);
  TEST_ASSERT_EQUAL_size_t(0xC00, n);
  TEST_ASSERT_EQUAL_size_t(0x400, s); // skip the pre-region bytes in the chunk
  TEST_ASSERT_EQUAL_size_t(0, d);     // write from the region's byte 0
}

void test_overlap_chunk_straddles_region_end(void) {
  size_t s = 99, d = 99;
  // Chunk crosses from the app slot into the skipped ota_1 region.
  size_t n = ota_region_overlap(APP_END - 0x400, 0x1000, APP_START, APP_END, &s, &d);
  TEST_ASSERT_EQUAL_size_t(0x400, n);
  TEST_ASSERT_EQUAL_size_t(0, s);
  TEST_ASSERT_EQUAL_size_t(APP_SLOT_SIZE - 0x400, d);
}

void test_overlap_chunk_spans_entire_region(void) {
  size_t s = 99, d = 99;
  size_t n = ota_region_overlap(FS_START - 0x100, FS_SIZE + 0x200, FS_START, FS_END, &s, &d);
  TEST_ASSERT_EQUAL_size_t(FS_SIZE, n);
  TEST_ASSERT_EQUAL_size_t(0x100, s);
  TEST_ASSERT_EQUAL_size_t(0, d);
}

void test_overlap_zero_length_chunk(void) {
  size_t s = 99, d = 99;
  TEST_ASSERT_EQUAL_size_t(0, ota_region_overlap(APP_START, 0, APP_START, APP_END, &s, &d));
}

void test_overlap_chunk_touching_region_boundaries(void) {
  size_t s = 99, d = 99;
  // Ends exactly where the region starts: no overlap (half-open intervals).
  TEST_ASSERT_EQUAL_size_t(0, ota_region_overlap(APP_START - 0x100, 0x100, APP_START, APP_END, &s, &d));
  // Starts exactly where the region ends: no overlap.
  TEST_ASSERT_EQUAL_size_t(0, ota_region_overlap(APP_END, 0x100, APP_START, APP_END, &s, &d));
}

void test_overlap_walks_a_merged_upload(void) {
  // Simulate a full 4 MB merged upload in 1460-byte chunks and account for
  // every byte: app-region bytes + fs-region bytes + skipped == file size.
  const size_t FILE_SIZE = FS_END; // merged image ends with the fs segment
  const size_t CHUNK = 1460;
  size_t appBytes = 0, fsBytes = 0;
  size_t expectedNextAppDst = 0, expectedNextFsDst = 0;
  for (size_t off = 0; off < FILE_SIZE; off += CHUNK) {
    size_t len = (FILE_SIZE - off < CHUNK) ? (FILE_SIZE - off) : CHUNK;
    size_t s, d, n;
    n = ota_region_overlap(off, len, APP_START, APP_END, &s, &d);
    if (n) {
      TEST_ASSERT_EQUAL_size_t(expectedNextAppDst, d); // sequential for esp_ota_write
      expectedNextAppDst = d + n;
      appBytes += n;
    }
    n = ota_region_overlap(off, len, FS_START, FS_END, &s, &d);
    if (n) {
      TEST_ASSERT_EQUAL_size_t(expectedNextFsDst, d);
      expectedNextFsDst = d + n;
      fsBytes += n;
    }
  }
  TEST_ASSERT_EQUAL_size_t(APP_SLOT_SIZE, appBytes);
  TEST_ASSERT_EQUAL_size_t(FS_SIZE, fsBytes);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_littlefs_magic_accepted);
  RUN_TEST(test_littlefs_short_buffer_rejected);
  RUN_TEST(test_littlefs_wrong_magic_rejected);
  RUN_TEST(test_firmware_image_is_not_littlefs);
  RUN_TEST(test_classify_littlefs_image);
  RUN_TEST(test_classify_bare_firmware);
  RUN_TEST(test_classify_merged_image);
  RUN_TEST(test_classify_esp_image_in_dead_zone_rejected);
  RUN_TEST(test_classify_garbage_rejected);
  RUN_TEST(test_classify_no_fs_partition_never_merged);
  RUN_TEST(test_overlap_chunk_entirely_before_region);
  RUN_TEST(test_overlap_chunk_entirely_after_region);
  RUN_TEST(test_overlap_chunk_fully_inside_region);
  RUN_TEST(test_overlap_chunk_straddles_region_start);
  RUN_TEST(test_overlap_chunk_straddles_region_end);
  RUN_TEST(test_overlap_chunk_spans_entire_region);
  RUN_TEST(test_overlap_zero_length_chunk);
  RUN_TEST(test_overlap_chunk_touching_region_boundaries);
  RUN_TEST(test_overlap_walks_a_merged_upload);
  return UNITY_END();
}
