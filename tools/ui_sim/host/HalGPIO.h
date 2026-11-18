#pragma once

// Host stand-in for lib/hal/HalGPIO.h. The simulator feeds keys from a scenario
// file instead of polling real pins, so only the shape GfxRenderer/UI code
// touches is provided here.
#include <cstdint>

class HalGPIO {
 public:
  enum class Button : uint8_t { None = 0, Up, Down, Left, Right, Confirm, Back, Power };

  static HalGPIO& getInstance() {
    static HalGPIO instance;
    return instance;
  }

  void update() {}
  bool isPressed(Button) const { return false; }
  bool wasPressed(Button) const { return false; }
  bool wasReleased(Button) const { return false; }
  void clearPressed() {}
};
