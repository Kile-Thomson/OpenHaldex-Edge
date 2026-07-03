#pragma once
// Off-target stub for the Freenove WS2812 RGB LED library. The core
// instantiates a single `strip` global; no methods are exercised natively.

enum LED_TYPE { TYPE_RGB, TYPE_GRB, TYPE_BRG, TYPE_RBG, TYPE_GBR, TYPE_BGR };

class Freenove_ESP32_WS2812 {
public:
  Freenove_ESP32_WS2812(int = 0, int = 0, int = 0, int = TYPE_RGB) {}
  bool begin() { return true; }
  void setLedCount(int) {}
  void setBrightness(int) {}
  void setLedColorData(int, int, int, int) {}
  void setLedColor(int, int, int, int) {}
  void show() {}
};
