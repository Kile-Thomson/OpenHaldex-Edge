#pragma once
// Minimal off-target Arduino shim for host-native characterization tests.
// Provides only what the pure-function core TUs (globals + Calculations)
// transitively need. This is a TEST stub — never linked into firmware.

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

// Arduino fixed-width aliases
typedef uint8_t  byte;
typedef bool     boolean;

// Digital level constants (used in InterruptButton ctor args)
#ifndef HIGH
#define HIGH 1
#endif
#ifndef LOW
#define LOW 0
#endif

// Arduino constrain() — identical clamp semantics to the AVR/ESP core macro.
#ifndef constrain
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#endif

// Minimal millis() so any timing reference resolves on the host.
static inline unsigned long millis() { return 0UL; }
static inline unsigned long micros() { return 0UL; }
static inline void delay(unsigned long) {}

// Minimal String — only present in case a transitively-included header
// references it. Backed by std::string; not a faithful Arduino String.
class String : public std::string {
public:
  String() = default;
  String(const char *s) : std::string(s ? s : "") {}
  String(const std::string &s) : std::string(s) {}
  const char *c_str() const { return std::string::c_str(); }
};

// Minimal Serial stub so DEBUG(...) macros (Serial.printf) resolve if any
// included header expands them in a compiled TU.
struct SerialStub {
  template <typename... A> int printf(const char *, A...) { return 0; }
  void begin(unsigned long) {}
  template <typename T> void print(T) {}
  template <typename T> void println(T) {}
  void println() {}
};
static SerialStub Serial;
