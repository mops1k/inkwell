// Host stand-in for src/components/UITheme.cpp.
//
// UITheme.cpp is the firmware's theme registry plus a set of helpers that read
// device state: the NVS/JSON settings store, per-book progress caches, the
// reading-stats ledger and the three extra theme variants. Linking it on the
// host would drag the whole persistence layer into the harness, so the
// simulator supplies the very same API here — the class declaration still comes
// from the REAL src/components/UITheme.h, so a signature drift breaks the host
// build instead of silently diverging.
//
// What stays real: BaseTheme/LyraTheme themselves (compiled from
// src/components/themes verbatim), their metrics (BaseMetrics/LyraMetrics) and
// every drawing call they make. What is host-simplified below is only the
// storage/book-cache side, which the rendered frames never touch (the shelf
// takes its progress from the caller's PageView, not from progress.bin).
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <memory>
#include <string>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "util/BookArchiveUtils.h"

// CrossPointSettings.cpp also carries the JSON/NVS persistence layer; the
// simulator only reads the header-declared defaults, so the singleton is
// defined here (the class' member initializers still apply — SETTINGS behaves
// like a fresh device whose settings file is absent).
CrossPointSettings CrossPointSettings::instance;

namespace {
constexpr int STATUS_BAR_ITEM_PADDING = 4;
constexpr int STATUS_BAR_DESCENDER_CLEARANCE = 4;

uint8_t normalizeProgressBar(const uint8_t progressBar) {
  return progressBar < CrossPointSettings::STATUS_BAR_PROGRESS_BAR_COUNT ? progressBar
                                                                         : CrossPointSettings::HIDE_PROGRESS;
}

uint8_t normalizeProgressBarThickness(const uint8_t thickness) {
  return thickness < CrossPointSettings::STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT
             ? thickness
             : CrossPointSettings::PROGRESS_BAR_NORMAL;
}

uint8_t normalizeStatusBarItemsPosition(const uint8_t position) {
  return position < CrossPointSettings::STATUS_BAR_ITEMS_POSITION_COUNT ? position
                                                                        : CrossPointSettings::STATUS_BAR_ITEMS_BOTTOM;
}
}  // namespace

UITheme UITheme::instance;

UITheme::UITheme() { setTheme(static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme)); }

void UITheme::reload() { setTheme(static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme)); }

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  switch (type) {
    case CrossPointSettings::UI_THEME::LYRA:
      currentTheme = std::make_unique<LyraTheme>();
      currentMetrics = &LyraMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::CLASSIC:
    default:
      // LYRA_3_COVERS / LYRA_CAROUSEL / MINIMAL are not compiled into the
      // harness (they only differ in the home screen, not in the chrome the
      // simulator renders), so they fall back to the Classic implementation.
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &BaseMetrics::values;
      break;
  }
  if (SETTINGS.uiTextSize != 0) {
    enlargedMetrics = *currentMetrics;
    enlargedMetrics.listRowHeight = std::max(enlargedMetrics.listRowHeight, 40);
    enlargedMetrics.listWithSubtitleRowHeight = std::max(enlargedMetrics.listWithSubtitleRowHeight, 100);
    enlargedMetrics.statusBarVerticalMargin = std::max(enlargedMetrics.statusBarVerticalMargin, 25);
    currentMetrics = &enlargedMetrics;
  }
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = getContentRect(renderer, hasButtonHints, /*hasSideHints=*/false);
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight;
  }
  if (hasButtonHints) {
    reservedHeight += metrics.verticalSpacing;
  }
  const int availableHeight = contentRect.height - reservedHeight;
  const int rowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  return availableHeight / rowHeight;
}

Rect UITheme::getContentRect(const GfxRenderer& renderer, bool hasBottomHints, bool hasSideHints) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const int bh = hasBottomHints ? metrics.buttonHintsHeight : 0;
  const int sw = hasSideHints ? metrics.sideButtonHintsWidth : 0;

  int top = 0, right = 0, bottom = 0, left = 0;
  switch (renderer.getOrientation()) {
    case GfxRenderer::Portrait:
      bottom = bh;
      if (gpio.deviceIsX3() && hasSideHints) {
        left = sw;
        right = sw;
      } else {
        right = sw;
      }
      break;
    case GfxRenderer::PortraitInverted:
      top = bh;
      if (gpio.deviceIsX3() && hasSideHints) {
        left = sw;
        right = sw;
      } else {
        left = sw;
      }
      break;
    case GfxRenderer::LandscapeClockwise:
      left = bh;
      if (gpio.deviceIsX3() && hasSideHints) {
        top = sw;
        bottom = sw;
      } else {
        bottom = sw;
      }
      break;
    case GfxRenderer::LandscapeCounterClockwise:
      right = bh;
      if (gpio.deviceIsX3() && hasSideHints) {
        top = sw;
        bottom = sw;
      } else {
        top = sw;
      }
      break;
  }

  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  return Rect{left, top, w - left - right, h - top - bottom};
}

std::string UITheme::makeSeparatorTitle(const std::string& title) { return std::string("__") + title; }

std::string UITheme::makeSeparatorTitle(StrId labelId) { return std::string("__") + I18N.get(labelId); }

bool UITheme::isSeparatorTitle(const std::string& title) { return title.rfind("__", 0) == 0; }

std::string UITheme::stripSeparatorTitle(const std::string& title) {
  return isSeparatorTitle(title) ? title.substr(2) : title;
}

std::function<bool(int)> UITheme::makeSelectablePredicate(int total, std::function<std::string(int)> titleGetter) {
  return
      [total, titleGetter](int index) { return index >= 0 && index < total && !isSeparatorTitle(titleGetter(index)); };
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  const size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int width, int height) {
  const size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(width) + "x" + std::to_string(height));
  }
  return coverBmpPath;
}

// Book progress lives in per-book cache directories (progress.bin) that the
// harness does not have: the shelf receives its percentages in PageView.
int UITheme::getBookProgressPercent(const RecentBook&) { return -1; }

bool UITheme::getBookProgressPages(const RecentBook&, int& currentPage, int& totalPages) {
  currentPage = 0;
  totalPages = 0;
  return false;
}

// The reading-pace suffix ("~45m") comes from the reading-stats ledger, which
// the host does not populate; the percentage itself is all a frame can show.
std::string UITheme::formatBookProgressStatus(const RecentBook&, int progressPercent) {
  return progressPercent < 0 ? std::string() : std::to_string(progressPercent) + "%";
}

void UITheme::drawCoverProgressIndicator(const GfxRenderer&, Rect, int) {}

void UITheme::drawCoverProgressBadge(const GfxRenderer&, Rect, const RecentBook&, int) {}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.empty()) return File;
  if (filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename) || isFb2OrZipBookPath(filename) || FsHelpers::hasXtcExtension(filename)) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasBmpExtension(filename) || FsHelpers::hasJpgExtension(filename) ||
      FsHelpers::hasPngExtension(filename)) {
    return Image;
  }
  return File;
}

int UITheme::getProgressBarHeight(const uint8_t progressBar, const uint8_t thickness) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  if (normalizeProgressBar(progressBar) == CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS) {
    return 0;
  }
  const uint8_t normalizedThickness = normalizeProgressBarThickness(thickness);
  return ((normalizedThickness + 1) * 2) + metrics.progressBarMarginTop;
}

int UITheme::getStatusBarItemsHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  return std::max(metrics.statusBarVerticalMargin + (STATUS_BAR_ITEM_PADDING * 2) + STATUS_BAR_DESCENDER_CLEARANCE,
                  metrics.batteryHeight + (STATUS_BAR_ITEM_PADDING * 2) + STATUS_BAR_DESCENDER_CLEARANCE);
}

int UITheme::getStatusBarTopHeight(const bool forceStatusItems) {
  const bool showStatusItems = forceStatusItems || SETTINGS.statusBarChapterPageCount ||
                               SETTINGS.statusBarBookProgressPercentage ||
                               SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE ||
                               SETTINGS.statusBarBattery || (SETTINGS.useClock && SETTINGS.statusBarClock);
  const bool statusItemsAtTop = normalizeStatusBarItemsPosition(SETTINGS.statusBarItemsPosition) ==
                                CrossPointSettings::STATUS_BAR_ITEMS_POSITION::STATUS_BAR_ITEMS_TOP;
  const int statusItemsHeight = showStatusItems && statusItemsAtTop ? getStatusBarItemsHeight() : 0;
  return getProgressBarHeight(SETTINGS.statusBarUpperProgressBar, SETTINGS.statusBarUpperProgressBarThickness) +
         statusItemsHeight;
}

int UITheme::getStatusBarBottomHeight(const bool forceStatusItems) {
  const bool showStatusItems = forceStatusItems || SETTINGS.statusBarChapterPageCount ||
                               SETTINGS.statusBarBookProgressPercentage ||
                               SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE ||
                               SETTINGS.statusBarBattery || (SETTINGS.useClock && SETTINGS.statusBarClock);
  const bool statusItemsAtBottom = normalizeStatusBarItemsPosition(SETTINGS.statusBarItemsPosition) ==
                                   CrossPointSettings::STATUS_BAR_ITEMS_POSITION::STATUS_BAR_ITEMS_BOTTOM;
  const int statusItemsHeight = showStatusItems && statusItemsAtBottom ? getStatusBarItemsHeight() : 0;
  return getProgressBarHeight(SETTINGS.statusBarLowerProgressBar, SETTINGS.statusBarLowerProgressBarThickness) +
         statusItemsHeight;
}

int UITheme::getStatusBarHeight(const bool forceStatusItems) {
  return getStatusBarTopHeight(forceStatusItems) + getStatusBarBottomHeight(forceStatusItems);
}
