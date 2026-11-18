#pragma once

#include <cstdint>

// The device header pulls HalGPIO.h in (it hands the gpio to startDeepSleep);
// the themes rely on that include for `gpio`, so the shim mirrors it.
#include "HalGPIO.h"

// Host stand-in for lib/hal/HalPowerManager.h. The device class drives the
// BQ27220 fuel gauge through BatteryMonitor/SdFat and owns deep sleep; the
// simulator needs none of that, only the charge figure the theme headers print.
// Keeping the shim here also keeps freeink-sdk out of the host build.
class HalPowerManager {
 public:
  void begin() {}
  void pollBatteryPercentage() {}
  void setPowerSaving(bool) {}
  void ensureFullSpeedForRadio() {}
  bool isCharging() const { return false; }
  uint16_t getBatteryPercentage() const { return 87; }
};

inline HalPowerManager powerManager;  // Singleton, as on the device
