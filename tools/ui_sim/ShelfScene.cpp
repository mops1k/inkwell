#include "ShelfScene.h"

#include <algorithm>
#include <cstdio>

#include "components/UITheme.h"

namespace uisim {

ShelfScene::ShelfScene() {
  books_ = {
      {"Война и мир", "Лев Толстой", {}, 12},
      {"Моби Дик", "Герман Мелвилл", {}, 64},
      {"Отцы и дети", "Иван Тургенев", {}, -1},
      {"Мастер и Маргарита", "Михаил Булгаков", {}, 100},
      {"Преступление и наказание", "Фёдор Достоевский", {}, 37},
      {"Тихий Дон", "Михаил Шолохов", {}, -1},
  };
}

shelfui::GridMetrics ShelfScene::gridMetrics(const GfxRenderer& renderer) const {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  // The theme owns the chrome, so the grid gets whatever the theme leaves free:
  // the content box minus the header. No hardcoded 40/56 pixel bands.
  const Rect content = UITheme::getContentRect(renderer, /*hasBottomHints=*/true, /*hasSideHints=*/true);
  const int top = content.y + metrics.headerHeight + metrics.verticalSpacing;
  return shelfui::gridMetrics(shelfui::GridArea{content.x, top, content.width, content.y + content.height - top});
}

int ShelfScene::pageStart(const shelfui::GridMetrics& metrics) const {
  const int capacity = shelfui::pageCapacity(metrics);
  return (selected_ / capacity) * capacity;
}

void ShelfScene::useCovers(const std::vector<std::string>& covers) {
  for (size_t i = 0; i < books_.size() && i < covers.size(); ++i) books_[i].coverPath = covers[i];
}

void ShelfScene::onKey(const std::string& key) {
  // Up/Down follow the rendered column count instead of a hardcoded constant.
  // The column count comes from the theme grid, which needs a renderer only for
  // the screen size; the harness screen matches the GridArea defaults.
  const int columns = shelfui::gridMetrics(shelfui::GridArea{}).columns;
  const int last = static_cast<int>(books_.size()) - 1;
  if (key == "Left") selected_ = std::clamp(selected_ - 1, 0, last);
  if (key == "Right") selected_ = std::clamp(selected_ + 1, 0, last);
  if (key == "Up") selected_ = std::clamp(selected_ - columns, 0, last);
  if (key == "Down") selected_ = std::clamp(selected_ + columns, 0, last);
}

void ShelfScene::render(GfxRenderer& renderer) const {
  renderer.clearScreen();

  const BaseTheme& theme = UITheme::getInstance().getTheme();
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const Rect content = UITheme::getContentRect(renderer, /*hasBottomHints=*/true, /*hasSideHints=*/true);

  char subtitle[32];
  std::snprintf(subtitle, sizeof(subtitle), "%zu книг(и)", books_.size());
  theme.drawHeader(renderer, Rect{content.x, content.y, content.width, metrics.headerHeight}, "Полка", subtitle);

  const shelfui::GridMetrics grid = gridMetrics(renderer);
  const int capacity = shelfui::pageCapacity(grid);
  const int start = pageStart(grid);
  const int count = std::min(capacity, static_cast<int>(books_.size()) - start);

  shelfui::PageView page;
  page.cells = books_.data() + start;
  page.count = static_cast<size_t>(std::max(0, count));
  page.selected = selected_ - start;
  page.page = start / capacity;
  page.pageCount = std::max(1, (static_cast<int>(books_.size()) + capacity - 1) / capacity);
  page.emptyText = "Книг не найдено";
  shelfui::drawShelfPage(renderer, grid, page);

  theme.drawButtonHints(renderer, "", "Открыть", "Влево", "Вправо");
  theme.drawSideButtonHints(renderer, "Вверх", "Вниз");
}

}  // namespace uisim
