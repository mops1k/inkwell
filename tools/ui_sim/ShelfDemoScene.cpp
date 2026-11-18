#include "ShelfDemoScene.h"

#include <CoverPolicy.h>
#include <GfxRenderer.h>
#include <fontIds.h>

#include <algorithm>
#include <cstdio>

namespace uisim {

namespace {
constexpr int kMargin = 16;
constexpr int kHeaderHeight = 40;
constexpr int kCaptionHeight = 22;
constexpr int kGap = 12;
constexpr int kColumns = 2;  // two 180px cells fit the 528px portrait width
}  // namespace

ShelfDemoScene::ShelfDemoScene() : titles_{"Война и мир", "Моби Дик", "Отцы и дети", "Мастер и Маргарита"} {}

void ShelfDemoScene::onKey(const std::string& key) {
  if (titles_.empty()) return;
  if (key == "Left") selected_ = (selected_ + static_cast<int>(titles_.size()) - 1) % static_cast<int>(titles_.size());
  if (key == "Right") selected_ = (selected_ + 1) % static_cast<int>(titles_.size());
}

void ShelfDemoScene::render(GfxRenderer& renderer) const {
  renderer.clearScreen();
  const shelf::CoverBox cell = shelf::coverBox(shelf::ShelfCoverSize::Cell);
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  renderer.drawText(UI_12_FONT_ID, kMargin, kMargin, "Полка — ui_sim (демо)");

  const int rowHeight = cell.height + kCaptionHeight + kGap;
  for (size_t index = 0; index < titles_.size(); ++index) {
    const int column = static_cast<int>(index) % kColumns;
    const int row = static_cast<int>(index) / kColumns;
    const int x = kMargin + column * (cell.width + kGap);
    const int y = kMargin + kHeaderHeight + row * rowHeight;

    // Cover placeholder frame with corner ticks: stands in for the real cover
    // BMP until phase Ф3 wires the cover service into this harness.
    renderer.drawRect(x, y, cell.width, cell.height, true);
    const int tick = 12;
    renderer.drawLine(x, y, x + tick, y, 2, true);
    renderer.drawLine(x, y, x, y + tick, 2, true);
    renderer.drawLine(x + cell.width - tick, y, x + cell.width, y, 2, true);
    renderer.drawLine(x + cell.width, y, x + cell.width, y + tick, 2, true);
    renderer.drawLine(x, y + cell.height - tick, x, y + cell.height, 2, true);
    renderer.drawLine(x, y + cell.height, x + tick, y + cell.height, 2, true);
    renderer.drawLine(x + cell.width - tick, y + cell.height, x + cell.width, y + cell.height, 2, true);
    renderer.drawLine(x + cell.width, y + cell.height - tick, x + cell.width, y + cell.height, 2, true);

    renderer.drawText(SMALL_FONT_ID, x, y + cell.height + 4, titles_[index].c_str());

    if (static_cast<int>(index) == selected_) {
      renderer.drawRect(x - 3, y - 3, cell.width + 6, cell.height + 6, 2, true);
      char badge[8];
      std::snprintf(badge, sizeof(badge), "%zu", index + 1);
      renderer.drawText(SMALL_FONT_ID, x + 6, y + 6, badge);
    }
  }

  const int footerY = screenHeight - 26;
  renderer.drawLine(kMargin, footerY - 6, screenWidth - kMargin, footerY - 6, true);
  renderer.drawText(SMALL_FONT_ID, kMargin, footerY, "Left/Right — выбор, Confirm — открыть");
}

}  // namespace uisim
