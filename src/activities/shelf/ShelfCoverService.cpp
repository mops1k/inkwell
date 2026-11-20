#include "activities/shelf/ShelfCoverService.h"

#include <BookFormat.h>
#include <HalStorage.h>
#include <Logging.h>

#include <string>
#include <vector>

#include "activities/reader/ReaderActivity.h"

namespace {

std::string thumbPathFor(const std::string& bookPath, shelf::ShelfCoverSize size) {
  const shelf::CoverBox box = shelf::coverBox(size);
  return ReaderActivity::bookCacheDir(bookPath) + "/" +
         ReaderActivity::coverThumbFileName(bookPath, box.width, box.height);
}

// Cell covers used to be generated aspect-fit into a 3:4 box, so a cover with a
// different ratio carried white bars inside the bitmap. They are cropped now, and
// thumbs made the old way have to be rebuilt: the marker makes that happen once
// per book instead of on every visit.
constexpr const char* kCellCropMarker = ".covercrop1";

// A book without artwork gets no synthetic bitmap: the grid and the card draw
// their own titled plate (and the card adds "no cover" underneath), while the
// white placeholder BMP this used to leave behind only ever looked like an empty
// frame. The 0-byte file is the existing "structurally absent" sentinel, so the
// verdict costs nothing on later passes and no book has to be read again.
void settleWithoutCover(const std::string& thumbPath) {
  if (Storage.exists(thumbPath.c_str())) Storage.remove(thumbPath.c_str());
  HalFile file;
  if (Storage.openFileForWrite("SHELF", thumbPath, file)) file.close();
}

void dropLetterboxedCellThumb(const std::string& bookPath, const std::string& thumbPath) {
  const std::string cacheDir = ReaderActivity::bookCacheDir(bookPath);
  if (cacheDir.empty()) return;
  const std::string marker = cacheDir + "/" + kCellCropMarker;
  if (Storage.exists(marker.c_str())) return;
  if (Storage.exists(thumbPath.c_str())) {
    Storage.remove(thumbPath.c_str());
  }
  Storage.writeFile(marker.c_str(), "1");
}

uint32_t bookFatDateTime(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("SHELF", path, file)) return 0;
  uint16_t date = 0;
  uint16_t time = 0;
  const bool ok = file.getModifyDateTime(&date, &time);
  file.close();
  return ok ? (static_cast<uint32_t>(date) << 16) | time : 0;
}

shelf::CoverThumbState thumbState(const std::string& thumbPath, shelf::ShelfCoverSize size) {
  const shelf::CoverBox box = shelf::coverBox(size);
  shelf::CoverThumbState state;
  HalFile file;
  if (!Storage.openFileForRead("SHELF", thumbPath, file)) return state;
  state.exists = true;
  // A zero-byte file is the legacy "structurally absent" sentinel; its presence
  // is a verdict, not a thumbnail.
  state.sentinel = file.fileSize() == 0;
  uint16_t date = 0;
  uint16_t time = 0;
  const bool hasTime = file.getModifyDateTime(&date, &time);
  file.close();
  if (hasTime) state.fatDateTime = (static_cast<uint32_t>(date) << 16) | time;
  state.complete = !state.sentinel && ReaderActivity::isCoverThumbComplete(thumbPath, box.width, box.height);
  return state;
}

}  // namespace

ShelfCoverService::Attempt* ShelfCoverService::findAttempt(const std::string& thumbPath) {
  for (auto& attempt : attempts_) {
    if (attempt.thumbPath == thumbPath) return &attempt;
  }
  return nullptr;
}

ShelfCoverService::Attempt& ShelfCoverService::noteFailure(const std::string& thumbPath) {
  if (Attempt* existing = findAttempt(thumbPath)) return *existing;
  if (attempts_.size() >= kMaxTrackedAttempts) attempts_.erase(attempts_.begin());
  attempts_.push_back(Attempt{thumbPath, 0});
  return attempts_.back();
}

std::string ShelfCoverService::existingCover(const std::string& bookPath, shelf::ShelfCoverSize size) const {
  // A .txt/.md has no artwork: whatever bitmap sits in its cache directory is a
  // leftover from an older build that synthesised one (it rendered as a black
  // block with noise). The grid draws its title plate instead.
  if (!shelf::formatHasArtwork(shelf::detectBookFormat(bookPath))) return {};
  const std::string thumbPath = thumbPathFor(bookPath, size);
  const shelf::CoverThumbState state = thumbState(thumbPath, size);
  if (!state.exists || state.sentinel || !state.complete) return {};
  return thumbPath;
}

std::string ShelfCoverService::ensureCover(const std::string& bookPath, shelf::ShelfCoverSize size) {
  const shelf::CoverBox box = shelf::coverBox(size);
  const std::string thumbPath = thumbPathFor(bookPath, size);
  // No artwork in this format (TXT/Markdown): drop any bogus bitmap an older build
  // left behind and let the grid draw its own title plate.
  if (!shelf::formatHasArtwork(shelf::detectBookFormat(bookPath))) {
    if (Storage.exists(thumbPath.c_str())) Storage.remove(thumbPath.c_str());
    return {};
  }
  if (size == shelf::ShelfCoverSize::Cell) dropLetterboxedCellThumb(bookPath, thumbPath);
  Attempt* attempt = findAttempt(thumbPath);
  const uint8_t previousFailures = attempt != nullptr ? attempt->transientAttempts : 0;

  switch (shelf::decideCoverAction(bookFatDateTime(bookPath), thumbState(thumbPath, size))) {
    case shelf::CoverDecision::Reuse:
      return thumbPath;
    case shelf::CoverDecision::RetryLater:
      return {};
    case shelf::CoverDecision::Placeholder:
      settleWithoutCover(thumbPath);
      return {};
    case shelf::CoverDecision::Generate:
      break;
  }

  // crop=true: a cell is a fixed 3:4 frame, so the artwork fills it. Aspect-fit
  // (crop=false) left white bars inside the bitmap whenever the cover ratio
  // differed, which the user saw as a white strip beside the picture.
  const ThumbResult result = ReaderActivity::ensureCoverThumb(bookPath, box.width, box.height, true);
  switch (shelf::decideAfterFailure(result, previousFailures)) {
    case shelf::CoverDecision::Reuse:
      if (attempt != nullptr) attempt->transientAttempts = 0;
      return thumbPath;
    case shelf::CoverDecision::Placeholder:
      // Settle on a valid plate instead of retrying forever: the book stays
      // visible with its title, only the artwork is absent.
      settleWithoutCover(thumbPath);
      return {};
    case shelf::CoverDecision::Generate:
    case shelf::CoverDecision::RetryLater:
      break;
  }

  LOG_DBG("SHELF", "Cover not ready for %s (%ux%u), attempt %u", bookPath.c_str(), static_cast<unsigned>(box.width),
          static_cast<unsigned>(box.height), static_cast<unsigned>(previousFailures + 1));
  ++noteFailure(thumbPath).transientAttempts;
  return {};
}

void ShelfCoverService::forgetBook(const std::string& bookPath) {
  const std::string cacheDir = ReaderActivity::bookCacheDir(bookPath);
  if (cacheDir.empty()) return;
  if (!Storage.removeDir(cacheDir.c_str())) {
    LOG_DBG("SHELF", "No cover cache to remove for %s", bookPath.c_str());
  }
}

void ShelfCoverService::forgetBookThunk(void*, const std::string& path) { forgetBook(path); }

void ShelfCoverService::refreshCovers(const std::string& bookPath) {
  const std::string cacheDir = ReaderActivity::bookCacheDir(bookPath);
  if (cacheDir.empty()) return;

  FsFile dir = Storage.open(cacheDir.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  // Collect first, remove after the directory is closed: removing an entry
  // during iteration makes SdFat skip the following one.
  std::vector<std::string> thumbs;
  char name[128];
  for (FsFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    file.close();
    const std::string entry(name);
    if (entry.rfind("thumb_", 0) == 0) thumbs.push_back(cacheDir + "/" + entry);
  }
  dir.close();

  for (const std::string& path : thumbs) {
    if (!Storage.remove(path.c_str())) LOG_DBG("SHELF", "Could not remove %s", path.c_str());
  }
  LOG_DBG("SHELF", "Cover thumbnails refreshed for %s (%zu file(s))", bookPath.c_str(), thumbs.size());
}
