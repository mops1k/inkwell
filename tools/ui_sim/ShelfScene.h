#pragma once

#include <GfxRenderer.h>

#include <string>
#include <vector>

#include "Scene.h"
#include "activities/shelf/ShelfGridRenderer.h"

// Host scene that renders the REAL shelf page renderer with fixture books. It
// exists so the layout can be checked without a device and without an SD card;
// it deliberately uses the same shelfui::drawShelfPage the device screen calls.
// The chrome around the grid (header, button hints, side hints) is drawn by the
// REAL theme, exactly as on the device — the grid only fills the content box the
// theme leaves free.
namespace uisim {

class ShelfScene : public Scene {
 public:
  ShelfScene();

  void onKey(const std::string& key) override;
  void render(GfxRenderer& renderer) const override;

  // Points the cells at real 2-bit BMP cover files (see CoverFixtures). Without
  // this the cells draw the placeholder frame, which is the device's "cover not
  // generated yet" state.
  void useCovers(const std::vector<std::string>& covers);

 private:
  // Grid area derived from the current theme's content rect, and the metrics
  // that follow from it. Both depend on the renderer (screen/orientation) and on
  // the selected theme, so they are computed per draw instead of cached.
  shelfui::GridMetrics gridMetrics(const GfxRenderer& renderer) const;
  int pageStart(const shelfui::GridMetrics& metrics) const;

  std::vector<shelfui::CellView> books_;
  int selected_ = 0;
};

}  // namespace uisim
