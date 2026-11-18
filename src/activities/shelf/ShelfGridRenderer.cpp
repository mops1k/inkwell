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
constexpr int kCoverInset = 1;

// Truncates to fit `maxWidth`, appending an ellipsis only when the text really
// does not fit — avoids a dotted stub for a title that barely overflows.
std::string fitText(GfxRenderer& renderer, int fontId, const std::string& text, int maxWidth) {
  if (text.empty()) return {};
  if (renderer.getTextWidth(fontId, text.c_str()) <= maxWidth) return text;
  return renderer.truncatedText(fontId, text.c_str(), maxWidth);
}

void drawPlaceholderCover(GfxRenderer& renderer, int x, int y, int width, int height) {
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
}

void drawCover(GfxRenderer& renderer, const CellView& cell, int x, int y, int width, int height) {
  // White interior first: the selection frame behind a cell must not show
  // through the gaps of the artwork.
  renderer.fillRect(x, y, width, height, false);
  if (cell.coverPath.empty()) {
    drawPlaceholderCover(renderer, x, y, width, height);
    return;
  }

  FsFile file;
  if (!Storage.openFileForRead("Shelf", cell.coverPath, file)) {
    drawPlaceholderCover(renderer, x, y, width, height);
    return;
  }
  Bitmap bmp(file);
  const bool usable =
      bmp.parseHeaders() == BmpReaderError::Ok && bmp.isComplete() && bmp.getWidth() > 0 && bmp.getHeight() > 0;
  if (usable) {
    renderer.drawBitmap(bmp, x + kCoverInset, y + kCoverInset, width - kCoverInset * 2, height - kCoverInset * 2);
  }
  file.close();
  if (!usable) drawPlaceholderCover(renderer, x, y, width, height);
  renderer.drawRect(x, y, width, height, true);
}

void drawProgress(GfxRenderer& renderer, int x, int y, int width, int progressPercent) {
  if (progressPercent < 0) return;
  const int clamped = std::clamp(progressPercent, 0, 100);
  const int lineHeight = renderer.getLineHeight(kCaptionFont);
  const int barHeight = lineHeight + 6;
  const int top = y - barHeight;

  // Track outline plus filled portion, with the percentage centred inside the
  // bar. The number flips to white where it sits on the black fill, so it stays
  // readable on both sides of the boundary.
  renderer.drawRect(x, top, width, barHeight, true);
  const int filled = width * clamped / 100;
  if (filled > 0) renderer.fillRect(x, top, filled, barHeight);

  char label[8];
  std::snprintf(label, sizeof(label), "%d%%", clamped);
  const int textWidth = renderer.getTextWidth(kCaptionFont, label);
  const int textX = x + (width - textWidth) / 2;
  const int textY = top + (barHeight - lineHeight) / 2;
  const bool overFill = textX + textWidth / 2 < x + filled;
  renderer.drawText(kCaptionFont, textX, textY, label, !overFill);
}

}  // namespace

GridMetrics gridMetrics(int screenWidth, int screenHeight) {
  GridMetrics metrics;
  const shelf::CoverBox cell = shelf::coverBox(shelf::ShelfCoverSize::Cell);
  metrics.cellW = cell.width;
  metrics.cellH = cell.height;

  // Two cells and one gap must fit between the margins; on a narrower screen the
  // gap shrinks before anything overflows.
  constexpr int kMinGapX = 8;
  const int usable = screenWidth - metrics.marginX * 2;
  metrics.gapX = std::max(kMinGapX, (usable - metrics.columns * metrics.cellW) / (metrics.columns - 1));

  const int rowHeight = metrics.cellH + metrics.captionH + metrics.gapY;
  const int available = screenHeight - metrics.gridTop - 44;  // footer band
  metrics.rows = std::max(1, (available + metrics.gapY) / rowHeight);
  return metrics;
}

int pageCapacity(const GridMetrics& metrics) { return std::max(1, metrics.columns * metrics.rows); }

void drawShelfPage(GfxRenderer& renderer, const GridMetrics& metrics, const PageView& page) {
  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();

  renderer.drawText(kHeaderFont, metrics.marginX, metrics.headerTop, page.title);

  char counter[24];
  std::snprintf(counter, sizeof(counter), "%d/%d", page.page + 1, std::max(1, page.pageCount));
  const int counterWidth = renderer.getTextWidth(kCaptionFont, counter);
  renderer.drawText(kCaptionFont, screenWidth - metrics.marginX - counterWidth, metrics.headerTop + 4, counter);

  if (page.count == 0 || page.cells == nullptr) {
    const int textWidth = renderer.getTextWidth(kCaptionFont, page.emptyText);
    renderer.drawText(kCaptionFont, (screenWidth - textWidth) / 2, screenHeight / 2, page.emptyText);
  } else {
    for (size_t index = 0; index < page.count; ++index) {
      const int column = static_cast<int>(index) % metrics.columns;
      const int row = static_cast<int>(index) / metrics.columns;
      const int x = metrics.marginX + column * (metrics.cellW + metrics.gapX);
      const int y = metrics.gridTop + row * (metrics.cellH + metrics.captionH + metrics.gapY);
      const CellView& cell = page.cells[index];

      if (static_cast<int>(index) == page.selected) {
        renderer.drawRect(x - kSelectionInset, y - kSelectionInset, metrics.cellW + kSelectionInset * 2,
                          metrics.cellH + metrics.captionH + kSelectionInset * 2, 2, true);
      }

      drawCover(renderer, cell, x, y, metrics.cellW, metrics.cellH);
      // Progress sits on the artwork's bottom edge: a caption-row bar read as a
      // stray line between grid rows.
      drawProgress(renderer, x, y + metrics.cellH - 2, metrics.cellW, cell.progressPercent);

      const int captionY = y + metrics.cellH + 4;
      const int lineHeight = renderer.getLineHeight(kCaptionFont);
      const std::string title = fitText(renderer, kCaptionFont, cell.title, metrics.cellW);
      renderer.drawText(kCaptionFont, x, captionY, title.c_str());
      if (!cell.author.empty()) {
        const std::string author = fitText(renderer, kCaptionFont, cell.author, metrics.cellW);
        renderer.drawText(kCaptionFont, x, captionY + lineHeight, author.c_str());
      }
    }
  }

  const int footerY = screenHeight - 26;
  renderer.drawLine(metrics.marginX, footerY - 8, screenWidth - metrics.marginX, footerY - 8, true);
  renderer.drawText(kCaptionFont, metrics.marginX, footerY, page.hint);
}

}  // namespace shelfui
