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
// Sort order offered by the picker, in display order. Titles reuse the strings
// that already exist ("Размер", "Дата", "Недавние книги"), so only the progress
// key needed a new word. The list mirrors shelf::sortKeys() order — the picker
// index is the only thing the overlay has to translate.
constexpr SortEntry kSortEntries[] = {
    {shelf::SortKey::Title, StrId::STR_SORT_KEY_NAME},        {shelf::SortKey::Author, StrId::STR_SORT_KEY_AUTHOR},
    {shelf::SortKey::Series, StrId::STR_SORT_KEY_SERIES},     {shelf::SortKey::LastOpened, StrId::STR_SORT_KEY_RECENT},
    {shelf::SortKey::Progress, StrId::STR_SORT_KEY_PROGRESS}, {shelf::SortKey::Size, StrId::STR_SORT_KEY_SIZE},
    {shelf::SortKey::Added, StrId::STR_SORT_KEY_ADDED},       {shelf::SortKey::Year, StrId::STR_SORT_KEY_YEAR},
};
static_assert(sizeof(kSortEntries) / sizeof(kSortEntries[0]) == shelf::kSortKeyCount,
              "sort picker must list every key");

// Display text of one sort key, for the picker row and the shelf footer. Single
// entry point: a key added to the enum but not here fails the static_assert above.
const char* sortEntryLabel(shelf::SortKey key) {
  for (const SortEntry& entry : kSortEntries) {
    if (entry.key == key) return I18n::getInstance().get(entry.label);
  }
  return I18n::getInstance().get(StrId::STR_SORT_NAME);
}

// "Сортировка: Автор (по возр.)" — one row instead of the three the old picker
// spent, and it carries the direction so the second step is never a guess.
std::string sortRowLabel(shelf::SortKey key, bool descending) {
  std::string label = std::string(tr(STR_SORT_BY)) + ": " + sortEntryLabel(key);
  label += " (";
  label += descending ? tr(STR_SORT_DESC_SHORT) : tr(STR_SORT_ASC_SHORT);
  label += ")";
  return label;
}

// Short uppercase badge on the cover, the same cue the Android BookShelf shows.
// The label itself lives next to the index types so the format filter collects
// exactly the strings the chip draws.
const char* formatBadge(shelf::BookFormat format) { return shelf::bookFormatLabel(format); }

// Dimensions the browse overlay offers, in display order. One table: the row
// order, the index arithmetic in activateOverlayItem and the counts all read it,
// so adding a dimension cannot shift the others by accident.
constexpr shelf::FilterField kBrowseFields[] = {shelf::FilterField::Author, shelf::FilterField::Series,
                                                shelf::FilterField::Genre, shelf::FilterField::Format,
                                                shelf::FilterField::ReadingStatus};
constexpr int kBrowseFieldCount = static_cast<int>(sizeof(kBrowseFields) / sizeof(kBrowseFields[0]));

// The localized name of a reading-status value; every other field's value is
// already display text (an author name, a format label).
std::string filterValueLabel(shelf::FilterField field, const std::string& value) {
  if (field != shelf::FilterField::ReadingStatus) return value;
  switch (shelf::readingStatusFromName(value)) {
    case shelf::ReadingStatus::Unread:
      return tr(STR_STATUS_UNREAD);
    case shelf::ReadingStatus::Reading:
      return tr(STR_STATUS_READING);
    case shelf::ReadingStatus::Finished:
      return tr(STR_STATUS_FINISHED);
  }
  return value;
}

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
    case shelf::FilterField::ReadingStatus:
      return tr(STR_READING_STATUS);
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

// "Автор (5)": the count in parentheses, not "·" — the UI font is a subset and a
// middle dot is not guaranteed to be in it, while parentheses always are.
std::string withCount(const std::string& label, int count) { return label + " (" + std::to_string(count) + ")"; }

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
      query_.descending = state.descending;
      shelfView_ = shelfui::shelfViewFromName(state.view);
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
          // choice may redraw the grid underneath it. Whatever the action opens
          // next (a value list, the sort fields) sets the overlay itself, so
          // nothing may be forced back here — doing that used to swallow the
          // sort picker the moment the sort row was chosen.
          const Overlay previous = overlay_;
          overlay_ = Overlay::None;
          activateOverlayItem(previous, menuIndex_);
          requestUpdate();
        }
      }
      if (ev.button == MappedInputManager::Button::Back) {
        // Back steps out of the pop-up one level at a time: values → categories,
        // direction → fields → categories. Anything else closes the pop-up.
        if (overlay_ == Overlay::FilterValues || overlay_ == Overlay::SortFields) {
          overlay_ = Overlay::FilterCategories;
          menuIndex_ = 0;
        } else if (overlay_ == Overlay::SortDirection) {
          // Stepping back to the fields keeps the staged field under the cursor,
          // and nothing has been applied yet.
          overlay_ = Overlay::SortFields;
          menuIndex_ = 0;
          for (size_t i = 0; i < shelf::kSortKeyCount; ++i) {
            if (kSortEntries[i].key == pendingSort_) menuIndex_ = static_cast<int>(i);
          }
        } else {
          overlay_ = Overlay::None;
          menuIndex_ = 0;
        }
        requestUpdate();
      }
      continue;
    }

    // The browse row owns the keys while it is focused: Left/Right (and Up) move
    // inside it, Confirm applies the chip, Down or Back return to the grid. The
    // row is not an overlay — the grid stays visible and its selection stays put.
    if (chipsFocused_) {
      if (ev.type == ButtonEventManager::PressType::Short) {
        const int chipCount = static_cast<int>(Chip::Count);
        if (ev.button == MappedInputManager::Button::Confirm) {
          applyChip(static_cast<Chip>(chipIndex_));
          return;
        }
        if (ev.button == MappedInputManager::Button::Left || ev.button == MappedInputManager::Button::Up) {
          chipIndex_ = (chipIndex_ + chipCount - 1) % chipCount;
          chipsMoved_ = true;
          requestUpdate();
          continue;
        }
        if (ev.button == MappedInputManager::Button::Right) {
          chipIndex_ = (chipIndex_ + 1) % chipCount;
          chipsMoved_ = true;
          requestUpdate();
          continue;
        }
        if (ev.button == MappedInputManager::Button::Down) {
          if (focusChips(false)) chipsMoved_ = false;
          continue;
        }
      }
      // Back leaves the row first; the main menu opens on the next press. A single
      // Back must never mean "menu" while the cursor is off the grid.
      if (ev.button == MappedInputManager::Button::Back) {
        if (focusChips(false)) chipsMoved_ = false;
        continue;
      }
      continue;
    }

    if (ev.type == ButtonEventManager::PressType::Short) {
      switch (ev.button) {
        case MappedInputManager::Button::Confirm:
          if (emptyAction() != EmptyAction::None) {
            runEmptyAction();
            return;
          }
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
        case MappedInputManager::Button::Up: {
          // Up from the first row steps into the browse row instead of bouncing
          // off the top of the page — that is how the row is reachable with
          // buttons. In list mode the first row is a single book, not a column.
          const int step = shelfView_ == shelfui::ShelfView::List ? 1 : metrics_.columns;
          if (!view_.empty() && step > 0 && selected_ < step && focusChips(true)) return;
          moveSelection(-step);
          break;
        }
        case MappedInputManager::Button::Down:
          moveSelection(shelfView_ == shelfui::ShelfView::List ? 1 : metrics_.columns);
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
  // Fast path: the focus moved inside the browse row. Repaint only that strip —
  // a full page render for one chip would cost the whole panel refresh.
  if (mode_ == Mode::Grid && overlay_ == Overlay::None && gridFrameShown_ && chipsFocused_ && chipsMoved_ &&
      chipsWindowRefreshes_ < kMaxChipsWindowRefreshes) {
    if (syncPartialChipsRefresh()) return;
  }

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
  chipsWindowRefreshes_ = 0;
  chipsMoved_ = false;
  pendingCellFrom_ = -1;
  pendingCellTo_ = -1;
  cellWindowRefreshes_ = 0;

  renderer.clearScreen();

  if (mode_ == Mode::Grid) {
    updateLayout();
    buildChips();
    const auto& metrics = UITheme::getInstance().getMetrics();
    const Rect contentRect = UITheme::getContentRect(renderer, true, true);
    // The subtitle is the active filter: it must be readable at a glance which
    // slice of the library the grid is showing.
    std::string subtitle;
    if (query_.filter.active()) {
      subtitle = filterLabel(query_.filter.field);
      subtitle += ": ";
      subtitle += filterValueLabel(query_.filter.field, query_.filter.value);
    } else if (!view_.empty()) {
      // The filter entry is a long press, which no hint bar shows: without this
      // line nothing on the screen would tell the user the filters exist.
      subtitle = tr(STR_SHELF_FILTER_HINT);
    }
    GUI.drawHeader(renderer,
                   Rect{contentRect.x, contentRect.y + metrics.topPadding, contentRect.width, metrics.headerHeight},
                   tr(STR_SHELF), subtitle.empty() ? nullptr : subtitle.c_str());

    shelfui::PageView page;
    page.pageArea = pageArea_;
    page.chips.chips = chips_.data();
    page.chips.count = chips_.size();
    page.chips.active = chipIndex_;
    if (chipsFocused_) page.chips.focused = chipIndex_;
    const int capacity = pageCapacity();
    page.cells = cells_.data();
    page.count = cells_.size();
    page.selected = capacity > 0 ? selected_ % capacity : 0;
    page.page = capacity > 0 ? selected_ / capacity : 0;
    page.pageCount = std::max(1, (static_cast<int>(view_.size()) + capacity - 1) / capacity);
    page.emptyText = view_.empty() && !index_.books.empty() ? tr(STR_NO_FILES_FOUND) : tr(STR_SHELF_EMPTY);
    page.emptyActionText = emptyActionLabel(emptyAction());
    page.noCoverText = tr(STR_NO_COVER);
    // "Книг: 12, Сортировать по: Серии (по убыв.)" — the footer answers «сколько
    // книг в этой выборке» and «в каком порядке», which the page counter alone
    // cannot. Built here because the renderer links no translations.
    footerText_ = std::string(tr(STR_BOOKS_COUNT)) + ": " + std::to_string(view_.size()) + ", " +
                  sortRowLabel(query_.sort, query_.descending);
    page.footerText = footerText_.c_str();
    if (shelfView_ == shelfui::ShelfView::List) {
      shelfui::drawShelfListPage(renderer, rowMetrics_, page);
    } else {
      shelfui::drawShelfPage(renderer, metrics_, page);
    }
    if (overlay_ != Overlay::None) {
      drawOverlayContents();
      overlayRectValid_ = true;
    }
    gridFrameShown_ = true;

    // Same button-hint chrome as the main screen. Only three captions are used,
    // exactly like Home on Lyra: four long labels overflow the 528px row. While
    // an overlay is open Back closes it and Confirm picks, so the hints change
    // with the mode instead of advertising grid navigation that does nothing.
    // All four front captions are always shown: inside an overlay Left/Right move the
    // highlight exactly like Up/Down, so blanking their labels hid working buttons.
    const auto labels =
        overlay_ == Overlay::None
            ? mappedInput.mapLabels(tr(STR_MENU),
                                    emptyAction() != EmptyAction::None ? emptyActionLabel(emptyAction()) : tr(STR_OPEN),
                                    tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT))
            : mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    // Up/Down keep working inside every overlay (they move the highlighted row), so the
    // side captions stay on screen instead of vanishing: hiding them made the popover
    // look as if the side buttons did nothing.
    GUI.drawSideButtonHints(renderer, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
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
  pageArea_ = area;
  // Cells and rows are laid out in the area left below the browse row, so the row
  // never overlaps content — it costs one row of height, stated here once.
  const shelfui::GridArea content = shelfui::gridAreaForCells(area);
  metrics_ = shelfui::gridMetrics(content);
  rowMetrics_ = shelfui::listMetrics(content);
}

int ShelfActivity::pageCapacity() const {
  return shelfView_ == shelfui::ShelfView::List ? shelfui::listPageCapacity(rowMetrics_)
                                                : shelfui::pageCapacity(metrics_);
}

// "Все книги (43) / Авторы (18) / Серии (7) / Жанры (5)": counts are of books,
// not of values, the same number the filter categories show.
void ShelfActivity::buildChips() {
  static constexpr shelf::FilterField kChipFields[] = {shelf::FilterField::None, shelf::FilterField::Author,
                                                       shelf::FilterField::Series, shelf::FilterField::Genre};
  static constexpr StrId kChipLabels[] = {StrId::STR_ALL_BOOKS, StrId::STR_BROWSE_AUTHORS, StrId::STR_BROWSE_SERIES,
                                          StrId::STR_BROWSE_GENRES};
  chips_.clear();
  chips_.reserve(static_cast<size_t>(Chip::Count));
  for (int i = 0; i < static_cast<int>(Chip::Count); ++i) {
    shelfui::ChipView chip;
    const int count = kChipFields[i] == shelf::FilterField::None ? static_cast<int>(view_.size())
                                                                 : query_.countWith(index_, kChipFields[i], &stats_);
    chip.label = withCount(I18n::getInstance().get(kChipLabels[i]), count);
    chips_.push_back(std::move(chip));
  }
}

void ShelfActivity::applyChip(Chip chip) {
  static constexpr shelf::FilterField kChipFields[] = {shelf::FilterField::None, shelf::FilterField::Author,
                                                       shelf::FilterField::Series, shelf::FilterField::Genre};
  const shelf::FilterField field = kChipFields[static_cast<int>(chip)];
  if (field == shelf::FilterField::None) {
    query_.filter.clear();
    applyQuery();
    saveState();
    refreshPage();
    requestUpdate();
    return;
  }
  // The chip is a way into the same value picker the filter overlay uses: the
  // row chooses the dimension, the picker chooses the value.
  openFilterValues(field);
}

bool ShelfActivity::focusChips(bool focused) {
  if (chipsFocused_ == focused) return false;
  chipsFocused_ = focused;
  if (focused) {
    // Entering the row puts the cursor on the browse currently in effect.
    static constexpr shelf::FilterField kChipFields[] = {shelf::FilterField::None, shelf::FilterField::Author,
                                                         shelf::FilterField::Series, shelf::FilterField::Genre};
    chipIndex_ = 0;
    for (int i = 1; i < static_cast<int>(Chip::Count); ++i) {
      if (query_.filter.field == kChipFields[i]) chipIndex_ = i;
    }
  }
  requestUpdate();
  return true;
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
  // One statistics snapshot per query pass: the time/progress sort keys and the
  // reading-status filter both need it, and rebuilding it per book would call
  // into the stats store hundreds of times per repaint.
  stats_.clear();
  stats_.reserve(index_.books.size());
  for (const auto& book : index_.books) {
    const std::string docId = KOReaderDocumentId::calculateFromFilename(book.path);
    const BookReadingStats* counters = docId.empty() ? nullptr : ReadingStatsStore::getInstance().findBook(docId);
    shelf::SortStat stat;
    if (counters != nullptr) {
      stat.lastOpenedEpoch = static_cast<uint32_t>(counters->lastReadEpoch);
      stat.progressPercent = counters->progress;
    }
    stats_.push_back(stat);
  }
  query_.apply(index_, view_, &stats_);
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
  const int capacity = pageCapacity();
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
  const int capacity = pageCapacity();
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
  seed.year = book->meta.year;
  seed.publisher = book->meta.publisher;
  seed.pages = book->meta.pages;
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

// An empty grid is not one state but three, and each has its own way out:
// nothing indexed yet (rescan), a filter that matches nothing (drop it), or a
// library whose every book is hidden (show them). Offering "rescan" for all
// three — the old text-only behaviour — was what made a filter look like a
// lost library.
ShelfActivity::EmptyAction ShelfActivity::emptyAction() const {
  if (!view_.empty()) return EmptyAction::None;
  if (index_.books.empty()) return EmptyAction::Rescan;
  if (query_.filter.active()) return EmptyAction::ClearFilter;
  if (!hiddenPaths_.empty()) return EmptyAction::ShowHidden;
  return EmptyAction::Rescan;
}

const char* ShelfActivity::emptyActionLabel(EmptyAction action) const {
  switch (action) {
    case EmptyAction::Rescan:
      return tr(STR_SHELF_REFRESH);
    case EmptyAction::ClearFilter:
      return tr(STR_CLEAR_FILTER);
    case EmptyAction::ShowHidden:
      return tr(STR_HIDDEN_BOOKS);
    case EmptyAction::None:
      break;
  }
  return "";
}

void ShelfActivity::runEmptyAction() {
  switch (emptyAction()) {
    case EmptyAction::Rescan:
      // The same explicit full rebuild the menu offers: "refresh" is what the
      // user presses after adding books, and here it is also the way to make an
      // empty card produce a library.
      mode_ = Mode::Scanning;
      requestUpdate(true);
      return;
    case EmptyAction::ClearFilter:
      query_.filter.clear();
      applyQuery();
      saveState();
      refreshPage();
      requestUpdate();
      return;
    case EmptyAction::ShowHidden:
      openHiddenBooks();
      requestUpdate();
      return;
    case EmptyAction::None:
      return;
  }
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
  // The list is the fast way to scan the library, so the switch sits next to the
  // hidden books instead of inside a settings screen.
  add(MenuAction::View, shelfView_ == shelfui::ShelfView::Grid ? StrId::STR_VIEW_LIST : StrId::STR_VIEW_GRID);
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
    case MenuAction::View:
      // One press flips the presentation; the page is rebuilt because the row
      // height and the page capacity change with it.
      shelfView_ = shelfView_ == shelfui::ShelfView::Grid ? shelfui::ShelfView::List : shelfui::ShelfView::Grid;
      saveState();
      refreshPage();
      requestUpdate();
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
      const int cell = selected_ % std::max(1, pageCapacity());
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

bool ShelfActivity::syncPartialChipsRefresh() {
  if (!chipsFocused_ || chips_.empty()) return false;
  const Rect row = shelfui::chipsRowBounds(pageArea_);
  if (row.width <= 0 || row.height <= 0) return false;
  renderer.syncWriteBufferFromDisplayed();
  renderer.fillRect(row.x, row.y, row.width, row.height, false);
  shelfui::ChipsRow view;
  view.chips = chips_.data();
  view.count = chips_.size();
  view.active = chipIndex_;
  view.focused = chipIndex_;
  shelfui::drawChipsRow(renderer, pageArea_, view);
  ++chipsWindowRefreshes_;
  chipsMoved_ = false;
  renderer.displayWindow(row.x, row.y, row.width, row.height);
  return true;
}

bool ShelfActivity::syncPartialFocusRefresh() {
  const int capacity = pageCapacity();
  if (capacity <= 0 || !gridFrameShown_) return false;
  const int count = static_cast<int>(cells_.size());
  if (pendingCellFrom_ >= count || pendingCellTo_ >= count) return false;

  renderer.syncWriteBufferFromDisplayed();
  const int selectedLocal = selected_ % capacity;
  if (shelfView_ == shelfui::ShelfView::List) {
    // One book per row: a focus move touches two whole rows and fresh artwork one
    // row, repainted with the row renderer. Same window-update discipline as the
    // grid (a full page refresh here would cost the whole panel for one line).
    int left = INT_MAX;
    int top = INT_MAX;
    int right = 0;
    int bottom = 0;
    std::vector<int> rows = pendingCoverRepaints_;
    for (const int index : {pendingCellFrom_, pendingCellTo_, lastSelectedLocal_}) {
      if (index < 0 || index >= count) continue;
      if (std::find(rows.begin(), rows.end(), index) == rows.end()) rows.push_back(index);
    }
    for (const int index : rows) {
      if (index < 0 || index >= count) continue;
      const Rect row = shelfui::listRowBounds(rowMetrics_, index);
      renderer.fillRect(row.x, row.y, row.width, row.height, false);
      shelfui::drawShelfRow(renderer, rowMetrics_, cells_[index], index, selectedLocal, tr(STR_NO_COVER));
      left = std::min(left, row.x);
      top = std::min(top, row.y);
      right = std::max(right, row.x + row.width);
      bottom = std::max(bottom, row.y + row.height);
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
  if (overlay_ == Overlay::FilterValues) return filterValueLabels_;
  if (overlay_ == Overlay::Menu) return menuLabels_;
  if (overlay_ == Overlay::HiddenBooks) return hiddenLabels_;

  items.clear();
  if (overlay_ == Overlay::BookMenu) {
    for (int i = 0; i < static_cast<int>(BookAction::Count); ++i) {
      items.emplace_back(actionLabel(static_cast<BookAction>(i)));
    }
    return items;
  }
  if (overlay_ == Overlay::SortFields) {
    for (const SortEntry& entry : kSortEntries) {
      std::string label = sortEntryLabel(entry.key);
      if (pendingSort_ == entry.key) label += " *";
      items.push_back(std::move(label));
    }
    return items;
  }
  if (overlay_ == Overlay::SortDirection) {
    // Ascending first, then descending: the two labels are the existing sort
    // strings, so the direction step reads the same as the rest of the UI.
    items.emplace_back(tr(STR_SORT_ASC));
    items.emplace_back(tr(STR_SORT_DESC));
    items[pendingDescending_ ? 1 : 0] += " *";
    return items;
  }
  if (overlay_ != Overlay::FilterCategories) return items;

  items.clear();
  items.emplace_back(tr(STR_ALL_BOOKS));
  for (const shelf::FilterField field : kBrowseFields) {
    // The number is how many books carry a value for this dimension, not how
    // many distinct values exist: the row answers "is there anything to browse".
    std::string label = withCount(filterLabel(field), query_.countWith(index_, field, &stats_));
    if (query_.filter.field == field) label += " *";
    items.push_back(std::move(label));
  }
  items.push_back(sortRowLabel(query_.sort, query_.descending));
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
  filterValueLabels_.clear();
  // "All books" is always the first value: the only way back to the whole
  // library without remembering which value was active before.
  filterValues_.emplace_back();
  filterValueLabels_.emplace_back(tr(STR_ALL_BOOKS));
  // Values carry how many books sit behind them: the same number the Android
  // library shows next to a category, and the reason a picker of authors is
  // worth opening at all.
  std::vector<shelf::FilterValueCount> counts;
  query_.collectValueCounts(index_, field, counts, 200, &stats_);
  for (auto& entry : counts) {
    filterValues_.push_back(std::move(entry.value));
    filterValueLabels_.push_back(withCount(filterValueLabel(field, filterValues_.back()), entry.count));
  }
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
      // 0 = all books, 1..kBrowseFieldCount = the dimensions, then sort.
      if (index == 0) {
        query_.filter.clear();
      } else if (index >= 1 && index <= kBrowseFieldCount) {
        openFilterValues(kBrowseFields[index - 1]);
        return;
      } else {
        // Sort is two steps (field, then direction): stepping into the field list
        // must not apply anything yet, or Cancel would still change the grid.
        pendingSort_ = query_.sort;
        pendingDescending_ = query_.descending;
        menuIndex_ = 0;
        for (size_t i = 0; i < shelf::kSortKeyCount; ++i) {
          if (kSortEntries[i].key == query_.sort) menuIndex_ = static_cast<int>(i);
        }
        overlay_ = Overlay::SortFields;
        requestUpdate();
        return;
      }
      break;
    }
    case Overlay::SortFields: {
      if (index < 0 || index >= static_cast<int>(shelf::kSortKeyCount)) return;
      // Stage the key, do not apply it: the direction step starts from the key's
      // natural end ("недавние" = newest first) and the two steps are committed
      // together, so backing out here leaves the library exactly as it was.
      pendingSort_ = kSortEntries[index].key;
      pendingDescending_ = shelf::naturalDescending(pendingSort_);
      overlay_ = Overlay::SortDirection;
      menuIndex_ = pendingDescending_ ? 1 : 0;
      requestUpdate(true);
      return;
    }
    case Overlay::SortDirection: {
      query_.sort = pendingSort_;
      query_.descending = index == 1;
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
  state.descending = query_.descending;
  state.view = shelfui::shelfViewName(shelfView_);
  state.hiddenPaths = hiddenPaths_;
  stateStore_.save(state);
}
