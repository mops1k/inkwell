#include "activities/shelf/ShelfActivity.h"

#include <BookMetadata.h>
#include <BookNameMeta.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <ShelfIndexer.h>
#include <fontIds.h>

#include <algorithm>

#include "../ActivityManager.h"
#include "../home/BookInfoActivity.h"
#include "KOReaderDocumentId.h"
#include "ReadingStats.h"
#include "components/UITheme.h"

namespace {

// Reading progress as the shelf shows it: the last recorded percentage, and
// 100 once the book was marked finished. -1 means "never opened".
int progressFor(const std::string& path) {
  const std::string docId = KOReaderDocumentId::calculateFromFilename(path);
  if (docId.empty()) return -1;
  const BookReadingStats* stats = ReadingStatsStore::getInstance().findBook(docId);
  if (stats == nullptr) return -1;
  if (stats->finishedCount > 0) return 100;
  return stats->totalSeconds == 0 ? -1 : stats->progress;
}

const char* filterLabel(shelf::FilterField field) {
  switch (field) {
    case shelf::FilterField::Author:
      return tr(STR_AUTHOR);
    case shelf::FilterField::Series:
      return tr(STR_SERIES);
    case shelf::FilterField::Genre:
      return tr(STR_GENRE);
    case shelf::FilterField::None:
      break;
  }
  return "";
}

const char* actionLabel(ShelfActivity::BookAction action) {
  switch (action) {
    case ShelfActivity::BookAction::Open:
      return tr(STR_OPEN);
    case ShelfActivity::BookAction::Info:
      return tr(STR_INFO);
    case ShelfActivity::BookAction::RefreshCover:
      return tr(STR_REFRESH_COVER);
    case ShelfActivity::BookAction::DeleteCache:
    default:
      return tr(STR_DELETE_CACHE);
  }
}

}  // namespace

void ShelfActivity::onEnter() {
  Activity::onEnter();
  updateLayout();
  index_ = {};
  selected_ = 0;

  // A successful load is the common case; anything else (missing, truncated,
  // foreign version) falls through to a rebuild instead of an error screen.
  if (store_.load(index_) && !index_.books.empty()) {
    // The saved view (filter + sort) is part of the shelf's identity: restore it
    // before resolving the focus so a return from the reader lands on the same
    // filtered page, not on the unfiltered grid.
    shelf::LibraryState state;
    if (stateStore_.load(state)) {
      query_.filter = state.filter;
      query_.sort = state.sort;
    }
    applyQuery();
    if (!focusBookPath_.empty()) {
      for (size_t i = 0; i < view_.size(); ++i) {
        if (index_.books[view_[i]].path == focusBookPath_) {
          selected_ = static_cast<int>(i);
          break;
        }
      }
    }
    mode_ = Mode::Grid;
    refreshPage();
  } else {
    mode_ = Mode::Scanning;
  }
}

void ShelfActivity::loop() {
  if (mode_ == Mode::Scanning) {
    rebuildIndex();
    return;
  }
  if (mode_ != Mode::Grid) return;

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (overlay_ != Overlay::None) {
      const int count = static_cast<int>(overlayItems().size());
      if (ev.type == ButtonEventManager::PressType::Short) {
        if (ev.button == MappedInputManager::Button::Up) {
          menuIndex_ = std::max(0, menuIndex_ - 1);
          requestUpdate();
        } else if (ev.button == MappedInputManager::Button::Down) {
          menuIndex_ = std::clamp(menuIndex_ + 1, 0, std::max(0, count - 1));
          requestUpdate();
        } else if (ev.button == MappedInputManager::Button::Confirm) {
          // The overlay is closed before dispatch: a book action or a filter
          // choice may redraw the grid underneath it.
          const Overlay previous = overlay_;
          overlay_ = Overlay::None;
          activateOverlayItem(previous, menuIndex_);
          if (previous == Overlay::FilterCategories) overlay_ = Overlay::FilterValues;
          requestUpdate();
        }
      }
      if (ev.button == MappedInputManager::Button::Back) {
        overlay_ = overlay_ == Overlay::FilterValues ? Overlay::FilterCategories : Overlay::None;
        menuIndex_ = 0;
        requestUpdate();
      }
      continue;
    }

    if (ev.type == ButtonEventManager::PressType::Short) {
      switch (ev.button) {
        case MappedInputManager::Button::Confirm:
          openSelected();
          return;
        case MappedInputManager::Button::Back:
          activityManager.goHome();
          return;
        case MappedInputManager::Button::Left:
          moveSelection(-1);
          break;
        case MappedInputManager::Button::Right:
          moveSelection(1);
          break;
        case MappedInputManager::Button::Up:
          moveSelection(-metrics_.columns);
          break;
        case MappedInputManager::Button::Down:
          moveSelection(metrics_.columns);
          break;
        default:
          break;
      }
    } else if (ev.type == ButtonEventManager::PressType::Long && ev.button == MappedInputManager::Button::Confirm) {
      overlay_ = Overlay::BookMenu;
      menuIndex_ = 0;
      requestUpdate();
    } else if (ev.type == ButtonEventManager::PressType::Long && ev.button == MappedInputManager::Button::Back) {
      // Long Back is the library's "filter and sort" entry: short Back still
      // leaves the screen, so nothing is remapped for the reader.
      openFilters();
    }
  }
}

void ShelfActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (mode_ == Mode::Grid) {
    updateLayout();
    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect contentRect = UITheme::getContentRect(renderer, true, true);
    // The subtitle is the active filter: it must be readable at a glance which
    // slice of the library the grid is showing.
    std::string subtitle;
    if (query_.filter.active()) {
      subtitle = filterLabel(query_.filter.field);
      subtitle += ": ";
      subtitle += query_.filter.value;
    } else if (!view_.empty()) {
      // The filter entry is a long press, which no hint bar shows: without this
      // line nothing on the screen would tell the user the filters exist.
      subtitle = tr(STR_SHELF_FILTER_HINT);
    }
    GUI.drawHeader(renderer,
                   Rect{contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight},
                   tr(STR_SHELF), subtitle.empty() ? nullptr : subtitle.c_str());

    shelfui::PageView page;
    const int capacity = shelfui::pageCapacity(metrics_);
    page.cells = cells_.data();
    page.count = cells_.size();
    page.selected = capacity > 0 ? selected_ % capacity : 0;
    page.page = capacity > 0 ? selected_ / capacity : 0;
    page.pageCount = std::max(1, (static_cast<int>(view_.size()) + capacity - 1) / capacity);
    page.emptyText = view_.empty() && !index_.books.empty() ? tr(STR_NO_FILES_FOUND) : tr(STR_SHELF_EMPTY);
    shelfui::drawShelfPage(renderer, metrics_, page);
    if (overlay_ == Overlay::BookMenu) {
      drawMenu();
    } else if (overlay_ != Overlay::None) {
      drawOverlayList(overlayItems(), menuIndex_);
    }

    // Same button-hint chrome as the main screen. Only three captions are used,
    // exactly like Home on Lyra: four long labels overflow the 528px row. While
    // an overlay is open Back closes it and Confirm picks, so the hints change
    // with the mode instead of advertising grid navigation that does nothing.
    const auto labels = overlay_ == Overlay::None
                            ? mappedInput.mapLabels("", tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT))
                            : mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    GUI.drawSideButtonHints(renderer, overlay_ == Overlay::None ? tr(STR_DIR_UP) : "",
                            overlay_ == Overlay::None ? tr(STR_DIR_DOWN) : "");
  } else {
    const char* text = mode_ == Mode::Scanning ? tr(STR_SHELF_SCANNING) : tr(STR_LOADING);
    const int width = renderer.getTextWidth(UI_12_FONT_ID, text);
    renderer.drawText(UI_12_FONT_ID, (renderer.getScreenWidth() - width) / 2, renderer.getScreenHeight() / 2, text);
  }

  renderer.displayBuffer();
}

void ShelfActivity::updateLayout() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);
  shelfui::GridArea area;
  area.x = contentRect.x;
  area.y = contentRect.y + metrics.topPadding + metrics.headerHeight + 4;
  area.width = contentRect.width;
  area.height = contentRect.height - metrics.topPadding - metrics.headerHeight - 4;
  metrics_ = shelfui::gridMetrics(area);
}

void ShelfActivity::rebuildIndex() {
  LOG_DBG("Shelf", "rebuilding library index");
  index_ = {};
  shelf::ShelfScanOptions options;
  shelf::ShelfIndexer indexer(&shelf::readBookMetadataThunk, nullptr, &ShelfCoverService::forgetBookThunk, nullptr);
  const shelf::ShelfScanStats stats = indexer.scan(index_, options);
  LOG_DBG("Shelf", "scan done: files=%zu kept=%zu refreshed=%zu removed=%zu truncated=%d", stats.filesSeen, stats.kept,
          stats.refreshed, stats.removed, stats.truncated ? 1 : 0);
  if (!store_.save(index_)) LOG_ERR("Shelf", "index save failed, shelf keeps the previous copy");
  selected_ = 0;
  mode_ = Mode::Grid;
  applyQuery();
  refreshPage();
  requestUpdate();
}

void ShelfActivity::applyQuery() {
  query_.apply(index_, view_);
  if (view_.empty()) {
    selected_ = 0;
  } else {
    selected_ = std::clamp(selected_, 0, static_cast<int>(view_.size()) - 1);
  }
}

void ShelfActivity::refreshPage() {
  cells_.clear();
  updateLayout();
  const int capacity = shelfui::pageCapacity(metrics_);
  const int pageIndex = capacity > 0 ? selected_ / capacity : 0;
  const int start = pageIndex * capacity;
  const int end = std::min(static_cast<int>(view_.size()), start + capacity);
  for (int i = start; i < end; ++i) {
    const shelf::ShelfBook& book = index_.books[view_[i]];
    shelfui::CellView cell;
    cell.title = book.meta.title.empty() ? shelf::fileNameStem(book.path) : book.meta.title;
    cell.author = book.meta.author;
    cell.coverPath = covers_.ensureCover(book.path, shelf::ShelfCoverSize::Cell);
    cell.progressPercent = progressFor(book.path);
    cells_.push_back(std::move(cell));
  }
}

void ShelfActivity::moveSelection(int delta) {
  if (view_.empty()) return;
  const int last = static_cast<int>(view_.size()) - 1;
  const int next = std::clamp(selected_ + delta, 0, last);
  if (next == selected_) return;
  selected_ = next;
  refreshPage();
  requestUpdate();
}

const shelf::ShelfBook* ShelfActivity::selectedBook() const {
  if (selected_ < 0 || selected_ >= static_cast<int>(view_.size())) return nullptr;
  return &index_.books[view_[selected_]];
}

void ShelfActivity::openSelected() {
  const shelf::ShelfBook* book = selectedBook();
  if (book == nullptr) return;
  const std::string path = book->path;
  ReturnHint hint;
  hint.target = ReturnTo::Shelf;
  hint.selectName = path;
  activityManager.replaceWithReader(path, std::move(hint));
}

void ShelfActivity::runBookAction(BookAction action) {
  const shelf::ShelfBook* book = selectedBook();
  if (book == nullptr) return;
  const std::string path = book->path;
  switch (action) {
    case BookAction::Open:
      openSelected();
      return;
    case BookAction::Info:
      startActivityForResult(std::make_unique<BookInfoActivity>(renderer, mappedInput, path, true),
                             [this](const ActivityResult&) { requestUpdate(); });
      return;
    case BookAction::RefreshCover:
      // Regenerate only this book's cover artwork; the reader's cached parsing
      // artifacts stay, they are expensive to rebuild.
      ShelfCoverService::refreshCovers(path);
      refreshPage();
      requestUpdate();
      return;
    case BookAction::DeleteCache:
      // Drop the whole derived cache directory of the book. The book file and
      // the reader's sidecars are never touched (AGENTS.md: cleanup is never user data).
      ShelfCoverService::forgetBook(path);
      refreshPage();
      requestUpdate();
      return;
    default:
      return;
  }
}

void ShelfActivity::drawMenu() { drawOverlayList(overlayItems(), menuIndex_); }

void ShelfActivity::drawOverlayList(const std::vector<std::string>& items, int selected) const {
  shelfui::drawOverlayList(renderer, items, selected);
}

const std::vector<std::string>& ShelfActivity::overlayItems() const {
  static std::vector<std::string> items;
  if (overlay_ == Overlay::FilterValues) return filterValues_;

  items.clear();
  if (overlay_ == Overlay::BookMenu) {
    for (int i = 0; i < static_cast<int>(BookAction::Count); ++i) {
      items.emplace_back(actionLabel(static_cast<BookAction>(i)));
    }
    return items;
  }
  if (overlay_ != Overlay::FilterCategories) return items;

  items.clear();
  items.emplace_back(tr(STR_ALL_BOOKS));
  for (const shelf::FilterField field :
       {shelf::FilterField::Author, shelf::FilterField::Series, shelf::FilterField::Genre}) {
    std::string label = filterLabel(field);
    if (query_.filter.field == field) {
      label += " *";
    }
    items.push_back(std::move(label));
  }
  for (const shelf::SortKey key : {shelf::SortKey::Title, shelf::SortKey::Author}) {
    const char* sortName = key == shelf::SortKey::Title ? tr(STR_SORT_NAME) : tr(STR_AUTHOR);
    std::string label = std::string(tr(STR_SORT_BY)) + ": " + sortName;
    if (query_.sort == key) label += " *";
    items.push_back(std::move(label));
  }
  return items;
}

void ShelfActivity::openFilters() {
  overlay_ = Overlay::FilterCategories;
  menuIndex_ = 0;
  requestUpdate();
}

void ShelfActivity::openFilterValues(shelf::FilterField field) {
  pendingField_ = field;
  filterValues_.clear();
  // "All books" is always the first value: the only way back to the whole
  // library without remembering which value was active before.
  filterValues_.emplace_back(tr(STR_ALL_BOOKS));
  std::vector<std::string> values;
  shelf::collectValues(index_, field, values);
  filterValues_.insert(filterValues_.end(), values.begin(), values.end());
  overlay_ = Overlay::FilterValues;
  menuIndex_ = 0;
  requestUpdate();
}

void ShelfActivity::activateOverlayItem(Overlay source, int index) {
  switch (source) {
    case Overlay::BookMenu:
      runBookAction(static_cast<BookAction>(index));
      return;
    case Overlay::FilterCategories: {
      // 0 = all books, 1..3 = author/series/genre pickers, 4..5 = sort order.
      if (index == 0) {
        query_.filter.clear();
      } else if (index >= 1 && index <= 3) {
        static constexpr shelf::FilterField kFields[] = {shelf::FilterField::Author, shelf::FilterField::Series,
                                                         shelf::FilterField::Genre};
        openFilterValues(kFields[index - 1]);
        return;
      } else {
        query_.sort = index == 4 ? shelf::SortKey::Title : shelf::SortKey::Author;
      }
      break;
    }
    case Overlay::FilterValues: {
      if (index == 0) {
        query_.filter.clear();
      } else if (static_cast<size_t>(index) < filterValues_.size()) {
        query_.filter = {pendingField_, filterValues_[index]};
      }
      break;
    }
    default:
      return;
  }
  applyQuery();
  saveState();
  refreshPage();
}

void ShelfActivity::saveState() {
  shelf::LibraryState state;
  state.filter = query_.filter;
  state.sort = query_.sort;
  stateStore_.save(state);
}
