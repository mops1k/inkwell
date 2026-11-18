#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ShelfIndex.h"
#include "ShelfTypes.h"

namespace shelf {

// Opening a book is the only device-specific step of indexing, so it is passed
// in as a function pointer (no std::function allocation on the C3): the device
// wires readBookMetadata(), tests drive the walk/merge logic with a fake.
using MetadataFn = bool (*)(void* ctx, const std::string& path, BookFormat format, BookMeta& out);

// Called once per book that existed in the previous index but is gone from the
// card, so the caller can drop that book's thumbnail cache.
using RemovedFn = void (*)(void* ctx, const std::string& path);

struct ShelfScanOptions {
  // Plan Ф1: "/books" recursively (where books normally live) plus the SD root
  // non-recursively, so a book dropped straight onto the card is still found
  // without walking every folder of a 16 GB card.
  std::string booksRoot = "/books";
  std::string root = "/";
  // OPDS downloads go to a configurable folder (SETTINGS.opdsDownloadFolder),
  // not necessarily under /books: without walking it those books never reach
  // the shelf. Empty means "no extra folder" (downloads land in the SD root,
  // which the shallow root walk already covers).
  std::string downloadsRoot;
  uint16_t maxDepth = kMaxLibraryDepth;
  size_t maxBooks = kMaxIndexBooks;
};

struct ShelfScanStats {
  size_t entriesScanned = 0;  // every directory entry visited
  size_t filesSeen = 0;       // supported book files encountered
  size_t kept = 0;            // unchanged signature: previous metadata reused
  size_t refreshed = 0;       // metadata extracted again
  size_t removed = 0;         // books no longer on the card
  bool truncated = false;     // maxBooks reached
};

// Walks the card and merges the result into the index. Incremental by design:
// a file whose size and FAT timestamp did not change is never reopened, which
// keeps a 100-book rescan in the tens of milliseconds instead of minutes.
class ShelfIndexer {
 public:
  ShelfIndexer(MetadataFn metadataFn, void* ctx, RemovedFn removedFn = nullptr, void* removedCtx = nullptr)
      : fetch(metadataFn), fetchCtx(ctx), removed(removedFn), removedCtx(removedCtx) {}

  ShelfScanStats scan(ShelfIndex& index, const ShelfScanOptions& options);

 private:
  struct WalkState;

  void walkDirectory(WalkState& state, const std::string& dir, uint16_t depth, bool recursive);
  void considerFile(WalkState& state, const std::string& path, uint32_t size, uint32_t fatDateTime, BookFormat format);

  MetadataFn fetch;
  void* fetchCtx;
  RemovedFn removed;
  void* removedCtx;
};

}  // namespace shelf
