#include "BookInfoActivity.h"

#include <Bitmap.h>
#include <BookFormat.h>
#include <BookMetadata.h>
#include <CoverPolicy.h>
#include <Epub.h>
#include <Fb2.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <KOReaderDocumentId.h>
#include <LibraryQuery.h>
#include <Logging.h>
#include <ShelfIndexStore.h>
#include <Xtc.h>

#include <cstdio>
#include <ctime>
#include <memory>

#include "../ActivityManager.h"
#include "BookCoverMenuActivity.h"
#include "ReadingStats.h"
#include "RecentBooksStore.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/shelf/ShelfGridRenderer.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include <BookFormat.h>

namespace {

// The reader's own FB2 conversion leaves the book's <annotation> in the cache as a
// small text file. Reading that is instant, while the archive scan below re-inflates
// the whole book (minutes on the emulated card) — so the card gets its description
// from here first, and the scan only fills whatever is still missing.
std::string readCachedAnnotation(const std::string& bookPath) {
  const std::string dir = ReaderActivity::bookCacheDir(bookPath);
  if (dir.empty()) return {};
  HalFile file;
  if (!Storage.openFileForRead("SHELF", dir + "/fb2_annotation.txt", file)) return {};
  std::string text;
  char buffer[256];
  int read = 0;
  while ((read = file.read(buffer, sizeof(buffer))) > 0) {
    text.append(buffer, static_cast<size_t>(read));
    if (text.size() > 32 * 1024) break;  // a card shows a few pages, not the whole essay
  }
  file.close();
  return text;
}

}  // namespace
#include "util/BookArchiveUtils.h"
#include "util/BookCoverUtils.h"

namespace {

std::string formatReadingDuration(uint32_t totalSeconds) {
  const uint32_t h = totalSeconds / 3600;
  const uint32_t m = (totalSeconds % 3600) / 60;
  char buf[24];
  if (h > 0) {
    snprintf(buf, sizeof(buf), "%uh %02um", h, m);
  } else {
    snprintf(buf, sizeof(buf), "%um", m);
  }
  return buf;
}

// "N days ago" / "Nh ago" style, or an absolute date past a month. Empty when
// the epoch is unknown or the clock isn't synced (caller then skips the row).
std::string formatLastRead(time_t epoch) {
  if (epoch == 0 || !HalClock::isSynced()) {
    return {};
  }
  const time_t now = HalClock::now();
  char buf[24];
  if (now <= epoch) {
    return "just now";
  }
  const uint32_t delta = static_cast<uint32_t>(now - epoch);
  if (delta < 60) {
    return "just now";
  }
  if (delta < 3600) {
    snprintf(buf, sizeof(buf), "%um ago", delta / 60);
    return buf;
  }
  if (delta < 86400) {
    snprintf(buf, sizeof(buf), "%uh ago", delta / 3600);
    return buf;
  }
  const uint32_t days = delta / 86400;
  if (days < 30) {
    snprintf(buf, sizeof(buf), "%ud ago", days);
    return buf;
  }
  struct tm t{};
  localtime_r(&epoch, &t);
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  return buf;
}

}  // namespace

std::string BookInfoActivity::formatFileSize(const size_t bytes) {
  char buf[16];
  if (bytes < 1024) {
    snprintf(buf, sizeof(buf), "%u B", static_cast<unsigned>(bytes));
  } else if (bytes < 1024 * 1024) {
    snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0f);
  } else {
    snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0f * 1024.0f));
  }
  return buf;
}

void BookInfoActivity::renderLoading() {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  const std::string loadingHeaderSource = !seed_.title.empty() ? seed_.title : std::string();
  const std::string loadingHeader = loadingHeaderSource.empty()
                                        ? std::string(tr(STR_INFO))
                                        : renderer.truncatedText(UI_12_FONT_ID, loadingHeaderSource.c_str(),
                                                                 contentRect.width - metrics.contentSidePadding * 2);
  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 loadingHeader.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  renderer.drawText(UI_12_FONT_ID, contentRect.x + metrics.contentSidePadding, contentTop, tr(STR_LOADING));

  renderer.displayBuffer();
}

void BookInfoActivity::loadData() {
  loadSucceeded = false;
  loadError.clear();
  // The seed is the baseline, not something to wipe: the library index already knows
  // title/author/series/genre, so a parse that returns nothing (or fails on a book
  // that is being unpacked by the cover worker) must not turn the card into a bare
  // title with an empty cover frame.
  title = seed_.title;
  author = seed_.author;
  series = seed_.series;
  seriesIndex = seed_.seriesIndex;
  genre = seed_.genre;
  year = seed_.year;
  publisher = seed_.publisher;
  pages = seed_.pages;
  description.clear();
  coverBmpPath = seed_.coverPath;

  // Get file size
  bool fileOpened = false;
  HalFile f = Storage.open(filePath.c_str());
  if (f) {
    fileOpened = true;
    fileSizeBytes = f.fileSize();
    f.close();
  }

  // Load epub metadata — builds cache if missing, which also gives us cover
  if (FsHelpers::hasEpubExtension(filePath) || isFb2OrZipBookPath(filePath)) {
    // FB2/FB2.ZIP: the native scanner reads title, series, genre and <annotation>
    // directly, without building the reader's EPUB-style cache first. The card is
    // a metadata screen; paying for a full conversion here made it take seconds
    // and even time out on big archives.
    bool handledByScanner = false;
    const shelf::BookFormat shelfFormat = shelf::detectBookFormat(filePath);
    if (shelfFormat == shelf::BookFormat::Fb2 || shelfFormat == shelf::BookFormat::Fb2Zip) {
      // Fast path: the reader already converted this book once, so the annotation is
      // sitting in its cache and the index supplied title/author/series/genre. Re-
      // inflating the archive just to read the same text is what made the card show
      // no description at all while the scan crawled (minutes on the emulated card).
      if (description.empty()) description = readCachedAnnotation(filePath);
      const bool seeded = !seed_.title.empty();
      if (seeded && !description.empty()) {
        coverBmpPath = seed_.coverPath;
        loadSucceeded = true;
        handledByScanner = true;
      }
    }
    if (shelfFormat == shelf::BookFormat::Fb2 || shelfFormat == shelf::BookFormat::Fb2Zip) {
      shelf::BookMeta scanned;
      if (handledByScanner) {
        // Metadata came from the index + cache; nothing to scan.
      } else if (shelf::readBookMetadata(filePath, shelfFormat, scanned)) {
        if (!scanned.title.empty()) title = scanned.title;
        if (!scanned.author.empty()) author = scanned.author;
        if (!scanned.series.empty()) series = scanned.series;
        if (!scanned.seriesIndex.empty()) seriesIndex = scanned.seriesIndex;
        if (!scanned.genre.empty()) genre = scanned.genre;
        if (!scanned.description.empty()) description = scanned.description;
        if (!scanned.year.empty()) year = scanned.year;
        if (!scanned.publisher.empty()) publisher = scanned.publisher;
        if (!scanned.pages.empty()) pages = scanned.pages;
        loadSucceeded = true;
        handledByScanner = true;
      } else {
        // A failed scan is not a broken card: the index already gave us title,
        // author, series and genre, so show those and only lose the description.
        LOG_ERR("BookInfo", "FB2 metadata scan failed: %s", filePath.c_str());
        loadSucceeded = !title.empty() || !coverBmpPath.empty();
        if (!loadSucceeded) loadError = tr(STR_LOAD_EPUB_FAILED);
        handledByScanner = true;
      }
    }
    if (handledByScanner) {
      // The shared tail below still applies the seed fallbacks and reading stats.
    } else {
      std::unique_ptr<Fb2> fb2;
      std::string packagePath = filePath;
      if (isFb2BookPath(filePath) ||
          (isBookZipPath(filePath) && detectBookArchiveType(filePath) == BookArchiveType::Fb2)) {
        fb2 = std::make_unique<Fb2>(filePath, "/.crosspoint");
        if (!fb2->load()) {
          loadError = tr(STR_LOAD_EPUB_FAILED);
          LOG_ERR("BookInfo", "Failed to prepare FB2 metadata: %s", filePath.c_str());
          return;
        }
        packagePath = fb2->getPackagePath();
      }
      Epub epub(packagePath, "/.crosspoint");
      if (!epub.load(true, true)) {
        loadError = tr(STR_LOAD_EPUB_FAILED);
        LOG_ERR("BookInfo", "Failed to load EPUB metadata: %s", filePath.c_str());
        return;
      }

      title = epub.getTitle();
      author = epub.getAuthor();
      series = epub.getSeries();
      seriesIndex = epub.getSeriesIndex();
      description = epub.getDescription();
      genre = epub.getGenre();
      year = epub.getYear();
      publisher = epub.getPublisher();
      pages = epub.getPages();
      // FB2 packages are read through the EPUB wrapper, whose generated OPF does
      // not carry the source <genre> or the <annotation>, so fall back to the
      // shelf's own FB2 scanner rather than showing empty rows for every FB2 card.
      if (genre.empty() || description.empty()) {
        const shelf::BookFormat format = shelf::detectBookFormat(filePath);
        if (format == shelf::BookFormat::Fb2 || format == shelf::BookFormat::Fb2Zip) {
          shelf::BookMeta scanned;
          if (shelf::readBookMetadata(filePath, format, scanned)) {
            if (genre.empty()) genre = scanned.genre;
            if (description.empty()) description = scanned.description;
          }
        }
      }

      // No cover generation here: the card must paint and stay interactive. The
      // seeded path (grid thumb) is used when present; a missing one is produced
      // later, outside this parse.
      coverBmpPath = seed_.coverPath;
      loadSucceeded = true;
    }
  } else if (FsHelpers::hasXtcExtension(filePath)) {
    Xtc xtc(filePath, "/.crosspoint");
    if (!xtc.load()) {
      loadError = tr(STR_LOAD_XTC_FAILED);
      LOG_ERR("BookInfo", "Failed to load XTC metadata: %s", filePath.c_str());
      return;
    }
    title = xtc.getTitle();
    author = xtc.getAuthor();

    const auto& metrics = UITheme::getInstance().getMetrics();
    const std::string thumbPath = xtc.getThumbBmpPath(metrics.homeCoverHeight);
    if (!Storage.exists(thumbPath.c_str())) {
      xtc.generateThumbBmp(metrics.homeCoverHeight);
    }
    coverBmpPath = Storage.exists(thumbPath.c_str()) ? thumbPath : "";
    loadSucceeded = true;
  } else if (fileOpened) {
    // TXT/MD and friends have no metadata parser at all, and that is not an error:
    // the card shows what the library already knows (title, author, size, reading
    // history) with an empty description and no artwork frame.
    if (title.empty()) title = seed_.title;
    loadSucceeded = true;
  } else {
    loadError = tr(STR_ERROR_GENERAL_FAILURE);
    LOG_ERR("BookInfo", "Unsupported file type for book info: %s", filePath.c_str());
  }

  // Pull any recorded reading history for this book. Keyed by the same
  // filename-based document id the reader uses when recording sessions.
  if (loadSucceeded) {
    const std::string docId = KOReaderDocumentId::calculateFromFilename(filePath);
    if (const BookReadingStats* stats = READING_STATS.findBook(docId)) {
      hasReadingStats = stats->totalSeconds > 0 || stats->sessions > 0;
      statTotalSeconds = stats->totalSeconds;
      statProgress = stats->progress;
      statLastReadEpoch = stats->lastReadEpoch;
    }
  }

  // The library's index is authoritative for what the grid already showed: some
  // formats carry no series in their own metadata, so an empty parsed field must
  // not blank the seeded value.
  if (title.empty()) title = seed_.title;
  if (author.empty()) author = seed_.author;
  if (series.empty()) series = seed_.series;
  if (seriesIndex.empty()) seriesIndex = seed_.seriesIndex;
  if (genre.empty()) genre = seed_.genre;
  if (year.empty()) year = seed_.year;
  if (publisher.empty()) publisher = seed_.publisher;
  if (pages.empty()) pages = seed_.pages;
  if (coverBmpPath.empty()) coverBmpPath = seed_.coverPath;

  // Opened from the book menu ("Информация") there is no seed, and the FB2
  // package's generated OPF carries no series at all: take the library index as
  // the second source instead of showing an empty series row. The number is part
  // of that row too — the scanner knows the series but not always its position
  // when the seed is missing, so it is fetched even when the name came from the
  // file itself.
  if (series.empty() || genre.empty() || (seriesIndex.empty() && !series.empty())) {
    shelf::ShelfIndex index;
    shelf::ShelfIndexStore store;
    if (store.load(index)) {
      for (const auto& book : index.books) {
        if (book.path != filePath) continue;
        if (series.empty()) {
          series = book.meta.series;
          seriesIndex = book.meta.seriesIndex;
        }
        if (seriesIndex.empty() && !book.meta.series.empty() && book.meta.series == series) {
          seriesIndex = book.meta.seriesIndex;
        }
        if (genre.empty()) genre = book.meta.genre;
        break;
      }
    }
  }

  // A parse may have been the first source of the series name, so the siblings are
  // refreshed here as well as in onEnter().
  rebuildSeriesSiblings();
}

void BookInfoActivity::rebuildSeriesSiblings() {
  // Series siblings come from the library index (the card itself only parses one
  // file). Reading order and no wrap-around match the shelf's series filter.
  seriesBooks_.clear();
  seriesPosition_ = -1;
  if (series.empty()) return;
  shelf::ShelfIndex index;
  shelf::ShelfIndexStore store;
  if (!store.load(index)) return;
  std::vector<uint16_t> ids;
  shelf::collectSeriesBooks(index, series, ids);
  seriesBooks_.reserve(ids.size());
  for (const uint16_t id : ids) {
    const shelf::ShelfBook& sibling = index.books[id];
    SeriesBook entry;
    entry.path = sibling.path;
    entry.title = sibling.meta.title;
    entry.author = sibling.meta.author;
    entry.series = sibling.meta.series;
    entry.seriesIndex = sibling.meta.seriesIndex;
    entry.genre = sibling.meta.genre;
    if (entry.path == filePath) seriesPosition_ = static_cast<int>(seriesBooks_.size());
    seriesBooks_.push_back(std::move(entry));
  }
}

void BookInfoActivity::fillSeedFromIndex() {
  shelf::ShelfIndex index;
  shelf::ShelfIndexStore store;
  if (!store.load(index)) return;
  for (const auto& book : index.books) {
    if (book.path != filePath) continue;
    seed_.title = book.meta.title;
    seed_.author = book.meta.author;
    seed_.series = book.meta.series;
    seed_.seriesIndex = book.meta.seriesIndex;
    seed_.genre = book.meta.genre;
    // Artwork already produced for the card or the grid: reuse it for the first
    // frame, the card upgrades the small one after the parse.
    for (const auto size : {shelf::ShelfCoverSize::Card, shelf::ShelfCoverSize::Cell}) {
      const shelf::CoverBox box = shelf::coverBox(size);
      const std::string thumb = ReaderActivity::bookCacheDir(filePath) + "/" +
                                ReaderActivity::coverThumbFileName(filePath, box.width, box.height);
      if (Storage.exists(thumb.c_str())) {
        seed_.coverPath = thumb;
        break;
      }
    }
    hasSeed_ = !seed_.title.empty() || !seed_.coverPath.empty();
    return;
  }
}

void BookInfoActivity::walkSeries(int delta) {
  if (!hasSeriesSiblings()) return;
  // No wrap-around: a series is read in order, so the first book must not jump to the
  // last one (and the hints already hide the impossible direction).
  const int next = seriesPosition_ + delta;
  if (next < 0 || next >= static_cast<int>(seriesBooks_.size())) return;
  const SeriesBook& sibling = seriesBooks_[next];
  BookInfoSeed seed;
  seed.title = sibling.title;
  seed.author = sibling.author;
  seed.series = sibling.series;
  seed.seriesIndex = sibling.seriesIndex;
  seed.genre = sibling.genre;
  // Replace, not push: Back from the neighbour still lands on the library, not on
  // a stack of cards the user would have to walk out of one by one.
  activityManager.replaceActivity(
      std::make_unique<BookInfoActivity>(renderer, mappedInput, sibling.path, returnToShelf_, std::move(seed)));
}

bool BookInfoActivity::supportsCustomCover() const {
  if (FsHelpers::hasEpubExtension(filePath) || isFb2BookPath(filePath)) return true;
  if (!isBookZipPath(filePath)) return false;
  const BookArchiveType type = detectBookArchiveType(filePath);
  return type == BookArchiveType::Epub || type == BookArchiveType::Fb2;
}

void BookInfoActivity::refreshAfterCoverChange() {
  // Home's carousel keeps a pre-rendered cover strip in RAM even while another
  // activity is on top. The thumbnail files below are regenerated correctly,
  // but that frame would otherwise continue showing the previous cover until a
  // reboot or an unrelated cache invalidation.
  {
    RenderLock lock;
    UITheme::getInstance().getMutableTheme().invalidateFrameCache();
  }
  fullRenderDone = false;
  partialRenderCount = 0;
  descLines.clear();
  descWrappedWidth = 0;
  loadData();
  RECENT_BOOKS.updateBook(filePath, title, author, series, ReaderActivity::coverThumbPlaceholder(filePath));
  requestUpdate(true);
}

void BookInfoActivity::openCoverPicker() {
  const size_t slash = filePath.find_last_of('/');
  const std::string initialPath = slash == std::string::npos || slash == 0 ? "/" : filePath.substr(0, slash);
  startActivityForResult(std::make_unique<FileBrowserActivity>(renderer, mappedInput, initialPath, std::string{},
                                                               FileBrowserActivity::Mode::PickImage),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           const auto* selected = std::get_if<FilePathResult>(&result.data);
                           if (!selected || selected->path.empty()) return;
                           if (!BookCoverUtils::install(filePath, selected->path)) {
                             LOG_ERR("BookInfo", "Failed to install custom cover %s", selected->path.c_str());
                           }
                           refreshAfterCoverChange();
                         });
}

void BookInfoActivity::openReader() {
  ReturnHint hint;
  // Back from the reader returns to this card, and the card's own Back returns to the
  // library: the reader is entered from the card, so leaving it must walk the same path
  // in reverse instead of jumping straight to the grid.
  hint.target = returnToShelf_ ? ReturnTo::BookCard : ReturnTo::Home;
  hint.selectName = filePath;
  activityManager.replaceWithReader(filePath, std::move(hint));
}

void BookInfoActivity::openCoverMenu() {
  startActivityForResult(
      std::make_unique<BookCoverMenuActivity>(renderer, mappedInput, BookCoverUtils::hasCustomCover(filePath), false),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto* menu = std::get_if<MenuResult>(&result.data);
        if (!menu) return;
        switch (static_cast<BookCoverMenuActivity::Action>(menu->action)) {
          case BookCoverMenuActivity::Action::Read:
            openReader();
            return;
          case BookCoverMenuActivity::Action::Set:
            openCoverPicker();
            return;
          case BookCoverMenuActivity::Action::Reset:
            BookCoverUtils::reset(filePath);
            refreshAfterCoverChange();
            return;
        }
      });
}

void BookInfoActivity::applySeed() {
  if (seed_.title.empty()) {
    hasSeed_ = false;
    return;
  }
  hasSeed_ = true;
  title = seed_.title;
  author = seed_.author;
  series = seed_.series;
  genre = seed_.genre;
  coverBmpPath = seed_.coverPath;
  if (seed_.progressPercent >= 0) {
    hasReadingStats = true;
    statProgress = static_cast<uint8_t>(seed_.progressPercent);
  }
  // Walked-to cards get their hints from the sibling list, and that list must exist
  // before the first paint: without it the last book of a series showed no «Назад».
  rebuildSeriesSiblings();
  loadSucceeded = true;
}

void BookInfoActivity::onEnter() {
  Activity::onEnter();
  fullRenderDone = false;
  // Opened from the book menu there is no seed: take one from the library index
  // synchronously (a small JSON read) so the first frame already has title,
  // series and cached artwork instead of a "loading" screen followed by an
  // expensive FB2 cache build.
  if (!hasSeed_) fillSeedFromIndex();
  applySeed();
  if (hasSeed_) {
    // The card is already complete enough to show: paint it now and let the full
    // parse run on the first loop tick. Before, opening the card waited for an
    // EPUB/FB2 metadata load before drawing anything.
    parsePending_ = true;
    requestUpdate(true);
    return;
  }
  {
    RenderLock lock(*this);
    renderLoading();
  }
  loadData();
  requestUpdate(true);
}

void BookInfoActivity::loop() {
  // Input first, work second: pressing Back must leave the card even while its
  // metadata is still being parsed below. This is why the card used to feel stuck.
  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Back && ev.type == ButtonEventManager::PressType::Short) {
      if (returnToShelf_) {
        // Opened from the grid: Back returns to the library focused on this book.
        activityManager.goToShelf(filePath);
      } else {
        finish();
      }
      return;
    }
    if (ev.button == MappedInputManager::Button::Confirm && ev.type == ButtonEventManager::PressType::Short &&
        loadSucceeded) {
      openReader();
      return;
    }
    if (ev.button == MappedInputManager::Button::Confirm && ev.type == ButtonEventManager::PressType::Long &&
        loadSucceeded && supportsCustomCover()) {
      openCoverMenu();
      return;
    }
    if (ev.type != ButtonEventManager::PressType::Short) continue;
    // Front Left/Right: the series when this book has siblings, otherwise the
    // description pages. Up/Down always page the description, so both readings
    // stay reachable without leaving the card.
    if (ev.button == MappedInputManager::Button::Left) {
      if (hasSeriesSiblings()) {
        walkSeries(-1);
        return;
      }
      if (descPage > 0) {
        descPage--;
        requestUpdate(true);
      }
    } else if (ev.button == MappedInputManager::Button::Right) {
      if (hasSeriesSiblings()) {
        walkSeries(1);
        return;
      }
      if (descPage + 1 < descTotalPages) {
        descPage++;
        requestUpdate(true);
      }
    } else if (ev.button == MappedInputManager::Button::Up && descPage > 0) {
      descPage--;
      requestUpdate(true);
    } else if (ev.button == MappedInputManager::Button::Down && descPage + 1 < descTotalPages) {
      descPage++;
      requestUpdate(true);
    }
  }

  if (parsePending_) {
    parsePending_ = false;
    loadData();
    requestUpdate(true);
    return;
  }

  // Card artwork: one card-size thumbnail per book, written into the same book
  // cache as the cell thumbnail and reused from then on. A seeded cell-sized
  // thumb is only a stand-in for the first frame, so it is upgraded exactly once
  // after the parse — never regenerated on later visits.
  // Only formats that carry artwork may upgrade the card image: for TXT/Markdown
  // the same helper writes a thumbnail of the first *page*, which the card used
  // to draw as a cover — a dense text page scaled to the frame reads as a black
  // block. Text books keep the framed title placeholder instead.
  if (!coverTried_ && loadSucceeded && shelf::formatHasArtwork(shelf::detectBookFormat(filePath))) {
    coverTried_ = true;
    const auto& metrics = UITheme::getInstance().getMetrics();
    const std::string cardThumbPath = ReaderActivity::bookCacheDir(filePath) + "/" +
                                      ReaderActivity::coverThumbFileName(filePath, metrics.homeCoverHeight);
    if (coverBmpPath != cardThumbPath) {
      ReaderActivity::ensureCoverThumb(filePath, metrics.homeCoverHeight);
      if (Storage.exists(cardThumbPath.c_str())) {
        coverBmpPath = cardThumbPath;
        requestUpdate(true);
      }
    }
  }
}

void BookInfoActivity::render(RenderLock&&) {
  // Fast path: only the description page / button hints changed. Reuse the
  // already-rendered header/cover/meta by clearing just those two bands. Cap
  // consecutive partial renders to bound e-ink ghosting buildup.
  //
  // Requires Portrait orientation: the hints-gutter location is derived below
  // assuming the gutter sits at the bottom, which is only true in Portrait
  // (PortraitInverted puts it at the top; landscape orientations put it on a
  // side). In non-Portrait we fall through to a full render to avoid leaving
  // stale button labels in the real gutter.
  if (fullRenderDone && loadSucceeded && !description.empty() && partialRenderCount < MAX_PARTIAL_RENDERS &&
      renderer.getOrientation() == GfxRenderer::Portrait) {
    // The write framebuffer holds the frame from two refreshes ago (displayBuffer() swaps
    // buffers), so on alternating page turns it still contains the "Loading" screen or an
    // older page rather than the current cover/meta. Resync it to the displayed frame before
    // patching just the description/hints bands; without this the stale top section (cover or
    // "Loading") ships back to the panel and the cover appears to flip in and out.
    renderer.syncWriteBufferFromDisplayed();
    renderer.fillRect(descBandX, descBandY, descBandWidth, descBandHeight, false);
    renderer.fillRect(0, hintsBandY, renderer.getScreenWidth(), hintsBandHeight, false);
    renderDescriptionAndHints();
    ++partialRenderCount;
    renderer.displayBuffer();
    return;
  }

  partialRenderCount = 0;
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  // Header: the book itself, not the generic screen name — this screen is "about
  // this book", and the title is the one thing the user needs to keep in view while
  // scrolling the description. Falls back to the screen name before it is known.
  const std::string headerSource = !title.empty() ? title : seed_.title;
  const std::string headerText = headerSource.empty()
                                     ? std::string(tr(STR_INFO))
                                     : renderer.truncatedText(UI_12_FONT_ID, headerSource.c_str(),
                                                              contentRect.width - metrics.contentSidePadding * 2);
  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 headerText.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = contentRect.y + contentRect.height - metrics.verticalSpacing;
  const int textX = contentRect.x + metrics.contentSidePadding;
  const int textWidth = contentRect.width - metrics.contentSidePadding * 2;
  const int lineHeightSmall = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineHeightLarge = renderer.getLineHeight(UI_12_FONT_ID);

  if (!loadSucceeded) {
    const auto lastSlash = filePath.find_last_of('/');
    const std::string fileName = (lastSlash == std::string::npos) ? filePath : filePath.substr(lastSlash + 1);
    const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, fileName.c_str(), textWidth, 2, EpdFontFamily::BOLD);
    const auto errorLines = renderer.wrappedText(
        UI_10_FONT_ID, loadError.empty() ? tr(STR_ERROR_GENERAL_FAILURE) : loadError.c_str(), textWidth, 3);

    int y = contentTop + metrics.verticalSpacing;
    for (const auto& line : titleLines) {
      if (y + lineHeightLarge > contentBottom) break;
      renderer.drawText(UI_12_FONT_ID, textX, y, line.c_str(), true, EpdFontFamily::BOLD);
      y += lineHeightLarge;
    }
    y += metrics.verticalSpacing;
    for (const auto& line : errorLines) {
      if (y + lineHeightSmall > contentBottom) break;
      renderer.drawText(UI_10_FONT_ID, textX, y, line.c_str());
      y += lineHeightSmall;
    }

    if (fileSizeBytes > 0 && y + lineHeightSmall <= contentBottom) {
      y += metrics.verticalSpacing;
      const std::string sizeLine = std::string(tr(STR_FILE_SIZE)) + ": " + formatFileSize(fileSizeBytes);
      const std::string fittedSize = renderer.truncatedText(UI_10_FONT_ID, sizeLine.c_str(), textWidth);
      renderer.drawText(UI_10_FONT_ID, textX, y, fittedSize.c_str());
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // Reserve space so description always gets at least a few lines below the cover block
  const int descReserve = description.empty() ? 0 : (3 * lineHeightSmall + 2 * metrics.verticalSpacing);
  const int topSectionMaxH = contentBottom - contentTop - descReserve;

  // --- Top section: cover (left) + title/author/series (right) ---
  int topSectionBottom = contentTop;
  int metaX = textX;
  int metaWidth = textWidth;

  bool coverDrawn = false;
  if (!coverBmpPath.empty()) {
    HalFile coverFile = Storage.open(coverBmpPath.c_str());
    if (coverFile) {
      Bitmap bmp(coverFile);
      if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getWidth() > 0 && bmp.getHeight() > 0) {
        // Natural aspect ratio, capped to available height
        int coverDisplayH = std::min(bmp.getHeight(), topSectionMaxH);
        int coverDisplayW = bmp.getWidth() * coverDisplayH / bmp.getHeight();
        // Also cap width at half the text area
        if (coverDisplayW > textWidth / 2) {
          coverDisplayW = textWidth / 2;
          coverDisplayH = bmp.getHeight() * coverDisplayW / bmp.getWidth();
        }
        renderer.drawBitmap(bmp, textX, contentTop, coverDisplayW, coverDisplayH, 0, 0, true);
        coverDrawn = true;
        topSectionBottom = contentTop + coverDisplayH;

        const int gap = metrics.contentSidePadding;
        metaX = textX + coverDisplayW + gap;
        metaWidth = textWidth - coverDisplayW - gap;
      }
      coverFile.close();
    }
  }

  // No artwork: keep the same framed box with the title centred that the grid shows
  // for such books. Without it the card collapsed its cover column and the screen
  // looked half-empty instead of "this book simply has no picture".
  // The condition keys off the draw itself, not off "the cursor did not move": a
  // cached thumb path that fails to open used to leave a bare frame with no title
  // inside it, which reads as a broken screen rather than "this book has no cover".
  if (!coverDrawn) {
    const int boxW = std::min(textWidth / 2, topSectionMaxH * 3 / 4);
    const int boxH = std::min(topSectionMaxH, boxW * 4 / 3);
    std::string placeholderLabel = title;
    if (placeholderLabel.empty()) {
      // Never leave the frame empty: the file name is a worse label than the title
      // but a far better one than a blank hole.
      const size_t slash = filePath.find_last_of('/');
      placeholderLabel = slash == std::string::npos ? filePath : filePath.substr(slash + 1);
      for (const char* ext : {".fb2.zip", ".fb2", ".epub", ".txt", ".md", ".xtch", ".xtc", ".zip"}) {
        const size_t extLen = strlen(ext);
        if (placeholderLabel.size() > extLen &&
            placeholderLabel.compare(placeholderLabel.size() - extLen, extLen, ext) == 0) {
          placeholderLabel.resize(placeholderLabel.size() - extLen);
          break;
        }
      }
    }
    shelfui::drawCoverPlaceholderBox(renderer, Rect{textX, contentTop, boxW, boxH}, placeholderLabel, tr(STR_NO_COVER));
    topSectionBottom = contentTop + boxH;
    const int gap = metrics.contentSidePadding;
    metaX = textX + boxW + gap;
    metaWidth = textWidth - boxW - gap;
  }

  // The header already shows the book title, and repeating it here read as a
  // duplicate; the right column starts with the author instead.
  int metaY = contentTop;

  if (!author.empty()) {
    const std::string authorLine = std::string(tr(STR_AUTHOR)) + ": " + author;
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, authorLine.c_str(), metaWidth, 2);
    for (const auto& line : lines) {
      if (metaY + lineHeightSmall > contentBottom) break;
      const std::string fitted = renderer.truncatedText(UI_10_FONT_ID, line.c_str(), metaWidth);
      renderer.drawText(UI_10_FONT_ID, metaX, metaY, fitted.c_str());
      metaY += lineHeightSmall;
    }
    metaY += 2;
  }

  if (!series.empty()) {
    // "Серия: #2 Личный враг Бога" — the number leads the name so the eye finds
    // the reading order first, and the label makes the row self-explanatory.
    std::string seriesLine = std::string(tr(STR_SERIES)) + ": ";
    if (!seriesIndex.empty()) seriesLine += "#" + seriesIndex + " ";
    seriesLine += series;
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, seriesLine.c_str(), metaWidth, 2);
    for (const auto& line : lines) {
      if (metaY + lineHeightSmall > contentBottom) break;
      const std::string fitted = renderer.truncatedText(UI_10_FONT_ID, line.c_str(), metaWidth);
      renderer.drawText(UI_10_FONT_ID, metaX, metaY, fitted.c_str());
      metaY += lineHeightSmall;
    }
    metaY += 2;
  }

  if (!genre.empty()) {
    const std::string genreLine = std::string(tr(STR_GENRE)) + ": " + genre;
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, genreLine.c_str(), metaWidth, 2);
    for (const auto& line : lines) {
      if (metaY + lineHeightSmall > contentBottom) break;
      const std::string fitted = renderer.truncatedText(UI_10_FONT_ID, line.c_str(), metaWidth);
      renderer.drawText(UI_10_FONT_ID, metaX, metaY, fitted.c_str());
      metaY += lineHeightSmall;
    }
    metaY += 2;
  }

  // Publication rows: the container declares them or they do not appear at all,
  // so a book without dc:date does not reserve an empty line above its size.
  // One lambda, one layout rule — the three rows cannot drift apart.
  const auto drawPublicationRow = [&](const char* label, const std::string& value) {
    if (value.empty()) return;
    const std::string line = std::string(label) + ": " + value;
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, line.c_str(), metaWidth, 2);
    for (const auto& wrapped : lines) {
      if (metaY + lineHeightSmall > contentBottom) break;
      const std::string fitted = renderer.truncatedText(UI_10_FONT_ID, wrapped.c_str(), metaWidth);
      renderer.drawText(UI_10_FONT_ID, metaX, metaY, fitted.c_str());
      metaY += lineHeightSmall;
    }
    metaY += 2;
  };
  drawPublicationRow(tr(STR_YEAR), year);
  drawPublicationRow(tr(STR_PUBLISHER), publisher);
  drawPublicationRow(tr(STR_PAGES), pages);

  if (fileSizeBytes > 0) {
    const std::string sizeStr = formatFileSize(fileSizeBytes);
    metaY += metrics.verticalSpacing;
    if (metaY + lineHeightSmall <= contentBottom) {
      renderer.drawText(UI_10_FONT_ID, metaX, metaY, sizeStr.c_str());
      metaY += lineHeightSmall;
    }
  }

  // Reading stats: a progress bar (the shape readers recognise from the library)
  // plus time/last-read rows, shown for books with any recorded progress.
  if (hasReadingStats) {
    metaY += metrics.verticalSpacing;
    const int barHeight = 12;
    if (metaY + barHeight <= contentBottom) {
      char pctBuf[8];
      snprintf(pctBuf, sizeof(pctBuf), "%u%%", statProgress);
      const int pctWidth = renderer.getTextWidth(SMALL_FONT_ID, pctBuf);
      const int barWidth = std::max(40, metaWidth - pctWidth - 8);
      renderer.drawRect(metaX, metaY, barWidth, barHeight, true);
      const int clamped = std::clamp(static_cast<int>(statProgress), 0, 100);
      const int filled = (barWidth - 4) * clamped / 100;
      if (filled > 0) renderer.fillRect(metaX + 2, metaY + 2, filled, barHeight - 4);
      const int textY = metaY + (barHeight - renderer.getLineHeight(SMALL_FONT_ID)) / 2;
      renderer.drawText(SMALL_FONT_ID, metaX + barWidth + 8, textY, pctBuf);
      metaY += barHeight + metrics.verticalSpacing;
    }
    const auto drawStatRow = [&](const char* label, const std::string& value) {
      if (value.empty() || metaY + lineHeightSmall > contentBottom) return;
      const std::string line = std::string(label) + ": " + value;
      const int availableLines = std::max(1, (contentBottom - metaY) / lineHeightSmall);
      const auto lines = renderer.wrappedText(UI_10_FONT_ID, line.c_str(), metaWidth, availableLines);
      for (const auto& wrappedLine : lines) {
        if (metaY + lineHeightSmall > contentBottom) break;
        const std::string fitted = renderer.truncatedText(UI_10_FONT_ID, wrappedLine.c_str(), metaWidth);
        renderer.drawText(UI_10_FONT_ID, metaX, metaY, fitted.c_str());
        metaY += lineHeightSmall;
      }
    };

    drawStatRow(tr(STR_READING_STATS_TOTAL_TIME), formatReadingDuration(statTotalSeconds));
    drawStatRow(tr(STR_READING_STATS_LAST_READ), formatLastRead(statLastReadEpoch));
  }

  topSectionBottom = std::max(topSectionBottom, metaY);

  // --- Description: full width below the top section, paged via Left/Right ---
  descTotalPages = 0;
  descLinesPerPage = 0;
  descBandX = textX;
  descBandWidth = textWidth;
  descBandY = contentBottom;
  descBandHeight = 0;
  if (!description.empty()) {
    int y = topSectionBottom + metrics.verticalSpacing;
    if (y + lineHeightSmall + 4 < contentBottom) {
      renderer.drawLine(textX, y, contentRect.x + contentRect.width - metrics.contentSidePadding, y);
      y += 4;

      // Record the description band (below the separator) for partial redraws.
      descBandY = y;
      descBandHeight = contentBottom - y;
    }
  }

  // Hints band spans from the bottom of the content area to the screen bottom.
  hintsBandY = contentRect.y + contentRect.height;
  hintsBandHeight = renderer.getScreenHeight() - hintsBandY;

  renderDescriptionAndHints();

  fullRenderDone = true;
  renderer.displayBuffer();
}

void BookInfoActivity::renderDescriptionAndHints() {
  const int lineHeightSmall = renderer.getLineHeight(UI_10_FONT_ID);

  descTotalPages = 0;
  descLinesPerPage = 0;

  if (!description.empty() && descBandHeight > 0) {
    descLinesPerPage = descBandHeight / lineHeightSmall;
    if (descLinesPerPage > 0) {
      if (descLines.empty() || descWrappedWidth != descBandWidth) {
        descLines = renderer.wrappedText(UI_10_FONT_ID, description.c_str(), descBandWidth, 1000);
        descWrappedWidth = descBandWidth;
      }
      const int totalLines = static_cast<int>(descLines.size());
      descTotalPages = (totalLines + descLinesPerPage - 1) / descLinesPerPage;
      if (descPage >= descTotalPages) descPage = std::max(0, descTotalPages - 1);

      const int startLine = descPage * descLinesPerPage;
      const int endLine = std::min(totalLines, startLine + descLinesPerPage);
      int y = descBandY;
      const int bandBottom = descBandY + descBandHeight;
      for (int i = startLine; i < endLine; ++i) {
        if (y + lineHeightSmall > bandBottom) break;
        const std::string fitted = renderer.truncatedText(UI_10_FONT_ID, descLines[i].c_str(), descBandWidth);
        renderer.drawText(UI_10_FONT_ID, descBandX, y, fitted.c_str());
        y += lineHeightSmall;
      }
    }
  }

  const char* prevLabel = "";
  const char* nextLabel = "";
  if (hasSeriesSiblings()) {
    // The front pair walks the series, but only the directions that exist: the first
    // book of a series shows "next" only, the last one "previous" only, and a book in
    // the middle shows both — a hint for an impossible move is just noise.
    const bool hasPrevious = seriesPosition_ > 0;
    const bool hasNext = seriesPosition_ >= 0 && seriesPosition_ + 1 < static_cast<int>(seriesBooks_.size());
    prevLabel = hasPrevious ? tr(STR_PREV) : "";
    nextLabel = hasNext ? tr(STR_NEXT) : "";
  } else {
    prevLabel = (descPage > 0) ? tr(STR_PREV) : "";
    nextLabel = (descPage + 1 < descTotalPages) ? tr(STR_NEXT) : "";
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), loadSucceeded ? tr(STR_OPEN) : "", prevLabel, nextLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (hasSeriesSiblings() && descTotalPages > 1) {
    GUI.drawSideButtonHints(renderer, (descPage > 0) ? tr(STR_PREV) : "",
                            (descPage + 1 < descTotalPages) ? tr(STR_NEXT) : "");
  }
}
