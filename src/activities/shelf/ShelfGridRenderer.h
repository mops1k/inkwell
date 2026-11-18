#pragma once

#include <GfxRenderer.h>

#include <cstddef>
#include <string>

// Rendering of one shelf page, shared by the device screen (ShelfActivity) and
// the host harness (tools/ui_sim). Nothing here loads data: the caller hands in
// ready-to-draw cells, so the same code renders real books and test fixtures
// (AGENTS.md: "общая логика — один компонент и один вход").
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
  const char* title = "";  // header, e.g. "Полка"
  const char* hint = "";   // footer button hint
  const char* emptyText = "";
};

// Geometry of the grid. Derived from the screen so the UI follows a size change
// instead of hard-coding 528x792 in two places.
struct GridMetrics {
  int columns = 2;
  int rows = 2;
  int marginX = 16;
  int headerTop = 14;
  int gridTop = 52;
  int cellW = 180;
  int cellH = 240;
  int captionH = 44;  // title line + author line + progress bar
  int gapX = 28;
  int gapY = 24;
};

GridMetrics gridMetrics(int screenWidth, int screenHeight);

// How many books fit on one page.
int pageCapacity(const GridMetrics& metrics);

// Draws header, cells, selection, page counter and footer hints. Assumes the
// caller already cleared the frame; the caller also presents it.
void drawShelfPage(GfxRenderer& renderer, const GridMetrics& metrics, const PageView& page);

}  // namespace shelfui
