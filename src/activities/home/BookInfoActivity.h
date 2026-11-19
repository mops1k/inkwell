#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "../Activity.h"
#include "ReadingStats.h"

// Everything the library already knows about the book. Passing it in lets the
// card paint on the first frame; the full parse (description, exact metadata)
// then runs on the next loop tick instead of holding the screen on "loading".
// File-scope, not nested: a nested aggregate with a default member initialiser
// cannot be used as a default argument inside the enclosing class (C++11).
struct BookInfoSeed {
  std::string title;
  std::string author;
  std::string series;
  std::string seriesIndex;
  std::string genre;
  // Publication data the library index already knows, so the first paint has it
  // too and the card does not flash empty rows while the file is parsed.
  std::string year;
  std::string publisher;
  std::string pages;
  std::string coverPath;
  int progressPercent = -1;
  BookInfoSeed() = default;
};

class BookInfoActivity final : public Activity {
 private:
  // findBook() below needs the history; hold it for this screen only.
  ReadingStatsStore::ScopedLoad statsLoad_;

  const std::string filePath;

  // Metadata populated in onEnter
  std::string title;
  std::string author;
  std::string series;
  std::string seriesIndex;
  std::string genre;  // first FB2 <genre> / EPUB dc:subject, may stay empty
  // Publication data as the container declares it: FB2 <publish-info>, EPUB
  // dc:date/dc:publisher plus a page count only when the book states one. Each
  // row is drawn only when its string is not empty.
  std::string year;
  std::string publisher;
  std::string pages;
  std::string description;
  std::string coverBmpPath;
  std::string loadError;
  bool loadSucceeded = false;
  size_t fileSizeBytes = 0;
  // Whether "Read" from this card returns to the shelf (it was opened there)
  // or to Home (opened from the home carousel).
  bool returnToShelf_ = false;

  // Reading stats snapshot, populated in loadData() when this book has history.
  bool hasReadingStats = false;
  uint32_t statTotalSeconds = 0;
  uint8_t statProgress = 0;
  time_t statLastReadEpoch = 0;
  // From the library's own metadata: shown even before the parse finishes, and
  // the only source of series for formats whose card metadata lacks it.
  BookInfoSeed seed_;

  // Description paging (populated lazily on first render)
  std::vector<std::string> descLines;
  int descWrappedWidth = 0;
  int descPage = 0;
  int descLinesPerPage = 0;
  int descTotalPages = 0;

  // Series neighbours, in reading order (see shelf::collectSeriesBooks). The
  // card stays a card: Left/Right walk the series instead of leaving the book.
  struct SeriesBook {
    std::string path;
    std::string title;
    std::string author;
    std::string series;
    std::string seriesIndex;
    std::string genre;
  };
  std::vector<SeriesBook> seriesBooks_;
  int seriesPosition_ = -1;
  bool hasSeriesSiblings() const { return seriesBooks_.size() > 1 && seriesPosition_ >= 0; }
  // Rebuilds the sibling list from the library index; called on entry (so the hints
  // are right on the first frame) and after a parse that learned the series name.
  void rebuildSeriesSiblings();
  void walkSeries(int delta);

  // Partial-render caches: set on the first full render, reused when only the
  // description page changes.
  bool fullRenderDone = false;
  int descBandX = 0;
  int descBandY = 0;
  int descBandWidth = 0;
  int descBandHeight = 0;
  int hintsBandY = 0;
  int hintsBandHeight = 0;
  int partialRenderCount = 0;
  static constexpr int MAX_PARTIAL_RENDERS = 10;

  static std::string formatFileSize(size_t bytes);
  void renderLoading();
  void loadData();
  void fillSeedFromIndex();
  void applySeed();
  bool hasSeed_ = false;
  bool parsePending_ = false;
  bool coverTried_ = false;
  void renderDescriptionAndHints();
  bool supportsCustomCover() const;
  void openReader();
  void openCoverMenu();
  void openCoverPicker();
  void refreshAfterCoverChange();

 public:
  explicit BookInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath,
                            bool returnToShelf = false, BookInfoSeed seed = BookInfoSeed())
      : Activity("BookInfo", renderer, mappedInput),
        filePath(std::move(filePath)),
        returnToShelf_(returnToShelf),
        seed_(std::move(seed)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
