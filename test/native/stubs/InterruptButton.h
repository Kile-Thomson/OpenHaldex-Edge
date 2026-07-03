#pragma once
// Off-target stub for the InterruptButton library. The core instantiates two
// button globals (btnMode, btnMode_ext) with a 7-argument constructor.

class InterruptButton {
public:
  InterruptButton(int = 0, int = 0, int = 0, int = 0, int = 0, int = 0, int = 0) {}
  void begin() {}
  void bind(int, void (*)()) {}
  void setButtonState(int) {}
};
