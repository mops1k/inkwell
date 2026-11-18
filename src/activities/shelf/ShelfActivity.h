#pragma once

#include <ShelfIndex.h>
#include <ShelfIndexStore.h>

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

  void rebuildIndex();
  void updateLayout();  // content rect + grid metrics for the current theme chrome
  void refreshPage();   // rebuilds cells_ for the current page, resolves covers
  void moveSelection(int delta);
  void openSelected();
  void runBookAction(BookAction action);
  void drawMenu();

  const shelf::ShelfBook* selectedBook() const;

  shelf::ShelfIndex index_;
  shelf::ShelfIndexStore store_;
  ShelfCoverService covers_;
  shelfui::GridMetrics metrics_;
  std::vector<shelfui::CellView> cells_;

  std::string focusBookPath_;
  Mode mode_ = Mode::Loading;
  bool menuOpen_ = false;
  int menuIndex_ = 0;
  int selected_ = 0;  // flat index into index_.books
};
