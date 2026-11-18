#include "ShelfScene.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "components/UITheme.h"

namespace uisim {

namespace {
// Category labels mirror the device menu (ShelfActivity::overlayItems); the
// fixture list is a demo of the interaction, not a second implementation of it.
constexpr const char* kCategories[] = {
    "Все книги", "Автор", "Серия", "Жанр", "Сортировка: по названию", "Сортировка: по автору"};
constexpr int kCategoryCount = static_cast<int>(sizeof(kCategories) / sizeof(kCategories[0]));
constexpr const char* kAllBooks = "Все книги";
}  // namespace

ShelfScene::ShelfScene() {
  books_ = {
      {{"Война и мир", "Лев Толстой", {}, 12}, "Русская классика", "Проза"},
      {{"Моби Дик", "Герман Мелвилл", {}, 64}, "Морские романы", "Приключения"},
      {{"Отцы и дети", "Иван Тургенев", {}, -1}, "Русская классика", "Проза"},
      {{"Мастер и Маргарита", "Михаил Булгаков", {}, 100}, "", "Мистика"},
      {{"Преступление и наказание", "Фёдор Достоевский", {}, 37}, "Русская классика", "Проза"},
      {{"Тихий Дон", "Михаил Шолохов", {}, -1}, "", "Эпопея"},
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
  for (size_t i = 0; i < books_.size() && i < covers.size(); ++i) books_[i].cell.coverPath = covers[i];
}

void ShelfScene::applyDemoFilter() {
  filterField_ = "Автор";
  filterValue_ = "Лев Толстой";
  selected_ = 0;
}

std::vector<size_t> ShelfScene::visible() const {
  std::vector<size_t> out;
  out.reserve(books_.size());
  for (size_t i = 0; i < books_.size(); ++i) {
    if (filterValue_.empty()) {
      out.push_back(i);
      continue;
    }
    const std::string& value = filterField_ == "Серия"   ? books_[i].series
                               : filterField_ == "Жанр"  ? books_[i].genre
                               : filterField_ == "Автор" ? books_[i].cell.author
                                                         : books_[i].cell.title;
    if (value == filterValue_) out.push_back(i);
  }
  return out;
}

std::vector<std::string> ShelfScene::overlayItems() const {
  if (overlay_ == Overlay::Categories) {
    std::vector<std::string> items;
    for (const char* label : kCategories) items.emplace_back(label);
    return items;
  }
  return values_;
}

void ShelfScene::openCategories() {
  overlay_ = Overlay::Categories;
  menuIndex_ = 0;
}

void ShelfScene::openValues() {
  categoryIndex_ = menuIndex_;
  filterField_ = kCategories[categoryIndex_];
  values_.clear();
  values_.emplace_back(kAllBooks);
  if (filterField_ == "Автор") {
    for (const auto& book : books_) {
      if (!book.cell.author.empty() && std::find(values_.begin(), values_.end(), book.cell.author) == values_.end()) {
        values_.push_back(book.cell.author);
      }
    }
    std::sort(values_.begin() + 1, values_.end());
  } else if (filterField_ == "Серия") {
    for (const auto& book : books_) {
      if (!book.series.empty() && std::find(values_.begin(), values_.end(), book.series) == values_.end()) {
        values_.push_back(book.series);
      }
    }
    std::sort(values_.begin() + 1, values_.end());
  } else if (filterField_ == "Жанр") {
    for (const auto& book : books_) {
      if (!book.genre.empty() && std::find(values_.begin(), values_.end(), book.genre) == values_.end()) {
        values_.push_back(book.genre);
      }
    }
    std::sort(values_.begin() + 1, values_.end());
  }
  overlay_ = Overlay::Values;
  menuIndex_ = 0;
}

void ShelfScene::activateItem() {
  if (overlay_ == Overlay::Categories) {
    if (menuIndex_ == 0) {
      filterField_.clear();
      filterValue_.clear();
      overlay_ = Overlay::None;
      selected_ = 0;
      return;
    }
    if (menuIndex_ <= 3) {
      openValues();
      return;
    }
    overlay_ = Overlay::None;  // sort entries are inert in the host scene
    return;
  }
  if (overlay_ == Overlay::Values) {
    if (menuIndex_ == 0) {
      filterValue_.clear();
      filterField_.clear();
    } else {
      filterValue_ = values_[static_cast<size_t>(menuIndex_)];
    }
    overlay_ = Overlay::None;
    selected_ = 0;
  }
}

void ShelfScene::onKey(const std::string& key) {
  const int columns = shelfui::gridMetrics(shelfui::GridArea{}).columns;
  if (key == "FilterMenu") {
    openCategories();
    return;
  }
  if (overlay_ != Overlay::None) {
    const int count = static_cast<int>(overlayItems().size());
    if (key == "Up") menuIndex_ = std::max(0, menuIndex_ - 1);
    if (key == "Down") menuIndex_ = std::min(std::max(0, count - 1), menuIndex_ + 1);
    if (key == "Confirm") activateItem();
    if (key == "Back") overlay_ = overlay_ == Overlay::Values ? Overlay::Categories : Overlay::None;
    return;
  }
  const int last = static_cast<int>(visible().size()) - 1;
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

  // Same subtitle rule as the device screen: the active filter names the slice,
  // otherwise the shelf advertises the long-press entry point.
  const std::vector<size_t> view = visible();
  std::string subtitle;
  if (!filterValue_.empty()) {
    subtitle = filterField_ + ": " + filterValue_;
  } else if (!view.empty()) {
    subtitle = "Удержание «Назад»: фильтры";
  }
  theme.drawHeader(renderer, Rect{content.x, content.y, content.width, metrics.headerHeight}, "Полка",
                   subtitle.empty() ? nullptr : subtitle.c_str());

  const shelfui::GridMetrics grid = gridMetrics(renderer);
  const int capacity = shelfui::pageCapacity(grid);
  const int start = pageStart(grid);
  const int count = std::min(capacity, static_cast<int>(view.size()) - start);

  // drawShelfPage draws contiguous cells, so the filtered slice is copied into
  // a page-local vector — exactly what ShelfActivity::refreshPage does.
  std::vector<shelfui::CellView> cells;
  for (int i = 0; i < std::max(0, count); ++i) cells.push_back(books_[view[static_cast<size_t>(start + i)]].cell);

  shelfui::PageView page;
  page.cells = cells.data();
  page.count = cells.size();
  page.selected = selected_ - start;
  page.page = capacity > 0 ? start / capacity : 0;
  page.pageCount = std::max(1, (static_cast<int>(view.size()) + capacity - 1) / capacity);
  page.emptyText = "Книг не найдено";
  shelfui::drawShelfPage(renderer, grid, page);

  if (overlay_ != Overlay::None) shelfui::drawOverlayList(renderer, overlayItems(), menuIndex_);

  // Same mode-dependent hints as the device: grid navigation while browsing,
  // Back/Confirm while an overlay owns the input.
  if (overlay_ == Overlay::None) {
    theme.drawButtonHints(renderer, "", "Открыть", "Влево", "Вправо");
    theme.drawSideButtonHints(renderer, "Вверх", "Вниз");
  } else {
    theme.drawButtonHints(renderer, "Назад", "Выбрать", "", "");
  }
}

}  // namespace uisim
