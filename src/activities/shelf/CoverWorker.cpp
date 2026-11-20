#include "CoverWorker.h"

#include <BookFormat.h>
#include <Epub.h>
#include <Fb2Cover.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PngToBmpConverter.h>

#include <cstring>

#include "CoverTrace.h"
#include "activities/reader/ReaderActivity.h"

namespace shelfui {

namespace {

constexpr UBaseType_t kRequestDepth = 8;
constexpr UBaseType_t kResultDepth = 16;
// 8 KiB was enough for BMP and EPUB/FB2 artwork, but the PNG decoder for a
// user cover from the SD card needs more: with 8 KiB the task overflowed its
// stack and the board rebooted while the shelf was drawing covers. 16 KiB keeps
// the whole pipeline (PNG included) inside the task; RAM headroom is unchanged
// in practice (the firmware sits at ~17% of 320 KiB).
constexpr uint32_t kTaskStackBytes = 16384;
constexpr UBaseType_t kTaskPriority = 1;

// One finished cover, copied through the queue. Fixed size on purpose: the task
// sends and the UI receives without sharing heap or locks.
struct ResultMessage {
  int32_t cellIndex = -1;
  char thumbPath[192] = {};
};

}  // namespace

CoverWorker::~CoverWorker() { stop(); }

bool CoverWorker::start() {
  if (task_ != nullptr) return true;
  requests_ = xQueueCreate(kRequestDepth, sizeof(Request));
  results_ = xQueueCreate(kResultDepth, sizeof(ResultMessage));
  if (requests_ == nullptr || results_ == nullptr) {
    LOG_ERR("SHELF", "Cover worker queues could not be created");
    if (requests_ != nullptr) vQueueDelete(requests_);
    if (results_ != nullptr) vQueueDelete(results_);
    requests_ = nullptr;
    results_ = nullptr;
    return false;
  }
  if (xTaskCreate(&CoverWorker::taskEntry, "shelfcover", kTaskStackBytes, this, kTaskPriority, &task_) != pdPASS) {
    LOG_ERR("SHELF", "Cover worker task could not be created");
    vQueueDelete(requests_);
    vQueueDelete(results_);
    requests_ = nullptr;
    results_ = nullptr;
    task_ = nullptr;
    return false;
  }
  return true;
}

void CoverWorker::stop() {
  if (task_ == nullptr) {
    if (requests_ != nullptr) vQueueDelete(requests_);
    if (results_ != nullptr) vQueueDelete(results_);
    requests_ = nullptr;
    results_ = nullptr;
    return;
  }
  // An empty path is the shutdown sentinel: no book path can be empty, so it
  // cannot be confused with real work.
  const Request quit{};
  xQueueSend(requests_, &quit, portMAX_DELAY);
  // The task is the only writer of `task_`; wait until it clears it.
  for (int i = 0; i < 200 && task_ != nullptr; ++i) vTaskDelay(pdMS_TO_TICKS(10));
  if (task_ != nullptr) {
    LOG_ERR("SHELF", "Cover worker did not stop, killing the task");
    vTaskDelete(task_);
    task_ = nullptr;
  }
  vQueueDelete(requests_);
  vQueueDelete(results_);
  requests_ = nullptr;
  results_ = nullptr;
}

bool CoverWorker::request(int32_t cellIndex, const std::string& path, shelf::ShelfCoverSize size) {
  if (task_ == nullptr || requests_ == nullptr) return false;
  if (path.empty() || path.size() >= sizeof(Request::path)) return false;
  Request message;
  message.cellIndex = cellIndex;
  message.size = size;
  std::strncpy(message.path, path.c_str(), sizeof(message.path) - 1);
  // Never block the UI: if the queue is full the caller keeps its placeholder.
  return xQueueSend(requests_, &message, 0) == pdTRUE;
}

void CoverWorker::poll(const ResultFn& onResult) {
  if (results_ == nullptr) return;
  ResultMessage message;
  while (xQueueReceive(results_, &message, 0) == pdTRUE) {
    if (onResult && message.cellIndex >= 0) {
      Result result;
      result.cellIndex = message.cellIndex;
      result.thumbPath = message.thumbPath;
      onResult(result);
    }
  }
}

void CoverWorker::cancelPending() {
  if (requests_ != nullptr) xQueueReset(requests_);
}

void CoverWorker::taskEntry(void* arg) { static_cast<CoverWorker*>(arg)->run(); }

std::string CoverWorker::makeCover(const std::string& path, shelf::ShelfCoverSize size) {
  coverTrace("makeCover %s size=%d", path.c_str(), static_cast<int>(size));
  // 1. Cheap attempt: a cached thumbnail, a sidecar image, or a cover package that
  //    is already unpacked. This is also where the "structurally absent" verdict
  //    turns into a placeholder.
  std::string thumb = service_.ensureCover(path, size);
  coverTrace("  step1 ensureCover -> %s", thumb.empty() ? "empty" : thumb.c_str());
  if (!thumb.empty()) return thumb;
  if (paused_) return {};

  // 1b. FB2/FB2.ZIP: pull just the cover picture out of the file and decode it. This
  //     is the cheap path that scales to a large library — no package.epub, no spine,
  //     no inline images, only the referenced <binary> streamed to cover.img.
  const shelf::BookFormat format = shelf::detectBookFormat(path);
  if (format == shelf::BookFormat::Fb2 || format == shelf::BookFormat::Fb2Zip) {
    const std::string cacheDir = ReaderActivity::bookCacheDir(path);
    if (!cacheDir.empty()) {
      // The cache directory of a book that was never opened does not exist yet, and
      // writing cover.img into it would fail silently — which is exactly why one of the
      // two test books got its artwork and the other did not.
      Storage.mkdir(cacheDir.c_str(), true);
      const std::string coverImage = cacheDir + "/cover.img";
      const std::string noCoverMarker = cacheDir + "/.nocover";
      if (!Storage.exists(coverImage.c_str()) && Storage.exists(noCoverMarker.c_str())) {
        coverTrace("  step1b marked as no-cover, skipped");
        return {};
      }
      if (!Storage.exists(coverImage.c_str())) {
        if (!shelf::extractFb2Cover(path, format, coverImage)) {
          // Remember the verdict: a book without artwork must not be re-read on every
          // page rebuild (that would cost a full archive scan per visit).
          (void)Storage.writeFile(noCoverMarker.c_str(), "1");
        }
      }
      if (Storage.exists(coverImage.c_str())) {
        const shelf::CoverBox cellBox = shelf::coverBox(size);
        const std::string name = ReaderActivity::coverThumbFileName(path, cellBox.width, cellBox.height);
        const std::string made =
            ReaderActivity::convertSidecarToBmp(cacheDir, coverImage, cellBox.width, cellBox.height, name, true);
        coverTrace("  step1b fb2 cover -> %s", made.empty() ? "failed" : made.c_str());
        if (!made.empty()) return made;
      } else {
        coverTrace("  step1b no cover image in the file");
      }
    }
  }
  if (paused_) return {};

  // 2. .fb2.zip and EPUB covers are extracted and decoded in slices on purpose:
  //    the synchronous call above refuses to inflate a whole archive in one go.
  //    Here we have a task of our own, so pumping those slices to completion is
  //    exactly what the reader screen does across its ticks — and what the shelf
  //    never did, which is why its archive covers stayed empty.
  if (auto extract = ReaderActivity::beginCoverExtractSession(path)) {
    while (!paused_) {
      const auto status = extract->continueStep(4096);
      if (status != ReaderActivity::CoverExtractSession::Status::Running) {
        coverTrace("  step2 extract status=%d bytes=%u/%u", static_cast<int>(status),
                   static_cast<unsigned>(extract->bytesProduced()), static_cast<unsigned>(extract->totalBytes()));
        if (status == ReaderActivity::CoverExtractSession::Status::Error) {
          LOG_ERR("SHELF", "Cover extraction failed for %s", path.c_str());
        }
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    extract.reset();
  } else {
    coverTrace("  step2 extract session not started");
  }
  if (paused_) return {};
  // A JPEG/JPEG-in-package cover can be decoded now that cover.img is on the card.
  thumb = service_.ensureCover(path, size);
  coverTrace("  step3 ensureCover -> %s", thumb.empty() ? "empty" : thumb.c_str());
  if (!thumb.empty()) return thumb;

  if (paused_) return {};
  // 2b. The reader's converted FB2 package already exists on the card, but its
  //     cover.img was never extracted (the synchronous path refuses to inflate the
  //     archive). Extract it here — this is the step that finally lets the shelf show
  //     the artwork of a .fb2.zip without opening the book.
  if (shelf::detectBookFormat(path) != shelf::BookFormat::Epub) {
    const std::string packageDir = ReaderActivity::bookCacheDir(path);
    const bool havePackage = !packageDir.empty() && Storage.exists((packageDir + "/OEBPS/content.opf").c_str());
    coverTrace("  step4 package=%s dir=%s", havePackage ? "yes" : "no", packageDir.c_str());
    if (havePackage) {
      Epub package(packageDir + "/package.epub", "/.crosspoint");
      const bool loaded = package.loadForCover();
      const bool cached = loaded && package.ensureCoverImageCached();
      coverTrace("  step4 loadForCover=%d coverImg=%d path=%s", loaded ? 1 : 0, cached ? 1 : 0,
                 package.getCoverImageCachePath().c_str());
      if (cached) {
        thumb = service_.ensureCover(path, size);
        coverTrace("  step5 ensureCover -> %s", thumb.empty() ? "empty" : thumb.c_str());
        if (!thumb.empty()) return thumb;
      }
    }
  }
  // 3. PNG covers need the sliced decoder (the synchronous one bails on large
  //    images), so run it to completion right here.
  const shelf::CoverBox box = shelf::coverBox(size);
  ReaderActivity::PngThumbFiles files;
  if (auto png = ReaderActivity::beginPngThumbSession(path, box.width, box.height, files, true)) {
    while (!paused_) {
      const auto status = png->continueRows(6);
      if (status != PngDecodeSession::Status::Running) break;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    files.close();
  }
  if (paused_) return {};
  return service_.ensureCover(path, size);
}

void CoverWorker::run() {
  while (true) {
    Request message;
    if (xQueueReceive(requests_, &message, portMAX_DELAY) != pdTRUE) continue;
    if (message.path[0] == '\0') break;
    if (paused_) continue;

    const std::string thumb = makeCover(message.path, message.size);
    if (thumb.empty()) {
      // Transient failure: the cover policy decides whether a later pass retries;
      // the UI simply keeps the placeholder for now.
      LOG_DBG("SHELF", "Cover worker could not make %s", message.path);
      continue;
    }
    if (thumb.size() >= sizeof(ResultMessage::thumbPath)) continue;
    ResultMessage result;
    result.cellIndex = message.cellIndex;
    std::strncpy(result.thumbPath, thumb.c_str(), sizeof(result.thumbPath) - 1);
    if (xQueueSend(results_, &result, pdMS_TO_TICKS(50)) != pdTRUE) {
      LOG_DBG("SHELF", "Cover worker result queue full, dropping %s", thumb.c_str());
    }
  }
  task_ = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace shelfui
