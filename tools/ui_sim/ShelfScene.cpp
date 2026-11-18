#include "ShelfScene.h"

#include <algorithm>
#include <cstdio>

namespace uisim {
namespace {
constexpr int kScreenWidth = 528;
constexpr int kScreenHeight = 792;
}  // namespace

ShelfScene::ShelfScene() : metrics_(shelfui::gridMetrics(kScreenWidth, kScreenHeight)) {
  books_ = {
      {"Война и мир", "Лев Толстой", {}, 12},
      {"Моби Дик", "Герман Мелвилл", {}, 64},
      {"Отцы и дети", "Иван Тургенев", {}, -1},
      {"Мастер и Маргарита", "Михаил Булгаков", {}, 100},
      {"Преступление и наказание", "Фёдор Достоевский", {}, 37},
      {"Тихий Дон", "Михаил Шолохов", {}, -1},
  };
}

int ShelfScene::pageStart() const {
  const int capacity = shelfui::pageCapacity(metrics_);
  return (selected_ / capacity) * capacity;
}

void ShelfScene::onKey(const std::string& key) {
  const int last = static_cast<int>(books_.size()) - 1;
  if (key == "Left") selected_ = std::clamp(selected_ - 1, 0, last);
  if (key == "Right") selected_ = std::clamp(selected_ + 1, 0, last);
  if (key == "Up") selected_ = std::clamp(selected_ - metrics_.columns, 0, last);
  if (key == "Down") selected_ = std::clamp(selected_ + metrics_.columns, 0, last);
}

void ShelfScene::render(GfxRenderer& renderer) const {
  renderer.clearScreen();
  const int capacity = shelfui::pageCapacity(metrics_);
  const int start = pageStart();
  const int count = std::min(capacity, static_cast<int>(books_.size()) - start);

  shelfui::PageView page;
  page.cells = books_.data() + start;
  page.count = static_cast<size_t>(std::max(0, count));
  page.selected = selected_ - start;
  page.page = start / capacity;
  page.pageCount = std::max(1, (static_cast<int>(books_.size()) + capacity - 1) / capacity);
  page.title = "Полка — ui_sim";
  page.hint = "Стрелки — выбор, OK — открыть, удержание OK — меню";
  page.emptyText = "Книг не найдено";
  shelfui::drawShelfPage(renderer, metrics_, page);
}

}  // namespace uisim
