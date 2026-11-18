#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace uisim {

// Minimal, dependency-free 1-bit PNG writer for simulator frames.
//
// The panel buffer is 1bpp MSB-first with 1 = WHITE. PNG grayscale bit depth 1
// uses 1 = WHITE as well, so the frame maps without inversion; the writer just
// rotates the panel frame into reading orientation (the firmware draws in
// Portrait on a landscape-native panel).
class Png {
 public:
  // `frame` is panel-native: widthBytes = panelWidth/8, panelHeight rows.
  // rotated=true turns the 90-degree panel frame into an upright page.
  static bool write(const std::string& path, const std::vector<uint8_t>& frame, int panelWidth, int panelHeight,
                    bool rotated = true);
};

}  // namespace uisim
