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
  metrics_ = shelfui::gridMetrics(renderer.getScreenWidth(), renderer.getScreenHeight());
  index_ = {};
  selected_ = 0;

  // A successful load is the common case; anything else (missing, truncated,
  // foreign version) falls through to a rebuild instead of an error screen.
  if (store_.load(index_) && !index_.books.empty()) {
    if (!focusBookPath_.empty()) {
      for (size_t i = 0; i < index_.books.size(); ++i) {
        if (index_.books[i].path == focusBookPath_) {
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
    if (menuOpen_) {
      if (ev.type == ButtonEventManager::PressType::Short) {
        if (ev.button == MappedInputManager::Button::Up) {
          menuIndex_ = std::max(0, menuIndex_ - 1);
          requestUpdate();
        } else if (ev.button == MappedInputManager::Button::Down) {
          menuIndex_ = std::min(static_cast<int>(BookAction::Count) - 1, menuIndex_ + 1);
          requestUpdate();
        } else if (ev.button == MappedInputManager::Button::Confirm) {
          menuOpen_ = false;
          runBookAction(static_cast<BookAction>(menuIndex_));
        }
      }
      if (ev.button == MappedInputManager::Button::Back) {
        menuOpen_ = false;
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
      menuOpen_ = true;
      menuIndex_ = 0;
      requestUpdate();
    }
  }
}

void ShelfActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (mode_ == Mode::Grid) {
    shelfui::PageView page;
    page.cells = cells_.data();
    page.count = cells_.size();
    page.selected = selected_ - (selected_ / shelfui::pageCapacity(metrics_)) * shelfui::pageCapacity(metrics_);
    page.page = selected_ / shelfui::pageCapacity(metrics_);
    page.pageCount = std::max(1, (static_cast<int>(index_.books.size()) + shelfui::pageCapacity(metrics_) - 1) /
                                     shelfui::pageCapacity(metrics_));
    page.title = tr(STR_SHELF);
    page.hint = tr(STR_SHELF_HINT);
    page.emptyText = tr(STR_SHELF_EMPTY);
    shelfui::drawShelfPage(renderer, metrics_, page);
    if (menuOpen_) drawMenu();
  } else {
    const char* text = mode_ == Mode::Scanning ? tr(STR_SHELF_SCANNING) : tr(STR_LOADING);
    const int width = renderer.getTextWidth(UI_12_FONT_ID, text);
    renderer.drawText(UI_12_FONT_ID, (renderer.getScreenWidth() - width) / 2, renderer.getScreenHeight() / 2, text);
  }

  renderer.displayBuffer();
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
  refreshPage();
  requestUpdate();
}

void ShelfActivity::refreshPage() {
  cells_.clear();
  const int capacity = shelfui::pageCapacity(metrics_);
  const int pageIndex = capacity > 0 ? selected_ / capacity : 0;
  const int start = pageIndex * capacity;
  const int end = std::min(static_cast<int>(index_.books.size()), start + capacity);
  for (int i = start; i < end; ++i) {
    const shelf::ShelfBook& book = index_.books[i];
    shelfui::CellView cell;
    cell.title = book.meta.title.empty() ? shelf::fileNameStem(book.path) : book.meta.title;
    cell.author = book.meta.author;
    cell.coverPath = covers_.ensureCover(book.path, shelf::ShelfCoverSize::Cell);
    cell.progressPercent = progressFor(book.path);
    cells_.push_back(std::move(cell));
  }
}

void ShelfActivity::moveSelection(int delta) {
  if (index_.books.empty()) return;
  const int last = static_cast<int>(index_.books.size()) - 1;
  const int next = std::clamp(selected_ + delta, 0, last);
  if (next == selected_) return;
  selected_ = next;
  refreshPage();
  requestUpdate();
}

const shelf::ShelfBook* ShelfActivity::selectedBook() const {
  if (selected_ < 0 || selected_ >= static_cast<int>(index_.books.size())) return nullptr;
  return &index_.books[selected_];
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
  if (book == nullptr) {
    menuOpen_ = false;
    return;
  }
  const std::string path = book->path;
  switch (action) {
    case BookAction::Open:
      openSelected();
      return;
    case BookAction::Info:
      startActivityForResult(std::make_unique<BookInfoActivity>(renderer, mappedInput, path),
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

void ShelfActivity::drawMenu() {
  const int itemCount = static_cast<int>(BookAction::Count);
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID) + 12;
  const int boxWidth = 320;
  const int boxHeight = itemCount * lineHeight + 20;
  const int x = (renderer.getScreenWidth() - boxWidth) / 2;
  const int y = (renderer.getScreenHeight() - boxHeight) / 2;

  renderer.fillRect(x, y, boxWidth, boxHeight, false);
  renderer.drawRect(x, y, boxWidth, boxHeight, 2, true);
  for (int i = 0; i < itemCount; ++i) {
    const int itemY = y + 10 + i * lineHeight;
    const char* label = actionLabel(static_cast<BookAction>(i));
    if (i == menuIndex_) {
      renderer.fillRect(x + 2, itemY - 4, boxWidth - 4, lineHeight, true);
      renderer.drawText(SMALL_FONT_ID, x + 12, itemY, label, false);
    } else {
      renderer.drawText(SMALL_FONT_ID, x + 12, itemY, label, true);
    }
  }
}
