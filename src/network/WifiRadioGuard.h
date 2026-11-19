#pragma once

// Whether this board has a WiFi radio the firmware can actually initialize.
//
// The X3 does. The QEMU board model used for development (tools/qemu) does not:
// it emulates the panel, SD, I2C, GPIO and timers, but there is no WiFi MAC/PHY
// model and the emulated ROM leaves the SYSCON modem clock gates unset. Calling
// esp_wifi_init() there ends in esp_phy_enable()'s assertion
//   assert failed: esp_phy_enable phy_init.c:310
//   (phy_module_has_clock_bits(PHY_INIT_MODEM_CLOCK_REQUIRED_BITS))
// which aborts the chip and reboots it (rst:0xc RTC_SW_CPU_RST). Every network
// screen would therefore reboot the emulator instead of showing anything.
//
// Network features ask this first and report "WiFi is not available" instead of
// touching the radio. On real hardware it is a constant true.
bool wifiRadioAvailable();
