#include "activities/shelf/ShelfActivity.h"

#include <BookFormat.h>
#include <BookMetadata.h>
#include <BookNameMeta.h>
#include <CoverPolicy.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <ShelfIndexer.h>
#include <fontIds.h>

#include <algorithm>

#include "../ActivityManager.h"
#include "../home/BookInfoActivity.h"
#include "CoverTrace.h"
#include "CrossPointSettings.h"
#include "KOReaderDocumentId.h"
#include "ReadingStats.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/cog.h"
#include "components/icons/folder.h"
#include "components/icons/library.h"
#include "components/icons/recent.h"
#include "components/icons/settings.h"
#include "components/icons/transfer.h"

namespace {

// Reading progress as the shelf shows it: the last recorded percentage, and
// 100 once the book was marked finished. -1 means "never opened".
// The percentage is trusted on its own: a session can record progress while its
// elapsed time is still zero (opened, turned pages, closed quickly), and that
// book must still show its bar — the card already showed it, the grid did not.
int progressFor(const std::string& path) {
  const std::string docId = KOReaderDocumentId::calculateFromFilename(path);
  if (docId.empty()) return -1;
  const BookReadingStats* stats = ReadingStatsStore::getInstance().findBook(docId);
  if (stats == nullptr) return -1;
  if (stats->finishedCount > 0) return 100;
  // Exactly the card's own rule: a book counts as "started" once it has reading
  // time or a session, and then its percentage is shown even when it is still 0.
  // Requiring progress > 0 here made the grid disagree with the card.
  if (stats->totalSeconds == 0 && stats->sessions == 0) return -1;
  return stats->progress;
}

// Sort order offered by the filter overlay, in display order. The last entry
// reuses the existing "recent books" wording: a separate i18n key would say the
// same thing twice.
struct SortEntry {
  shelf::SortKey key;
  StrId label;
};
constexpr SortEntry kSortEntries[] = {
    {shelf::SortKey::Title, StrId::STR_SORT_NAME},
    {shelf::SortKey::Author, StrId::STR_AUTHOR},
    {shelf::SortKey::LastOpened, StrId::STR_MENU_RECENT_BOOKS},
};

// Short uppercase badge on the cover, the same cue the Android BookShelf shows.
// The label itself lives next to the index types so the format filter collects
// exactly the strings the chip draws.
const char* formatBadge(shelf::BookFormat format) { return shelf::bookFormatLabel(format); }

const char* filterLabel(shelf::FilterField field) {
  switch (field) {
    case shelf::FilterField::Author:
      return tr(STR_AUTHOR);
    case shelf::FilterField::Series:
      return tr(STR_SERIES);
    case shelf::FilterField::Genre:
      return tr(STR_GENRE);
    case shelf::FilterField::Format:
      return tr(STR_FORMAT);
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
    case ShelfActivity::BookAction::Hide:
      return tr(STR_HIDE_FROM_LIBRARY);
    case ShelfActivity::BookAction::DeleteCache:
    default:
      return tr(STR_DELETE_CACHE);
  }
}

}  // namespace

// The shelf is the home screen, so it owns the quick-sleep snapshot that used to
// belong to HomeActivity: the focused grid cell, restored on wake.
HomeSleepState ShelfActivity::getSleepState() const {
  HomeSleepState state;
  state.selectorIndex = selected_;
  return state;
}

void ShelfActivity::onEnter() {
  Activity::onEnter();
  // The worker has to exist before the first page is built: refreshPage() queues the
  // covers of every cell without artwork, and requests sent before the task starts are
  // dropped — which is exactly why covers only appeared after a manual "refresh cover".
  coverWorker_.setPaused(false);
  (void)coverWorker_.start();
  shelfui::coverTraceReset();
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
      hiddenPaths_ = std::move(state.hiddenPaths);
    }
    applyQuery();
    if (!focusBookPath_.empty()) {
      for (size_t i = 0; i < view_.size(); ++i) {
        if (index_.books[view_[i]].path == focusBookPath_) {
          selected_ = static_cast<int>(i);
          break;
        }
      }
    } else if (focusSelectorIndex_ >= 0 && !view_.empty()) {
      // Quick sleep / "return to home" hint: restore the exact grid cell instead
      // of jumping back to the first book.
      selected_ = std::clamp(focusSelectorIndex_, 0, static_cast<int>(view_.size()) - 1);
    }
    mode_ = Mode::Grid;
    refreshPage();
    // The cached index is the whole first paint: no scan on entry. Rescanning is
    // an explicit action ("Обновить библиотеку") because a full SD walk on every
    // visit made opening the library take seconds and swallowed the first key.
  } else {
    mode_ = Mode::Scanning;
  }

  // Drop keys left over from the screen we replaced: the Back that returned here
  // must not be re-read as "open the menu" on the very first tick.
  buttonEvents.drain();
  // Without this the shelf never repaints itself after replacing the previous
  // activity: the old frame (reader, settings) stayed on the panel until some
  // later key triggered a render, which felt like "Back needs two presses".
  requestUpdate(true);
}

void ShelfActivity::onExit() {
  // The reader now owns the SD card: stop decoding artwork so a thumbnail write
  // never competes with a page load. A decode already running finishes; the queue
  // behind it is dropped because those covers belong to the page we are leaving.
  coverWorker_.setPaused(true);
  coverWorker_.cancelPending();
  Activity::onExit();
}

void ShelfActivity::loop() {
  if (mode_ == Mode::Scanning) {
    rebuildIndex();
    return;
  }
  if (mode_ != Mode::Grid) return;

  // Artwork decoded by the cover task lands here. Taking it costs a queue poll,
  // never a decode: the grid stays responsive while a .fb2.zip is inflated.
  collectCoverResults();

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (overlay_ != Overlay::None) {
      const int count = static_cast<int>(overlayItems().size());
      if (ev.type == ButtonEventManager::PressType::Short) {
        // Both the side buttons (Up/Down) and the front strip move the pop-up
        // selection: with a menu open the lower front buttons have no page to
        // turn, so they act as Up/Down. Up/PageBack and Down/PageForward are
        // aliases of the same physical keys, so only one name per pair is read
        // here — handling both would move the selection twice per press.
        const bool previous =
            ev.button == MappedInputManager::Button::Up || ev.button == MappedInputManager::Button::Left;
        const bool next =
            ev.button == MappedInputManager::Button::Down || ev.button == MappedInputManager::Button::Right;
        if (previous) {
          // Wrap-around: the list is short and the keys are the only way around,
          // so Up on the first item lands on the last one (and Down on the last
          // returns to the first) instead of silently doing nothing.
          menuIndex_ = count > 0 ? (menuIndex_ + count - 1) % count : 0;
          requestUpdate();
        } else if (next) {
          menuIndex_ = count > 0 ? (menuIndex_ + 1) % count : 0;
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
          openBookCard();
          return;
        case MappedInputManager::Button::Back:
          openMainMenu();
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
  // Fast path: the same pop-up is open and the grid frame below it is already on
  // the panel. Moving the focus inside a menu redraws the pop-up in RAM and
  // refreshes only the two rows that changed — the rest of the pop-up keeps its
  // pixels, so the panel update stays tiny.
  if (mode_ == Mode::Grid && overlay_ != Overlay::None && overlayRectValid_ && gridFrameShown_ &&
      overlay_ == overlayRectKind_ && overlayWindowRefreshes_ < kMaxOverlayWindowRefreshes) {
    const int previous = overlaySelectedLast_;
    const shelfui::OverlayGeometry previousGeometry = overlayGeometry_;
    renderer.syncWriteBufferFromDisplayed();
    const shelfui::OverlayGeometry geometry = drawOverlayContents();
    const Rect previousRow = shelfui::overlayRowBounds(previousGeometry, previous);
    const Rect currentRow = shelfui::overlayRowBounds(geometry, menuIndex_);
    const bool sameBox = geometry.box.x == previousGeometry.box.x && geometry.box.y == previousGeometry.box.y &&
                         geometry.box.width == previousGeometry.box.width &&
                         geometry.box.height == previousGeometry.box.height &&
                         geometry.firstRow == previousGeometry.firstRow;
    if (sameBox && previousRow.width > 0 && currentRow.width > 0) {
      const int left = std::min(previousRow.x, currentRow.x);
      const int top = std::min(previousRow.y, currentRow.y);
      const int right = std::max(previousRow.x + previousRow.width, currentRow.x + currentRow.width);
      const int bottom = std::max(previousRow.y + previousRow.height, currentRow.y + currentRow.height);
      ++overlayWindowRefreshes_;
      renderer.displayWindow(left, top, right - left, bottom - top);
      return;
    }
  }
  // Fast path for a focus move inside one page: repaint only the cells that
  // changed. A full displayBuffer costs seconds on this panel, a window update a
  // fraction of that — this is the difference the pop-ups already showed.
  if (mode_ == Mode::Grid && overlay_ == Overlay::None && gridFrameShown_ && pendingCellFrom_ >= 0 &&
      pendingCellTo_ >= 0 && cellWindowRefreshes_ < kMaxCellWindowRefreshes) {
    if (syncPartialFocusRefresh()) return;
  }
  overlayWindowRefreshes_ = 0;
  overlayRectValid_ = false;
  gridFrameShown_ = false;
  pendingCellFrom_ = -1;
  pendingCellTo_ = -1;
  cellWindowRefreshes_ = 0;

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
    page.noCoverText = tr(STR_NO_COVER);
    shelfui::drawShelfPage(renderer, metrics_, page);
    if (overlay_ != Overlay::None) {
      drawOverlayContents();
      overlayRectValid_ = true;
    }
    gridFrameShown_ = true;

    // Same button-hint chrome as the main screen. Only three captions are used,
    // exactly like Home on Lyra: four long labels overflow the 528px row. While
    // an overlay is open Back closes it and Confirm picks, so the hints change
    // with the mode instead of advertising grid navigation that does nothing.
    const auto labels = overlay_ == Overlay::None
                            ? mappedInput.mapLabels(tr(STR_MENU), tr(STR_OPEN), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT))
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
  shelf::ShelfIndexer indexer(&shelf::readBookMetadataThunk, nullptr, &ShelfCoverService::forgetBookThunk, nullptr);
  const shelf::ShelfScanStats stats = indexer.scan(index_, scanOptions());
  LOG_DBG("Shelf", "scan done: files=%zu kept=%zu refreshed=%zu removed=%zu truncated=%d", stats.filesSeen, stats.kept,
          stats.refreshed, stats.removed, stats.truncated ? 1 : 0);
  if (!store_.save(index_)) LOG_ERR("Shelf", "index save failed, shelf keeps the previous copy");
  selected_ = 0;
  mode_ = Mode::Grid;
  applyQuery();
  refreshPage();
  requestUpdate();
}

shelf::ShelfScanOptions ShelfActivity::scanOptions() const {
  shelf::ShelfScanOptions options;
  // OPDS drops downloads into its configured folder, which is usually outside
  // /books — the shelf has to follow that setting or those books stay invisible.
  std::string folder = SETTINGS.opdsDownloadFolder;
  if (!folder.empty()) {
    if (folder.front() != '/') folder.insert(folder.begin(), '/');
    while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
    if (folder != "/") options.downloadsRoot = std::move(folder);
  }
  return options;
}

void ShelfActivity::applyQuery() {
  // Both the hidden list and the "recently opened" epochs read state that can be
  // stale if it is only loaded once at activity construction.
  ReadingStatsStore::ScopedLoad statsLoad;
  query_.hiddenPaths = hiddenPaths_;
  // "Recently opened" needs a timestamp per book; the index itself does not
  // store one (it would have to be rewritten on every read), so the epochs come
  // from the reading history at query time.
  std::vector<uint32_t> epochs;
  if (query_.sort == shelf::SortKey::LastOpened) {
    epochs.reserve(index_.books.size());
    for (const auto& book : index_.books) {
      const std::string docId = KOReaderDocumentId::calculateFromFilename(book.path);
      const BookReadingStats* stats = docId.empty() ? nullptr : ReadingStatsStore::getInstance().findBook(docId);
      epochs.push_back(stats == nullptr ? 0u : static_cast<uint32_t>(stats->lastReadEpoch));
    }
  }
  query_.apply(index_, view_, epochs.empty() ? nullptr : &epochs);
  if (view_.empty()) {
    selected_ = 0;
  } else {
    selected_ = std::clamp(selected_, 0, static_cast<int>(view_.size()) - 1);
  }
}

void ShelfActivity::refreshPage() {
  cells_.clear();
  // Requests for the page being replaced are worthless now; the new cells queue
  // their own artwork below.
  coverWorker_.cancelPending();
  pendingCoverRepaints_.clear();
  updateLayout();
  // Load the stats for the duration of this rebuild: the member ScopedLoad may
  // have failed its load if the activity was created before the card was ready,
  // and progressFor() then silently answered "never opened" forever.
  ReadingStatsStore::ScopedLoad statsLoad;
  const int capacity = shelfui::pageCapacity(metrics_);
  const int pageIndex = capacity > 0 ? selected_ / capacity : 0;
  const int start = pageIndex * capacity;
  const int end = std::min(static_cast<int>(view_.size()), start + capacity);
  for (int i = start; i < end; ++i) {
    const shelf::ShelfBook& book = index_.books[view_[i]];
    shelfui::CellView cell;
    cell.title = book.meta.title.empty() ? shelf::fileNameStem(book.path) : book.meta.title;
    cell.author = book.meta.author;
    cell.format = formatBadge(book.format);
    // First paint reads finished thumbnails only; the rest are queued to the
    // background worker, which decodes them while the grid stays interactive.
    cell.coverPath = covers_.existingCover(book.path, shelf::ShelfCoverSize::Cell);
    // Formats without any picture are never queued: asking the worker for a TXT
    // cover produced a log line and a wake-up per book for nothing.
    if (cell.coverPath.empty() && shelf::formatHasArtwork(book.format)) {
      (void)coverWorker_.request(static_cast<int32_t>(cells_.size()), book.path, shelf::ShelfCoverSize::Cell);
    }
    cell.progressPercent = progressFor(book.path);
    cells_.push_back(std::move(cell));
  }
}

void ShelfActivity::collectCoverResults() {
  coverWorker_.poll([this](const shelfui::CoverWorker::Result& result) {
    const int index = result.cellIndex;
    if (index < 0 || index >= static_cast<int>(cells_.size())) return;
    if (!cells_[index].coverPath.empty()) return;
    cells_[index].coverPath = result.thumbPath;
    pendingCoverRepaints_.push_back(index);
    // The repaint window is pendingCellFrom_..To_; the update path below repaints
    // these cells fully (artwork changed) instead of only moving the ring.
    if (pendingCellFrom_ < 0) pendingCellFrom_ = index;
    pendingCellTo_ = index;
    requestUpdate();
  });
}

void ShelfActivity::moveSelection(int delta) {
  if (view_.empty()) return;
  const int last = static_cast<int>(view_.size()) - 1;
  const int next = std::clamp(selected_ + delta, 0, last);
  if (next == selected_) return;
  // A move inside the current page only changes the highlight: rebuilding the
  // page here reloaded the reading stats from SD and reset the queued covers,
  // which is exactly the delay the grid had and the pop-ups did not.
  const int capacity = shelfui::pageCapacity(metrics_);
  const int pageBefore = capacity > 0 ? selected_ / capacity : 0;
  const int pageLocalBefore = capacity > 0 ? selected_ % capacity : 0;
  selected_ = next;
  const int pageAfter = capacity > 0 ? selected_ / capacity : 0;
  const int pageLocalAfter = capacity > 0 ? selected_ % capacity : 0;
  if (pageAfter != pageBefore) {
    refreshPage();
  } else {
    // Same page: remember both cells so the renderer can repaint just them.
    pendingCellFrom_ = pageLocalBefore;
    pendingCellTo_ = pageLocalAfter;
  }
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
  // Back from the reader lands on this book's card, and the card's Back on the grid.
  hint.target = ReturnTo::BookCard;
  hint.selectName = path;
  activityManager.replaceWithReader(path, std::move(hint));
}

// Confirm on the grid opens the book card, not the reader: the card is where a
// book is inspected and where reading starts. Reading straight from the grid is
// still one long-Confirm away (book menu → Open).
void ShelfActivity::openBookCard() {
  const shelf::ShelfBook* book = selectedBook();
  if (book == nullptr) return;
  BookInfoSeed seed;
  seed.title = book->meta.title.empty() ? shelf::fileNameStem(book->path) : book->meta.title;
  seed.author = book->meta.author;
  seed.series = book->meta.series;
  seed.seriesIndex = book->meta.seriesIndex;
  seed.genre = book->meta.genre;
  // Card-sized artwork only if it is already on the card: the card paints first
  // and generating a cover there would defeat its own instant open.
  seed.coverPath = covers_.existingCover(book->path, shelf::ShelfCoverSize::Card);
  if (seed.coverPath.empty()) {
    // Something is better than a blank frame: the cell thumb is already on the
    // card, so the first paint shows the same artwork scaled down while the
    // card-size artwork is being warmed.
    seed.coverPath = covers_.existingCover(book->path, shelf::ShelfCoverSize::Cell);
  }
  {
    ReadingStatsStore::ScopedLoad statsLoad;
    seed.progressPercent = progressFor(book->path);
  }
  const std::string path = book->path;
  startActivityForResult(std::make_unique<BookInfoActivity>(renderer, mappedInput, path, true, std::move(seed)),
                         [this](const ActivityResult&) { requestUpdate(); });
}

void ShelfActivity::openMainMenu() {
  menuActions_.clear();
  menuLabels_.clear();
  const auto add = [this](MenuAction action, StrId label) {
    menuActions_.push_back(action);
    menuLabels_.emplace_back(I18n::getInstance().get(label));
  };
  // The library is not in this list: it is the screen the menu is drawn over.
  add(MenuAction::Files, StrId::STR_BROWSE_FILES);
  add(MenuAction::Recents, StrId::STR_MENU_RECENT_BOOKS);
  add(MenuAction::Opds, StrId::STR_OPDS_BROWSER);
  add(MenuAction::Transfer, StrId::STR_FILE_TRANSFER);
  add(MenuAction::Settings, StrId::STR_SETTINGS_TITLE);
  add(MenuAction::Hidden, StrId::STR_HIDDEN_BOOKS);
  add(MenuAction::Refresh, StrId::STR_SHELF_REFRESH);
  overlay_ = Overlay::Menu;
  menuIndex_ = 0;
  requestUpdate();
}

void ShelfActivity::runMenuAction(MenuAction action) {
  switch (action) {
    case MenuAction::Files:
      activityManager.goToFileBrowser();
      return;
    case MenuAction::Recents:
      activityManager.goToRecentBooks();
      return;
    case MenuAction::Opds:
      activityManager.goToBrowser();
      return;
    case MenuAction::Transfer:
      activityManager.goToFileTransfer();
      return;
    case MenuAction::Settings:
      activityManager.goToSettings();
      return;
    case MenuAction::Hidden:
      openHiddenBooks();
      return;
    case MenuAction::Refresh:
      // Explicit only, and deliberately a FULL rebuild: "refresh" is what the
      // user presses after adding books or when metadata looks wrong, so every
      // file is re-parsed (series included) instead of trusting cached entries.
      // Opening the library still never scans.
      mode_ = Mode::Scanning;
      requestUpdate(true);
      return;
  }
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
    case BookAction::RefreshCover: {
      // Regenerate only this book's cover artwork; the reader's cached parsing
      // artifacts stay, they are expensive to rebuild. The work is queued to the
      // cover task: for .fb2.zip one archive inflate would otherwise freeze the
      // grid for a second (or minutes in the emulator).
      ShelfCoverService::refreshCovers(path);
      refreshPage();
      const int cell = selected_ % std::max(1, shelfui::pageCapacity(metrics_));
      (void)coverWorker_.request(cell, path, shelf::ShelfCoverSize::Cell);
      (void)coverWorker_.request(cell, path, shelf::ShelfCoverSize::Card);
      requestUpdate();
      return;
    }
    case BookAction::DeleteCache:
      // Drop the whole derived cache directory of the book. The book file and
      // the reader's sidecars are never touched (AGENTS.md: cleanup is never user data).
      ShelfCoverService::forgetBook(path);
      refreshPage();
      requestUpdate();
      return;
    case BookAction::Hide:
      // Hiding is a view decision: the file, its cache and its reading history
      // stay untouched, so unhiding from the menu restores everything at once.
      setHidden(path, true);
      applyQuery();
      saveState();
      refreshPage();
      requestUpdate();
      return;
    default:
      return;
  }
}

void ShelfActivity::drawMenu() { drawOverlayContents(); }

bool ShelfActivity::syncPartialFocusRefresh() {
  const int capacity = shelfui::pageCapacity(metrics_);
  if (capacity <= 0 || !gridFrameShown_) return false;
  const int count = static_cast<int>(cells_.size());
  if (pendingCellFrom_ >= count || pendingCellTo_ >= count) return false;

  renderer.syncWriteBufferFromDisplayed();
  const int selectedLocal = selected_ % capacity;
  int left = INT_MAX;
  int top = INT_MAX;
  int right = 0;
  int bottom = 0;
  // Two different reasons to touch a cell:
  //  * a focus move changes only the title row (normal vs inverted plate) — the
  //    artwork, chip, progress and author line are identical, so the refresh window
  //    is that one row;
  //  * freshly decoded artwork replaces pixels inside the cell, so that cell is
  //    redrawn in full.
  const bool artworkChanged = !pendingCoverRepaints_.empty();
  std::vector<int> indices;
  if (artworkChanged) {
    indices = pendingCoverRepaints_;
    for (const int index : {pendingCellFrom_, pendingCellTo_}) {
      if (index >= 0 && std::find(indices.begin(), indices.end(), index) == indices.end()) indices.push_back(index);
    }
  } else {
    indices = {pendingCellFrom_, pendingCellTo_, lastSelectedLocal_};
  }
  for (const int index : indices) {
    if (index < 0 || index >= count) continue;
    const Rect cellRect = shelfui::cellBounds(metrics_, index);
    if (artworkChanged) {
      renderer.fillRect(cellRect.x, cellRect.y, cellRect.width, cellRect.height, false);
      shelfui::drawShelfCell(renderer, metrics_, cells_[index], index, selectedLocal, tr(STR_NO_COVER));
    } else {
      // Redrawing the row in its normal style *is* the erase: the plate and its text
      // are fully inside this rectangle, so no stale selection can survive.
      const std::string title = shelfui::fitCellTitle(renderer, metrics_, cells_[index].title);
      const std::string author = shelfui::fitCellAuthor(renderer, metrics_, cells_[index].author);
      shelfui::drawCellTitleBand(renderer, metrics_, index, title, author, index == selectedLocal);
    }
    const Rect bounds =
        artworkChanged ? shelfui::cellBounds(metrics_, index) : shelfui::choiceBandBounds(renderer, metrics_, index);
    left = std::min(left, bounds.x);
    top = std::min(top, bounds.y);
    right = std::max(right, bounds.x + bounds.width);
    bottom = std::max(bottom, bounds.y + bounds.height);
  }
  lastSelectedLocal_ = selectedLocal;
  if (right <= left || bottom <= top) return false;

  ++cellWindowRefreshes_;
  renderer.displayWindow(left, top, right - left, bottom - top);
  pendingCellFrom_ = -1;
  pendingCellTo_ = -1;
  pendingCoverRepaints_.clear();
  return true;
}

void ShelfActivity::drawOverlayList(const std::vector<std::string>& items, int selected) const {
  shelfui::drawOverlayList(renderer, items, selected);
}

// One entry point for every pop-up, so the fast window refresh and the full
// render always draw the same widget and agree on its rectangle.
shelfui::OverlayGeometry ShelfActivity::drawOverlayContents() {
  overlayRectKind_ = overlay_;
  shelfui::OverlayGeometry geometry;
  if (overlay_ == Overlay::Menu) {
    static const uint8_t* const kMenuIcons[] = {FolderIcon,   RecentIcon, LibraryIcon, TransferIcon,
                                                SettingsIcon, BookIcon,   CogIcon};
    std::vector<const uint8_t*> icons(kMenuIcons, kMenuIcons + sizeof(kMenuIcons) / sizeof(kMenuIcons[0]));
    icons.resize(menuLabels_.size(), nullptr);
    geometry = shelfui::drawOverlayIconList(renderer, menuLabels_, icons, menuIndex_);
  } else {
    geometry = shelfui::drawOverlayList(renderer, overlayItems(), menuIndex_);
  }
  overlayGeometry_ = geometry;
  overlayRect_ = geometry.box;
  overlaySelectedLast_ = menuIndex_;
  return geometry;
}

const std::vector<std::string>& ShelfActivity::overlayItems() const {
  static std::vector<std::string> items;
  if (overlay_ == Overlay::FilterValues) return filterValues_;
  if (overlay_ == Overlay::Menu) return menuLabels_;
  if (overlay_ == Overlay::HiddenBooks) return hiddenLabels_;

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
  for (const shelf::FilterField field : {shelf::FilterField::Author, shelf::FilterField::Series,
                                         shelf::FilterField::Genre, shelf::FilterField::Format}) {
    std::string label = filterLabel(field);
    if (query_.filter.field == field) {
      label += " *";
    }
    items.push_back(std::move(label));
  }
  for (const SortEntry& entry : kSortEntries) {
    std::string label = std::string(tr(STR_SORT_BY)) + ": " + I18n::getInstance().get(entry.label);
    if (query_.sort == entry.key) label += " *";
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
  query_.collectValues(index_, field, values);
  filterValues_.insert(filterValues_.end(), values.begin(), values.end());
  overlay_ = Overlay::FilterValues;
  menuIndex_ = 0;
  requestUpdate();
}

void ShelfActivity::openHiddenBooks() {
  hiddenItems_.clear();
  hiddenLabels_.clear();
  for (const auto& book : index_.books) {
    if (!query_.isHidden(book)) continue;
    hiddenItems_.push_back(book.path);
    hiddenLabels_.push_back(book.meta.title.empty() ? shelf::fileNameStem(book.path) : book.meta.title);
  }
  if (hiddenLabels_.empty()) {
    // Nothing to restore: keep the pop-up open with a single explanatory row
    // instead of an empty box or a jump to another screen.
    hiddenLabels_.emplace_back(tr(STR_NO_HIDDEN_BOOKS));
    hiddenItems_.clear();
  }
  overlay_ = Overlay::HiddenBooks;
  menuIndex_ = 0;
  requestUpdate();
}

void ShelfActivity::setHidden(const std::string& path, bool hidden) {
  auto it = std::find(hiddenPaths_.begin(), hiddenPaths_.end(), path);
  if (hidden) {
    if (it == hiddenPaths_.end()) hiddenPaths_.push_back(path);
  } else if (it != hiddenPaths_.end()) {
    hiddenPaths_.erase(it);
  }
}

void ShelfActivity::activateOverlayItem(Overlay source, int index) {
  switch (source) {
    case Overlay::Menu:
      if (index >= 0 && index < static_cast<int>(menuActions_.size())) {
        runMenuAction(menuActions_[index]);
      }
      return;
    case Overlay::BookMenu:
      runBookAction(static_cast<BookAction>(index));
      return;
    case Overlay::FilterCategories: {
      // 0 = all books, 1..4 = author/series/genre/format pickers, 5.. = sort.
      if (index == 0) {
        query_.filter.clear();
      } else if (index >= 1 && index <= 4) {
        static constexpr shelf::FilterField kFields[] = {shelf::FilterField::Author, shelf::FilterField::Series,
                                                         shelf::FilterField::Genre, shelf::FilterField::Format};
        openFilterValues(kFields[index - 1]);
        return;
      } else {
        const size_t sortIndex = static_cast<size_t>(index) - 5;
        if (sortIndex >= sizeof(kSortEntries) / sizeof(kSortEntries[0])) break;
        query_.sort = kSortEntries[sortIndex].key;
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
    case Overlay::HiddenBooks: {
      // Confirm on a hidden book brings it back and rebuilds the list, so
      // several books can be restored in one visit.
      if (index >= 0 && index < static_cast<int>(hiddenItems_.size())) {
        setHidden(hiddenItems_[index], false);
        applyQuery();
        saveState();
        refreshPage();
        openHiddenBooks();
      }
      return;
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
  state.hiddenPaths = hiddenPaths_;
  stateStore_.save(state);
}
