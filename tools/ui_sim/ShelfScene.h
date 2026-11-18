#pragma once

#include <GfxRenderer.h>

#include <string>
#include <vector>

#include "activities/shelf/ShelfGridRenderer.h"

// Host scene that renders the REAL shelf page renderer with fixture books. It
// exists so the layout can be checked without a device and without an SD card;
// it deliberately uses the same shelfui::drawShelfPage the device screen calls.
namespace uisim {

class ShelfScene {
 public:
  ShelfScene();

  void onKey(const std::string& key);
  void render(GfxRenderer& renderer) const;

 private:
  int pageStart() const;

  std::vector<shelfui::CellView> books_;
  shelfui::GridMetrics metrics_;
  int selected_ = 0;
};

}  // namespace uisim
