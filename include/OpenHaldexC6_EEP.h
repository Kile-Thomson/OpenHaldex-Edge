#pragma once

#include <OpenHaldexC6_defs.h>

void readEEP();
void writeEEP(void *arg);

// ---- On-device map slot library --------------------------------------------
// Saved tunes live in their own NVS namespace ("ohmaps"), opened per-operation
// on a local handle so these never touch the settings `pref` handle that the
// writeEEP task owns. All callers run on the single async-webserver task, so the
// operations are already serialized with respect to each other.

// Fill names[] with each slot's stored name (empty string = free slot).
void mapSlotNames(char names[MAP_SLOT_COUNT][MAP_NAME_MAX]);

// Load slot idx into the caller's arrays + name. Returns false if idx is out of
// range or the slot is empty/corrupt; on false the outputs are left untouched.
bool mapSlotLoad(uint8_t idx,
                 uint16_t outSpeed[speedArrayCount],
                 uint8_t outThrottle[throttleArrayCount],
                 uint8_t outLock[throttleArrayCount][speedArrayCount],
                 char outName[MAP_NAME_MAX]);

// Save the given tune into slot idx under name. Returns false on out-of-range
// idx, empty name, or NVS write failure.
bool mapSlotSave(uint8_t idx, const char *name,
                 const uint16_t inSpeed[speedArrayCount],
                 const uint8_t inThrottle[throttleArrayCount],
                 const uint8_t inLock[throttleArrayCount][speedArrayCount]);

// Clear slot idx. Returns false only on out-of-range idx.
bool mapSlotDelete(uint8_t idx);