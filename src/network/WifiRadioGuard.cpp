#include "WifiRadioGuard.h"

#include <cstdint>

#include <Logging.h>

#ifdef XTEINK_WRAP_ESP_WIFI_INIT
#include <esp_wifi.h>
#endif

namespace {

// ESP32-C3 SYSCON "origin" register (0x60026000 + 0x3F8). QEMU's esp32c3
// machine answers 0x51454D55 ('QEMU') here on purpose -- hw/riscv/esp32c3.c,
// A_SYSCON_ORIGIN_REG -- so an application can tell the emulator apart from
// silicon. Real chips do not return this word, so the check is inert there.
constexpr uintptr_t kSysconOriginRegister = 0x60026000u + 0x3f8u;
constexpr uint32_t kQemuOriginMagic = 0x51454d55u;  // 'QEMU' little endian

}  // namespace

bool wifiRadioAvailable() {
  // Constant for the lifetime of the boot; read once.
  static const bool available =
      *reinterpret_cast<volatile uint32_t*>(kSysconOriginRegister) != kQemuOriginMagic;
  return available;
}

#ifdef XTEINK_WRAP_ESP_WIFI_INIT
// Single gate for every WiFi entry point: WiFi.mode(), WiFi.begin(),
// WiFi.scanNetworks(), WiFi.softAP() and the OTA/sync code all go through
// esp_wifi_init(), which is the call that aborts on a board without a radio.
// Returning an error here lets the Arduino WiFi layer and the activities fail
// the way they already know how (WiFi.mode() returns false, scan reports
// WIFI_SCAN_FAILED, softAP returns false) instead of resetting the chip.
extern "C" esp_err_t __real_esp_wifi_init(const wifi_init_config_t* config);

extern "C" esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t* config) {
  if (!wifiRadioAvailable()) {
    LOG_ERR("WIFI", "No WiFi radio on this board: esp_wifi_init() refused (emulator)");
    return ESP_ERR_NOT_SUPPORTED;
  }
  return __real_esp_wifi_init(config);
}
#endif
