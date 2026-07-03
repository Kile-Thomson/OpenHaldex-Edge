#pragma once
// Off-target stub for the ESP32 Preferences (NVS) library. The core declares
// a single `pref` global; EEP read/write lives in an excluded TU.
#include <Arduino.h>

class Preferences {
public:
  bool begin(const char *, bool = false) { return true; }
  void end() {}
  bool clear() { return true; }
  template <typename T> T getT(const char *, T d) { return d; }
  unsigned putUInt(const char *, unsigned) { return 0; }
  unsigned getUInt(const char *, unsigned d = 0) { return d; }
  unsigned putBool(const char *, bool) { return 0; }
  bool getBool(const char *, bool d = false) { return d; }
};
