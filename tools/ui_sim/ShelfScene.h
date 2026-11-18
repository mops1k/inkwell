#pragma once

#include <GfxRenderer.h>

#include <string>
#include <vector>

#include "Scene.h"
#include "activities/shelf/ShelfGridRenderer.h"

// Host scene that renders the REAL shelf page renderer with fixture books. It
// exists so the layout can be checked without a device and without an SD card;
// it deliberately uses the same shelfui::drawShelfPage the device screen calls.
// The chrome around the grid (header, button hints, side hints) is drawn by the
// REAL theme, exactly as on the device — the grid only fills the content box the
// theme leaves free.
namespace uisim {

class ShelfScene : public Scene {
 public:
  ShelfScene();

  void onKey(const std::string& key) override;
  void render(GfxRenderer& renderer) const override;

  // Points the cells at real 2-bit BMP cover files (see CoverFixtures). Without
  // this the cells draw the placeholder frame, which is the device's "cover not
  // generated yet" state.
  void useCovers(const std::vector<std::string>& covers);

  // Pre-applies the "Автор: Лев Толстой" filter for the shelf_filter scene, so
  // the frame shows the filtered state without scripting key presses.
  void applyDemoFilter();

 private:
  // One fixture book keeps the shelf cell and the metadata the filter menu
  // needs; the device reads the same fields from ShelfIndex.
  struct Book {
    shelfui::CellView cell;
    std::string series;
    std::string genre;
  };

  // Mirrors ShelfActivity's overlay state machine: the shelf, the filter
  // category menu, or the list of values of the selected category.
  enum class Overlay { None, Categories, Values };

  shelfui::GridMetrics gridMetrics(const GfxRenderer& renderer) const;
  int pageStart(const shelfui::GridMetrics& metrics) const;
  // Indices of books_ the current filter leaves visible, in shelf order.
  std::vector<size_t> visible() const;
  std::vector<std::string> overlayItems() const;
  void openCategories();
  void openValues();
  void activateItem();

  std::vector<Book> books_;
  std::vector<std::string> values_;
  std::string filterField_;  // "Автор"/"Серия"/"Жанр" while a value list is open
  std::string filterValue_;  // applied filter value, empty = whole library
  Overlay overlay_ = Overlay::None;
  int menuIndex_ = 0;
  int categoryIndex_ = 0;
  int selected_ = 0;
};

}  // namespace uisim
