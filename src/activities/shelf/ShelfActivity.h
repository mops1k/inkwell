#pragma once

#include <ShelfIndex.h>
#include <ShelfIndexStore.h>
#include <ShelfIndexer.h>
#include <ShelfStateStore.h>

#include <string>
#include <utility>
#include <vector>

#include "../Activity.h"
#include "CoverWorker.h"
#include "CrossPointState.h"
#include "ShelfCoverService.h"
#include "ShelfGridRenderer.h"

// The shelf screen: a grid of indexed books with covers, a 4-way focus model,
// page-scoped covers and a long-Confirm menu. It owns no indexing rules — the
// library index (lib/Shelf) and the cover pipeline (ShelfCoverService) do — and
// no drawing rules: ShelfGridRenderer draws the page for both this activity and
// the host harness.
class ShelfActivity final : public Activity {
 public:
  explicit ShelfActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string focusBookPath = {},
                         int focusSelectorIndex = -1)
      : Activity("Shelf", renderer, mappedInput),
        focusBookPath_(std::move(focusBookPath)),
        focusSelectorIndex_(focusSelectorIndex) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // State the quick-sleep path preserves across deep sleep: the shelf is the home
  // screen now, so it takes over what HomeActivity used to hand to the sleep
  // screen (which grid cell was focused).
  HomeSleepState getSleepState() const;

  // Long-Confirm menu. Order is the on-screen order; public because the label
  // table lives next to the activity in the .cpp.
  enum class BookAction : uint8_t { Open, Info, RefreshCover, DeleteCache, Hide, Count };

 private:
  enum class Mode : uint8_t { Loading, Scanning, Grid };

  // Short Back opens the main menu over the grid, in the same list widget as the
  // filters (the user asked for the menu to look like the filter popup). The
  // library itself is absent from it: the library IS the main screen now.
  enum class MenuAction : uint8_t { Files, Recents, Opds, Transfer, Settings, Hidden, View, Refresh, Count };

  // Overlays share one list widget: the book menu and the filter picker differ
  // only in their item labels and the action a Confirm runs.
  enum class Overlay : uint8_t {
    None,
    BookMenu,
    FilterCategories,
    FilterValues,
    HiddenBooks,
    Menu,
    SortFields,
    SortDirection
  };

  void rebuildIndex();
  // Books per page for the current presentation. One entry point: navigation,
  // page building and the footer all ask this instead of guessing the mode.
  int pageCapacity() const;
  // Browse row above the grid: "Все / Авторы / Серии / Жанры". Up from the top
  // row focuses it, Left/Right move, Confirm applies. Independent of the
  // long-Back filter overlay: the row answers "which view", the overlay narrows
  // the view by a concrete value.
  enum class Chip : uint8_t { All = 0, Authors, Series, Genres, Count };
  void buildChips();
  void applyChip(Chip chip);
  bool focusChips(bool focused);  // returns true when the focus actually moved
  bool syncPartialChipsRefresh();
  shelf::ShelfScanOptions scanOptions() const;  // /books + SD root + OPDS downloads folder
  void updateLayout();                          // content rect + grid metrics for the current theme chrome
  void refreshPage();                           // rebuilds cells_ for the current page, resolves covers
  void collectCoverResults();                   // takes finished artwork from the worker (never blocks)
  void applyQuery();                            // re-filters/sorts the index into view_ and clamps the focus
  void moveSelection(int delta);
  void openSelected();
  void openBookCard();
  void runBookAction(BookAction action);
  void openFilters();
  void openFilterValues(shelf::FilterField field);
  void openHiddenBooks();
  void setHidden(const std::string& path, bool hidden);
  void openMainMenu();
  void runMenuAction(MenuAction action);
  // What Confirm does when the grid is empty: rescan, drop the filter, or show
  // the hidden books. None when there is something to open.
  enum class EmptyAction : uint8_t { None, Rescan, ClearFilter, ShowHidden };
  EmptyAction emptyAction() const;
  void runEmptyAction();
  const char* emptyActionLabel(EmptyAction action) const;
  const std::vector<std::string>& overlayItems() const;
  void activateOverlayItem(Overlay source, int index);
  void drawMenu();
  void drawOverlayList(const std::vector<std::string>& items, int selected) const;
  shelfui::OverlayGeometry drawOverlayContents();  // draws the open pop-up, remembers its geometry
  shelfui::OverlayGeometry overlayGeometry_;
  bool syncPartialFocusRefresh();  // repaints the two cells whose focus changed
  void saveState();

  const shelf::ShelfBook* selectedBook() const;

  // Reading progress lives in the stats store; it is loaded for the duration of
  // each rebuild/lookup (see refreshPage/applyQuery) rather than pinned for the
  // whole activity, because the pinned load can happen before storage is ready.
  shelf::ShelfIndex index_;
  shelf::ShelfIndexStore store_;
  shelf::ShelfStateStore stateStore_;
  shelf::LibraryQuery query_;
  ShelfCoverService covers_;
  // Artwork is decoded by this task, never on the activity thread: a .fb2.zip
  // cover inflates the whole book, and waiting for that under the user's finger
  // was the grid's remaining hesitation.
  shelfui::CoverWorker coverWorker_;
  shelfui::GridMetrics metrics_;
  // The same page area, laid out as rows for the list presentation.
  shelfui::RowMetrics rowMetrics_;
  shelfui::ShelfView shelfView_ = shelfui::ShelfView::Grid;
  // The full page area (grid + browse row) and the row's own contents.
  shelfui::GridArea pageArea_{};
  std::vector<shelfui::ChipView> chips_;
  bool chipsFocused_ = false;
  int chipIndex_ = 0;
  int chipsWindowRefreshes_ = 0;
  bool chipsMoved_ = false;  // the last change was a move inside the row
  static constexpr int kMaxChipsWindowRefreshes = 12;
  std::vector<shelfui::CellView> cells_;
  // Cells whose artwork arrived while the page was already on screen: they get a
  // full re-render (cover pixels changed), unlike a focus move that only redraws
  // the selection ring.
  std::vector<int> pendingCoverRepaints_;
  // Which cell carried the ring after the last frame. Erasing it explicitly keeps a
  // stale ring from surviving when a repaint arrives between two focus moves (that
  // is how the ring once appeared on the leftmost and the rightmost cell at once).
  int lastSelectedLocal_ = -1;
  // Indices into index_.books in display order; the grid talks only to this.
  std::vector<uint16_t> view_;
  // Values of the currently opened filter field (FilterValues overlay only) and
  // their display labels — the label carries the book count, the raw value is
  // what gets stored in the query, so they are kept in parallel.
  std::vector<std::string> filterValues_;
  std::vector<std::string> filterValueLabels_;
  // Footer line built in render(): owned here so PageView can point at it.
  std::string footerText_;
  // Reading statistics aligned with index_.books, rebuilt in applyQuery():
  // progress/last-read sort keys and the reading-status filter read it.
  std::vector<shelf::SortStat> stats_;
  std::vector<MenuAction> menuActions_;
  std::vector<std::string> menuLabels_;
  // Hidden books survive rescans: paths live in the saved view state, the index
  // entry stays (so unhiding needs no rescan).
  std::vector<std::string> hiddenPaths_;
  std::vector<std::string> hiddenLabels_;
  std::vector<std::string> hiddenItems_;
  shelf::FilterField pendingField_ = shelf::FilterField::None;

  // Pop-up repaint bookkeeping: while the same pop-up stays open, a move redraws
  // the pop-up in the buffer and refreshes only the two rows whose focus changed.
  Rect overlayRect_{};
  Overlay overlayRectKind_ = Overlay::None;
  int overlaySelectedLast_ = -1;
  bool overlayRectValid_ = false;
  bool gridFrameShown_ = false;
  int overlayWindowRefreshes_ = 0;
  static constexpr int kMaxOverlayWindowRefreshes = 12;

  // Focus move inside one page: repaint the two affected cells as a window
  // update instead of a full-screen e-ink refresh (which costs seconds here).
  int pendingCellFrom_ = -1;
  int pendingCellTo_ = -1;
  int cellWindowRefreshes_ = 0;
  static constexpr int kMaxCellWindowRefreshes = 12;

  std::string focusBookPath_;
  int focusSelectorIndex_ = -1;
  Mode mode_ = Mode::Loading;
  Overlay overlay_ = Overlay::None;
  int menuIndex_ = 0;
  int selected_ = 0;  // index into view_
  // The sort picker is two steps (field, then direction) and both are collected
  // before the library changes: choosing a field only stages it here, so backing
  // out of the direction step leaves the current sort untouched.
  shelf::SortKey pendingSort_ = shelf::SortKey::Title;
  bool pendingDescending_ = false;
};
