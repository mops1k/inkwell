#include "ShelfGridRenderer.h"

#include <Bitmap.h>
#include <CoverPolicy.h>
#include <HalStorage.h>
#include <fontIds.h>

#include <algorithm>
#include <cstdio>

namespace shelfui {
namespace {

constexpr int kHeaderFont = UI_12_FONT_ID;
constexpr int kCaptionFont = SMALL_FONT_ID;
constexpr int kSelectionInset = 3;

// Rectangle of the selection ring: the artwork box grown by the ring width, so the
// ring never overlaps the caption or the author line underneath it.
Rect focusRingRect(const GridMetrics& metrics, int indexInPage) {
  const GridArea& area = metrics.area;
  const int column = indexInPage % metrics.columns;
  const int row = indexInPage / metrics.columns;
  const int x = area.x + column * (metrics.cellW + metrics.gapX);
  const int y = area.y + row * (metrics.cellH + metrics.captionH + metrics.gapY);
  return Rect{x - kSelectionInset, y - kSelectionInset, metrics.cellW + kSelectionInset * 2,
              metrics.cellH + kSelectionInset * 2};
}
constexpr int kCoverInset = 1;

// Truncates to fit `maxWidth`, appending an ellipsis only when the text really
// does not fit — avoids a dotted stub for a title that barely overflows.
std::string fitText(GfxRenderer& renderer, int fontId, const std::string& text, int maxWidth) {
  if (text.empty()) return {};
  if (renderer.getTextWidth(fontId, text.c_str()) <= maxWidth) return text;
  return renderer.truncatedText(fontId, text.c_str(), maxWidth);
}

// A cell without artwork shows the book itself: the title, centered and wrapped
// inside the frame. Before, it drew only corner ticks, which told the reader
// nothing about which book they were looking at.
void drawPlaceholderCover(GfxRenderer& renderer, const CellView& cell, const char* noCoverText, int x, int y, int width,
                          int height) {
  renderer.drawRect(x, y, width, height, true);
  constexpr int kTick = 14;
  renderer.drawLine(x, y, x + kTick, y, 2, true);
  renderer.drawLine(x, y, x, y + kTick, 2, true);
  renderer.drawLine(x + width - kTick, y, x + width, y, 2, true);
  renderer.drawLine(x + width, y, x + width, y + kTick, 2, true);
  renderer.drawLine(x, y + height - kTick, x, y + height, 2, true);
  renderer.drawLine(x, y + height, x + kTick, y + height, 2, true);
  renderer.drawLine(x + width - kTick, y + height, x + width, y + height, 2, true);
  renderer.drawLine(x + width, y + height - kTick, x + width, y + height, 2, true);

  const std::string label = cell.title.empty() ? std::string(noCoverText) : cell.title;
  if (label.empty()) return;
  constexpr int kPadding = 12;
  const int textWidth = std::max(1, width - kPadding * 2);
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int maxLines = std::max(1, (height - kPadding * 2) / lineHeight);
  const auto lines = renderer.wrappedText(SMALL_FONT_ID, label.c_str(), textWidth, maxLines);
  int textY = y + (height - static_cast<int>(lines.size()) * lineHeight) / 2;
  for (const auto& line : lines) {
    // Centered inside the cell, not on the panel: drawCenteredText centres on the
    // screen, which put the title of an empty cell in the middle of the page.
    const int lineWidth = renderer.getTextWidth(SMALL_FONT_ID, line.c_str());
    renderer.drawText(SMALL_FONT_ID, x + (width - lineWidth) / 2, textY, line.c_str());
    textY += lineHeight;
  }
}

// Aspect-fit draw centered inside the box. drawBitmap only ever shrinks (it never
// scales artwork up) and always anchors at the top-left corner, so a cover whose
// proportions differ from the cell used to sit against the left edge with a white
// strip on the right. Centering the result puts the margins on both sides.
void drawCover(GfxRenderer& renderer, const CellView& cell, const char* noCoverText, int x, int y, int width,
               int height) {
  // White interior first: the selection frame behind a cell must not show
  // through the gaps of the artwork.
  renderer.fillRect(x, y, width, height, false);
  const int boxX = x + kCoverInset;
  const int boxY = y + kCoverInset;
  const int boxW = width - kCoverInset * 2;
  const int boxH = height - kCoverInset * 2;
  if (cell.coverPath.empty()) {
    drawPlaceholderCover(renderer, cell, noCoverText, boxX, boxY, boxW, boxH);
    renderer.drawRect(x, y, width, height, true);
    return;
  }

  FsFile file;
  if (!Storage.openFileForRead("Shelf", cell.coverPath, file)) {
    drawPlaceholderCover(renderer, cell, noCoverText, boxX, boxY, boxW, boxH);
    renderer.drawRect(x, y, width, height, true);
    return;
  }
  Bitmap bmp(file);
  const bool usable =
      bmp.parseHeaders() == BmpReaderError::Ok && bmp.isComplete() && bmp.getWidth() > 0 && bmp.getHeight() > 0;
  if (usable) {
    const float scale = std::min({1.0f, static_cast<float>(boxW) / static_cast<float>(bmp.getWidth()),
                                  static_cast<float>(boxH) / static_cast<float>(bmp.getHeight())});
    const int drawnW = std::max(1, static_cast<int>(static_cast<float>(bmp.getWidth()) * scale));
    const int drawnH = std::max(1, static_cast<int>(static_cast<float>(bmp.getHeight()) * scale));
    renderer.drawBitmap(bmp, boxX + (boxW - drawnW) / 2, boxY + (boxH - drawnH) / 2, drawnW, drawnH);
  }
  file.close();
  if (!usable) drawPlaceholderCover(renderer, cell, noCoverText, boxX, boxY, boxW, boxH);
  renderer.drawRect(x, y, width, height, true);
}

// Format badge, top-right of the cell: the same "what is this file" cue BookShelf
// shows on its cards. Drawn after the artwork so it stays readable on a dark cover.
void drawFormatChip(GfxRenderer& renderer, const char* label, int x, int y, int width) {
  if (label == nullptr || *label == '\0') return;
  constexpr int kPad = 4;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, label);
  const int chipW = textWidth + kPad * 2;
  const int chipH = lineHeight + 2;
  const int chipX = x + width - chipW - 4;
  const int chipY = y + 4;
  renderer.fillRect(chipX, chipY, chipW, chipH, false);
  renderer.drawRect(chipX, chipY, chipW, chipH, true);
  renderer.drawText(SMALL_FONT_ID, chipX + kPad, chipY + 1, label);
}

void drawProgress(GfxRenderer& renderer, int x, int y, int width, int progressPercent) {
  if (progressPercent < 0) return;
  const int clamped = std::clamp(progressPercent, 0, 100);
  const int lineHeight = renderer.getLineHeight(kCaptionFont);
  // Capped: with a large UI font the bar used to grow with the text and could eat
  // a noticeable part of the artwork. A progress bar stays a thin strip.
  const int barHeight = std::clamp(lineHeight + 4, 10, 20);
  const int barTop = y - barHeight;
  // White plate under the strip and the percentage: the bar sits on top of cover
  // art now, and black-on-black hid both the track and the digits on dark covers.
  const int plateHeight = std::max(barHeight, lineHeight + 2);
  renderer.fillRect(x, y - plateHeight, width, plateHeight, false);

  // Track, filled portion and a 1px white separator on top so the bar reads as
  // its own element rather than as part of the artwork.
  renderer.drawRect(x, barTop, width, barHeight, true);
  const int filled = width * clamped / 100;
  if (filled > 0) renderer.fillRect(x, barTop, filled, barHeight);
  renderer.drawLine(x, barTop, x + width - 1, barTop, false);

  char label[8];
  std::snprintf(label, sizeof(label), "%d%%", clamped);
  const int textWidth = renderer.getTextWidth(kCaptionFont, label);
  const int textY = barTop + (barHeight - lineHeight) / 2;

  // Per-glyph inversion: a digit whose centre lies on the black fill is drawn
  // white, the rest stay black on the white plate. Digits and '%' are ASCII, so
  // walking bytes is enough and each character is measured on its own.
  int penX = x + (width - textWidth) / 2;
  for (const char* glyph = label; *glyph != '\0'; ++glyph) {
    char text[2] = {*glyph, '\0'};
    const int glyphWidth = renderer.getTextWidth(kCaptionFont, text);
    const bool overFill = penX + glyphWidth / 2 < x + filled;
    renderer.drawText(kCaptionFont, penX, textY, text, !overFill);
    penX += glyphWidth;
  }
}

}  // namespace

GridMetrics gridMetrics(const GridArea& area) {
  // A cell keeps the 3:4 book-cover ratio; the width is what adapts. With two
  // fixed 180px cells the leftover width used to become one huge gutter between
  // them, which read as a half-empty page — a third column fits there instead.
  constexpr int kBaseCellW = 180;
  constexpr int kAspectH = 4;
  constexpr int kAspectW = 3;
  constexpr int kMinGapX = 8;
  constexpr int kThirdColumnMinW = 140;

  GridMetrics metrics;
  metrics.area = area;
  metrics.captionH = 40;
  metrics.gapY = 16;

  const int usable = std::max(1, area.width);
  int columns = std::max(1, (usable + kMinGapX) / (kBaseCellW + kMinGapX));
  if (columns < 3 && (usable - 2 * kMinGapX) / 3 >= kThirdColumnMinW) columns = 3;
  metrics.columns = columns;

  const int availableForCells = usable - kMinGapX * (columns - 1);
  metrics.cellW = std::clamp(availableForCells / columns, 1, kBaseCellW);
  metrics.cellH = metrics.cellW * kAspectH / kAspectW;
  metrics.gapX = columns > 1 ? std::max(kMinGapX, (usable - columns * metrics.cellW) / (columns - 1)) : 0;

  const int rowHeight = metrics.cellH + metrics.captionH + metrics.gapY;
  metrics.rows = std::max(1, (area.height + metrics.gapY) / rowHeight);
  return metrics;
}

int pageCapacity(const GridMetrics& metrics) { return std::max(1, metrics.columns * metrics.rows); }

Rect cellBounds(const GridMetrics& metrics, int indexInPage) {
  const GridArea& area = metrics.area;
  const int column = indexInPage % metrics.columns;
  const int row = indexInPage / metrics.columns;
  const int x = area.x + column * (metrics.cellW + metrics.gapX);
  const int y = area.y + row * (metrics.cellH + metrics.captionH + metrics.gapY);
  return Rect{x - kSelectionInset, y - kSelectionInset, metrics.cellW + kSelectionInset * 2,
              metrics.cellH + metrics.captionH + kSelectionInset * 2};
}

Rect choiceBandBounds(GfxRenderer& renderer, const GridMetrics& metrics, int indexInPage) {
  const GridArea& area = metrics.area;
  const int column = indexInPage % metrics.columns;
  const int row = indexInPage / metrics.columns;
  const int x = area.x + column * (metrics.cellW + metrics.gapX);
  const int y = area.y + row * (metrics.cellH + metrics.captionH + metrics.gapY);
  const int lineHeight = renderer.getLineHeight(kCaptionFont);
  // Title + author, plus a one-pixel margin so the plate edge lands inside the
  // refresh window. This is the whole focus indicator: an e-ink window refresh scales
  // with area, and a frame around a cell cost ~15x more pixels than this block.
  return Rect{x - 1, y + metrics.cellH + 3, metrics.cellW + 2, lineHeight * 2 + 2};
}

std::string fitCellTitle(GfxRenderer& renderer, const GridMetrics& metrics, const std::string& title) {
  return fitText(renderer, kCaptionFont, title, metrics.cellW);
}

std::string fitCellAuthor(GfxRenderer& renderer, const GridMetrics& metrics, const std::string& author) {
  return fitText(renderer, kCaptionFont, author, metrics.cellW);
}

void drawCellTitleBand(GfxRenderer& renderer, const GridMetrics& metrics, int indexInPage, const std::string& title,
                       const std::string& author, bool selected) {
  const Rect band = choiceBandBounds(renderer, metrics, indexInPage);
  const int lineHeight = renderer.getLineHeight(kCaptionFont);
  // One plate for title and author: the selection must say both what the book is and
  // who wrote it, and a two-line block still refreshes fast.
  renderer.fillRect(band.x, band.y, band.width, band.height, selected);
  renderer.drawText(kCaptionFont, band.x + 1, band.y + 1, title.c_str(), !selected);
  if (!author.empty()) {
    renderer.drawText(kCaptionFont, band.x + 1, band.y + 1 + lineHeight, author.c_str(), !selected);
  }
}

void drawCoverPlaceholderBox(GfxRenderer& renderer, const Rect& box, const std::string& title, const char* hint) {
  renderer.fillRect(box.x, box.y, box.width, box.height, false);
  CellView cell;
  cell.title = title;
  // Same widget as an empty shelf cell: framed box, corner ticks, title centred
  // inside. A card without artwork used to be a blank hole.
  drawPlaceholderCover(renderer, cell, hint, box.x, box.y, box.width, box.height);
}

void drawShelfCell(GfxRenderer& renderer, const GridMetrics& metrics, const CellView& cell, int indexInPage,
                   int selected, const char* noCoverText) {
  const int column = indexInPage % metrics.columns;
  const int row = indexInPage / metrics.columns;
  const int x = metrics.area.x + column * (metrics.cellW + metrics.gapX);
  const int y = metrics.area.y + row * (metrics.cellH + metrics.captionH + metrics.gapY);

  drawCover(renderer, cell, noCoverText, x, y, metrics.cellW, metrics.cellH);
  drawFormatChip(renderer, cell.format.c_str(), x, y, metrics.cellW);
  // Progress sits on the artwork's bottom edge: a caption-row bar read as a
  // stray line between grid rows.
  drawProgress(renderer, x, y + metrics.cellH - 2, metrics.cellW, cell.progressPercent);

  // The caption block (title + author) is the focus indicator: an inverted plate when
  // selected. A small window the panel can refresh almost instantly, unlike a frame
  // around the cell — and it never strikes through the caption text.
  drawCellTitleBand(renderer, metrics, indexInPage, fitCellTitle(renderer, metrics, cell.title),
                    fitCellAuthor(renderer, metrics, cell.author), indexInPage == selected);
}

void drawShelfPage(GfxRenderer& renderer, const GridMetrics& metrics, const PageView& page) {
  const GridArea& area = metrics.area;

  // Page counter only: the header above it belongs to the theme.
  char counter[24];
  std::snprintf(counter, sizeof(counter), "%d/%d", page.page + 1, std::max(1, page.pageCount));
  const int counterWidth = renderer.getTextWidth(kCaptionFont, counter);
  renderer.drawText(kCaptionFont, area.x + area.width - counterWidth, area.y, counter);

  if (page.count == 0 || page.cells == nullptr) {
    const int textWidth = renderer.getTextWidth(kCaptionFont, page.emptyText);
    renderer.drawText(kCaptionFont, area.x + (area.width - textWidth) / 2, area.y + area.height / 2, page.emptyText);
    return;
  }
  for (size_t index = 0; index < page.count; ++index) {
    drawShelfCell(renderer, metrics, page.cells[index], static_cast<int>(index), page.selected, page.noCoverText);
  }
}

OverlayGeometry drawOverlayList(GfxRenderer& renderer, const std::vector<std::string>& items, int selected) {
  const int itemCount = static_cast<int>(items.size());
  if (itemCount == 0) return OverlayGeometry{};
  const int lineHeight = renderer.getLineHeight(kCaptionFont) + 12;
  // Long metadata values (author/series strings) must not push the box past the
  // panel: the box grows with the label but never wider than the safe area.
  const int maxWidth = renderer.getScreenWidth() - 40;
  // Filter value lists can be longer than the panel (every author in the
  // library), so this list scrolls exactly like the icon list does.
  const int maxRows = std::max(1, (renderer.getScreenHeight() - 80) / lineHeight);
  const int visibleRows = std::min(itemCount, maxRows);
  const int firstRow = std::clamp(selected - visibleRows + 1, 0, itemCount - visibleRows);
  int boxWidth = 220;
  for (const auto& item : items) {
    boxWidth = std::max(boxWidth, renderer.getTextWidth(kCaptionFont, item.c_str()) + 40);
  }
  boxWidth = std::min(boxWidth, maxWidth);
  const int boxHeight = visibleRows * lineHeight + 20;
  const int x = (renderer.getScreenWidth() - boxWidth) / 2;
  const int y = (renderer.getScreenHeight() - boxHeight) / 2;

  renderer.fillRect(x, y, boxWidth, boxHeight, false);
  renderer.drawRect(x, y, boxWidth, boxHeight, 2, true);
  for (int row = 0; row < visibleRows; ++row) {
    const int i = firstRow + row;
    const int itemY = y + 10 + row * lineHeight;
    if (i == selected) {
      renderer.fillRect(x + 2, itemY - 4, boxWidth - 4, lineHeight, true);
      renderer.drawText(kCaptionFont, x + 12, itemY, items[i].c_str(), false);
    } else {
      renderer.drawText(kCaptionFont, x + 12, itemY, items[i].c_str(), true);
    }
  }
  return OverlayGeometry{Rect{x, y, boxWidth, boxHeight}, lineHeight, firstRow, visibleRows};
}

OverlayGeometry drawOverlayIconList(GfxRenderer& renderer, const std::vector<std::string>& items,
                                    const std::vector<const uint8_t*>& icons, int selected) {
  const int itemCount = static_cast<int>(items.size());
  if (itemCount == 0) return OverlayGeometry{};
  constexpr int kIconSize = 32;
  constexpr int kIconGap = 10;
  const int textHeight = renderer.getLineHeight(kCaptionFont);
  const int rowHeight = std::max(textHeight + 12, kIconSize + 8);
  const int maxWidth = renderer.getScreenWidth() - 40;
  // A filter value list can hold every author in the library, so the pop-up is a
  // window over the items, not the whole list: it never grows past the panel and
  // the selection is always scrolled into view.
  const int maxRows = std::max(1, (renderer.getScreenHeight() - 80) / rowHeight);
  const int visibleRows = std::min(itemCount, maxRows);
  const int firstRow = std::clamp(selected - visibleRows + 1, 0, itemCount - visibleRows);
  int labelWidth = 220;
  for (const auto& item : items) {
    labelWidth = std::max(labelWidth, renderer.getTextWidth(kCaptionFont, item.c_str()));
  }
  const int boxWidth = std::min(labelWidth + kIconSize + kIconGap + 40, maxWidth);
  const int boxHeight = visibleRows * rowHeight + 20;
  const int boxX = (renderer.getScreenWidth() - boxWidth) / 2;
  const int boxY = (renderer.getScreenHeight() - boxHeight) / 2;

  renderer.fillRect(boxX, boxY, boxWidth, boxHeight, false);
  renderer.drawRect(boxX, boxY, boxWidth, boxHeight, 2, true);
  for (int row = 0; row < visibleRows; ++row) {
    const int i = firstRow + row;
    const int rowY = boxY + 10 + row * rowHeight;
    if (i == selected) {
      renderer.drawRect(boxX + 3, rowY, boxWidth - 6, rowHeight, 2, true);
    }
    if (i < static_cast<int>(icons.size()) && icons[i] != nullptr) {
      renderer.drawImage(icons[i], boxX + 12, rowY + (rowHeight - kIconSize) / 2, kIconSize, kIconSize);
    }
    const int labelY = rowY + (rowHeight - textHeight) / 2;
    // Long labels are cut, not wrapped: a menu row stays one line.
    const std::string label = fitText(renderer, kCaptionFont, items[i], boxWidth - kIconSize - kIconGap - 30);
    renderer.drawText(kCaptionFont, boxX + 12 + kIconSize + kIconGap, labelY, label.c_str());
  }
  return OverlayGeometry{Rect{boxX, boxY, boxWidth, boxHeight}, rowHeight, firstRow, visibleRows};
}

Rect overlayRowBounds(const OverlayGeometry& geometry, int itemIndex) {
  const int row = itemIndex - geometry.firstRow;
  if (row < 0 || row >= geometry.visibleRows || geometry.rowHeight <= 0) return Rect{0, 0, 0, 0};
  // A little taller than the row: the list focus is a filled band that starts
  // above the text baseline, the icon list frames the whole row.
  const int top = geometry.box.y + 10 + row * geometry.rowHeight - 6;
  const int height = geometry.rowHeight + 8;
  return Rect{geometry.box.x, top, geometry.box.width, height};
}

}  // namespace shelfui
