#pragma once

#include <ShelfIndex.h>
#include <ShelfIndexStore.h>
#include <ShelfIndexer.h>
#include <ShelfStateStore.h>

#include <string>
#include <utility>
#include <vector>

#include "../Activity.h"
#include "ShelfCoverService.h"
#include "ShelfGridRenderer.h"

// The shelf screen: a grid of indexed books with covers, a 4-way focus model,
// page-scoped covers and a long-Confirm menu. It owns no indexing rules — the
// library index (lib/Shelf) and the cover pipeline (ShelfCoverService) do — and
// no drawing rules: ShelfGridRenderer draws the page for both this activity and
// the host harness.
class ShelfActivity final : public Activity {
 public:
  explicit ShelfActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string focusBookPath = {})
      : Activity("Shelf", renderer, mappedInput), focusBookPath_(std::move(focusBookPath)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Long-Confirm menu. Order is the on-screen order; public because the label
  // table lives next to the activity in the .cpp.
  enum class BookAction : uint8_t { Open, Info, RefreshCover, DeleteCache, Count };

 private:
  enum class Mode : uint8_t { Loading, Scanning, Grid };

  // Overlays share one list widget: the book menu and the filter picker differ
  // only in their item labels and the action a Confirm runs.
  enum class Overlay : uint8_t { None, BookMenu, FilterCategories, FilterValues };

  void rebuildIndex();
  shelf::ShelfScanOptions scanOptions() const;  // /books + SD root + OPDS downloads folder
  void rescanLibrary();                         // incremental, on every entry to the shelf
  void updateLayout();                          // content rect + grid metrics for the current theme chrome
  void refreshPage();                           // rebuilds cells_ for the current page, resolves covers
  void applyQuery();                            // re-filters/sorts the index into view_ and clamps the focus
  void moveSelection(int delta);
  void openSelected();
  void runBookAction(BookAction action);
  void openFilters();
  void openFilterValues(shelf::FilterField field);
  const std::vector<std::string>& overlayItems() const;
  void activateOverlayItem(Overlay source, int index);
  void drawMenu();
  void drawOverlayList(const std::vector<std::string>& items, int selected) const;
  void saveState();

  const shelf::ShelfBook* selectedBook() const;

  shelf::ShelfIndex index_;
  shelf::ShelfIndexStore store_;
  shelf::ShelfStateStore stateStore_;
  shelf::LibraryQuery query_;
  ShelfCoverService covers_;
  shelfui::GridMetrics metrics_;
  std::vector<shelfui::CellView> cells_;
  // Indices into index_.books in display order; the grid talks only to this.
  std::vector<uint16_t> view_;
  // Values of the currently opened filter field (FilterValues overlay only).
  std::vector<std::string> filterValues_;
  shelf::FilterField pendingField_ = shelf::FilterField::None;

  std::string focusBookPath_;
  Mode mode_ = Mode::Loading;
  Overlay overlay_ = Overlay::None;
  int menuIndex_ = 0;
  int selected_ = 0;  // index into view_
  bool rescanPending_ = false;
};
