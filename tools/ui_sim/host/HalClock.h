#pragma once

#include <cstddef>
#include <cstdio>

// Host stand-in for lib/hal/HalClock.h. The device clock bridges the missing
// battery-backed RTC (NVS + LP timer + SNTP); none of that exists on the host,
// and the themes only ever ask it to format the current time for a header. The
// time is fixed so consecutive simulator runs produce identical frames.
namespace HalClock {

inline void formatTime(char* buf, size_t bufSize, bool use24h) {
  std::snprintf(buf, bufSize, "%s", use24h ? "10:42" : "10:42am");
}

inline bool isSynced() { return true; }

}  // namespace HalClock
