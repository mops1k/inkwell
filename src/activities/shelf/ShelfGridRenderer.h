#pragma once

#include <GfxRenderer.h>

#include <cstddef>
#include <string>
#include <vector>

// Rendering of one shelf page, shared by the device screen (ShelfActivity) and
// the host harness (tools/ui_sim). Nothing here loads data: the caller hands in
// ready-to-draw cells, so the same code renders real books and test fixtures
// (AGENTS.md: "общая логика — один компонент и один вход").
//
// The renderer draws ONLY the grid and its page counter. Chrome around it —
// header and the button hint bar with its framed key labels — belongs to the
// theme (BaseTheme), exactly like the main screen, so the shelf cannot drift
// away from the rest of the UI.
namespace shelfui {

// One visible cell. coverPath is a BMP the cover service already produced; an
// empty path means "no cover yet" and the cell draws its frame instead.
struct CellView {
  std::string title;
  std::string author;
  std::string coverPath;
  int progressPercent = -1;  // -1 unknown, otherwise 0..100
};

struct PageView {
  const CellView* cells = nullptr;
  size_t count = 0;
  int selected = 0;  // index into cells, -1 when the page is empty
  int page = 0;      // zero-based
  int pageCount = 1;
  const char* emptyText = "";
};

// Drawable area the caller reserved for the grid (theme content rect minus the
// header). Keeping it explicit is what lets the shelf honour the theme's hint
// bar instead of guessing the footer height.
struct GridArea {
  int x = 16;
  int y = 52;
  int width = 528 - 32;
  int height = 792 - 52 - 44;
};

// Geometry of the grid, derived from the area so a size or theme change does not
// require editing constants in two places.
struct GridMetrics {
  int columns = 2;
  int rows = 2;
  int cellW = 180;
  int cellH = 240;
  int captionH = 44;  // title line + author line + progress bar
  int gapX = 28;
  int gapY = 24;
  GridArea area;
};

GridMetrics gridMetrics(const GridArea& area);

// How many books fit on one page.
int pageCapacity(const GridMetrics& metrics);

// Draws cells, selection frame and the page counter. Assumes the caller already
// cleared the frame and drew the theme chrome; the caller also presents it.
void drawShelfPage(GfxRenderer& renderer, const GridMetrics& metrics, const PageView& page);

// Centered list overlay (book menu, filter categories, filter values). Shared
// with the host harness so a screenshot of the filter flow is the real widget:
// a filled row for the focus, one line per item, width capped to the panel.
void drawOverlayList(GfxRenderer& renderer, const std::vector<std::string>& items, int selected);

}  // namespace shelfui
