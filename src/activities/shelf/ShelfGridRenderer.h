#pragma once

#include <GfxRenderer.h>

#include <cstddef>
#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"

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
  std::string format;        // short uppercase badge ("EPUB", "FB2"); empty = no badge
  int progressPercent = -1;  // -1 unknown, otherwise 0..100
};

struct PageView {
  const CellView* cells = nullptr;
  size_t count = 0;
  int selected = 0;  // index into cells, -1 when the page is empty
  int page = 0;      // zero-based
  int pageCount = 1;
  const char* emptyText = "";
  // Drawn inside an empty cover frame when the book has no title of its own.
  // A string, not an i18n lookup: this file is shared with the host harness,
  // which links no translations.
  const char* noCoverText = "";
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

// One cell's frame including its caption and selection border. Used by the
// partial refresh: moving the focus repaints just the two affected cells instead
// of a full-screen e-ink update.
Rect cellBounds(const GridMetrics& metrics, int indexInPage);

// Draws one cell (cover, chip, progress, caption, selection frame). Shared by the
// full page draw and the partial focus refresh, so both stay identical.
void drawShelfCell(GfxRenderer& renderer, const GridMetrics& metrics, const CellView& cell, int indexInPage,
                   int selected, const char* noCoverText);

// The focus indicator is the caption block of a cell (title + author), drawn as an
// inverted plate when selected. A frame around the whole cell looked right but its
// e-ink window refresh cost ~20x the pixels; this block is small enough to feel
// instant while still showing who wrote the book.
Rect choiceBandBounds(GfxRenderer& renderer, const GridMetrics& metrics, int indexInPage);
void drawCellTitleBand(GfxRenderer& renderer, const GridMetrics& metrics, int indexInPage, const std::string& title,
                       const std::string& author, bool selected);
// The texts as the cell shows them (fitted to the cell width). Exposed so a partial
// refresh draws exactly the same strings as the full page did.
std::string fitCellTitle(GfxRenderer& renderer, const GridMetrics& metrics, const std::string& title);
std::string fitCellAuthor(GfxRenderer& renderer, const GridMetrics& metrics, const std::string& author);

// The "no artwork" box: a framed rectangle with the book title centred inside it.
// Shared by a shelf cell and the book card, so a book without a cover is never an
// empty hole on either screen.
void drawCoverPlaceholderBox(GfxRenderer& renderer, const Rect& box, const std::string& title, const char* hint);

// Geometry of one pop-up: where its box sits and how tall a row is. Callers use
// the row bounds to refresh only the two rows whose focus changed — a whole-box
// window update was still visibly slower than the rest of the UI.
struct OverlayGeometry {
  Rect box;
  int rowHeight = 0;
  int firstRow = 0;  // first visible item (lists scroll when they grow)
  int visibleRows = 0;
};

// Row rectangle (a little taller than the row so the focus fill is covered).
Rect overlayRowBounds(const OverlayGeometry& geometry, int itemIndex);

// Centered list overlay (book menu, filter categories, filter values). Shared
// with the host harness so a screenshot of the filter flow is the real widget:
// a filled row for the focus, one line per item, width capped to the panel.
OverlayGeometry drawOverlayList(GfxRenderer& renderer, const std::vector<std::string>& items, int selected);

// Same list with a 32x32 icon per row, the way the main menu showed its entries.
// The focused row is framed instead of filled so a black-on-white icon stays
// readable; items and icons are matched by index.
OverlayGeometry drawOverlayIconList(GfxRenderer& renderer, const std::vector<std::string>& items,
                                    const std::vector<const uint8_t*>& icons, int selected);

}  // namespace shelfui
