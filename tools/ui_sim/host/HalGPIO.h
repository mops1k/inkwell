#pragma once

// Host stand-in for lib/hal/HalGPIO.h. The simulator feeds keys from a scenario
// file instead of polling real pins, so only the shape GfxRenderer/UI code
// touches is provided here. The themes call `gpio.deviceIsX3()` to pick the
// side-hint layout, hence the global reference mirroring the device singleton.
#include <cstdint>

class HalGPIO {
 public:
  enum class Button : uint8_t { None = 0, Up, Down, Left, Right, Confirm, Back, Power };

  static HalGPIO& getInstance() {
    static HalGPIO instance;
    return instance;
  }

  // X3 keeps Up/Down on the physical sides, so BaseTheme/LyraTheme draw the side
  // hints on both edges; on X4 they share the right edge.
  bool deviceIsX3() const { return true; }

  // The battery icon fills solid and shows a bolt while charging; the simulator
  // renders a device on battery.
  bool isUsbConnected() const { return false; }

  void update() {}
  bool isPressed(Button) const { return false; }
  bool wasPressed(Button) const { return false; }
  bool wasReleased(Button) const { return false; }
  void clearPressed() {}
};

inline HalGPIO& gpio = HalGPIO::getInstance();
