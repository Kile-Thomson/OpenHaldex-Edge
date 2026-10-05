#pragma once

// Pure decision core for the OTA upload path: what kind of image is being
// uploaded, and where each chunk of a merged flash image belongs. Header-only
// and Arduino-free so the native test suite (test/test_otaroute) pins the exact
// logic the device runs - a routing bug here writes to the wrong flash
// partition, which bricks the web UI or the firmware slot.

#include <stddef.h>
#include <stdint.h>

// First byte of every ESP-IDF app (and bootloader) image.
#define OTA_ESP_IMAGE_MAGIC 0xE9

// A littlefs image carries the ASCII superblock magic "littlefs" at byte
// offset 8 of block 0 (verified against mklittlefs output for this project).
#define OTA_LITTLEFS_MAGIC_OFFSET 8

static inline bool ota_image_is_littlefs(const uint8_t *data, size_t len) {
  static const char magic[8] = {'l', 'i', 't', 't', 'l', 'e', 'f', 's'};
  if (data == nullptr || len < OTA_LITTLEFS_MAGIC_OFFSET + 8) return false;
  for (int i = 0; i < 8; i++) {
    if (data[OTA_LITTLEFS_MAGIC_OFFSET + i] != (uint8_t)magic[i]) return false;
  }
  return true;
}

typedef enum {
  OTA_KIND_INVALID = 0, // unrecognized file - reject before touching flash
  OTA_KIND_APP,         // bare firmware.bin -> dual-slot app OTA
  OTA_KIND_FS,          // bare littlefs.bin -> filesystem partition
  OTA_KIND_MERGED,      // firmware-merged.bin -> split into app + fs regions
} ota_upload_kind_t;

// Classify an upload from its first chunk plus the declared upload size.
// appMax:    size of one app (OTA) partition - a bare firmware.bin can never
//            legitimately exceed it.
// mergedMin: flash offset of the filesystem partition - a merged image
//            contains the fs segment at that absolute offset, so it is always
//            at least this long. Anything with the ESP magic that is too big
//            for the app slot but too short to be a merged image is a
//            truncated/wrong file and gets rejected.
// contentLength is the HTTP Content-Length (file plus a little multipart
// framing), which only ever over-states the file size slightly.
static inline ota_upload_kind_t ota_classify_upload(const uint8_t *data, size_t len,
                                                    size_t contentLength,
                                                    size_t appMax, size_t mergedMin) {
  if (ota_image_is_littlefs(data, len)) return OTA_KIND_FS;
  if (data != nullptr && len > 0 && data[0] == OTA_ESP_IMAGE_MAGIC) {
    if (contentLength <= appMax) return OTA_KIND_APP;
    if (contentLength >= mergedMin) return OTA_KIND_MERGED;
  }
  return OTA_KIND_INVALID;
}

// Intersect an upload chunk [chunkStart, chunkStart+chunkLen) with a flash
// region [regionStart, regionEnd). Returns the number of overlapping bytes;
// when non-zero, *srcOff is the offset into the chunk buffer and *dstOff the
// offset into the region where those bytes belong.
static inline size_t ota_region_overlap(size_t chunkStart, size_t chunkLen,
                                        size_t regionStart, size_t regionEnd,
                                        size_t *srcOff, size_t *dstOff) {
  size_t chunkEnd = chunkStart + chunkLen;
  size_t lo = (chunkStart > regionStart) ? chunkStart : regionStart;
  size_t hi = (chunkEnd < regionEnd) ? chunkEnd : regionEnd;
  if (hi <= lo) return 0;
  *srcOff = lo - chunkStart;
  *dstOff = lo - regionStart;
  return hi - lo;
}
