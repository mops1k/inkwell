#include "activities/shelf/ShelfCoverService.h"

#include <HalStorage.h>
#include <Logging.h>

#include "activities/reader/ReaderActivity.h"

namespace {

std::string thumbPathFor(const std::string& bookPath, shelf::ShelfCoverSize size) {
  const shelf::CoverBox box = shelf::coverBox(size);
  return ReaderActivity::bookCacheDir(bookPath) + "/" +
         ReaderActivity::coverThumbFileName(bookPath, box.width, box.height);
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

std::string ShelfCoverService::ensureCover(const std::string& bookPath, shelf::ShelfCoverSize size) {
  const shelf::CoverBox box = shelf::coverBox(size);
  const std::string thumbPath = thumbPathFor(bookPath, size);
  Attempt* attempt = findAttempt(thumbPath);
  const uint8_t previousFailures = attempt != nullptr ? attempt->transientAttempts : 0;

  switch (shelf::decideCoverAction(bookFatDateTime(bookPath), thumbState(thumbPath, size))) {
    case shelf::CoverDecision::Reuse:
      return thumbPath;
    case shelf::CoverDecision::RetryLater:
      return {};
    case shelf::CoverDecision::Placeholder:
      return ReaderActivity::writeCoverPlaceholderBmp(thumbPath, box.width, box.height) ? thumbPath : std::string();
    case shelf::CoverDecision::Generate:
      break;
  }

  // crop=false: the shelf never cuts or stretches a cover (requirement 6).
  const ThumbResult result = ReaderActivity::ensureCoverThumb(bookPath, box.width, box.height, false);
  switch (shelf::decideAfterFailure(result, previousFailures)) {
    case shelf::CoverDecision::Reuse:
      if (attempt != nullptr) attempt->transientAttempts = 0;
      return thumbPath;
    case shelf::CoverDecision::Placeholder:
      // Settle on a valid placeholder instead of retrying forever. The book
      // stays visible; only its cover is generic.
      return ReaderActivity::writeCoverPlaceholderBmp(thumbPath, box.width, box.height) ? thumbPath : std::string();
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
