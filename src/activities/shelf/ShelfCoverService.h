#pragma once

#include <CoverPolicy.h>

#include <cstdint>
#include <string>
#include <vector>

// One entry point for shelf covers. It never decodes or dithers anything by
// itself: the existing ReaderActivity cover pipeline owns that (AGENTS.md:
// "обложки — только через существующий конвейер"). This class only decides
// WHEN to ask it, tracks transient retries for the current session, and cleans
// up after books that left the card.
class ShelfCoverService {
 public:
  // Ready-to-draw BMP path for this book and size. An empty result means
  // "nothing to draw yet" (transient failure exhausted or nothing written); the
  // caller should simply not draw a cover on this pass.
  std::string ensureCover(const std::string& bookPath, shelf::ShelfCoverSize size);

  // Existing, complete cover for this book and size, or an empty string. Used for
  // the first paint: reading a finished thumbnail is fast, generating one is not,
  // so the grid appears immediately and covers arrive one per loop tick.
  std::string existingCover(const std::string& bookPath, shelf::ShelfCoverSize size) const;

  // Drop every derived artifact of a book that left the card: its thumbnail
  // cache directory only — user sidecar images are never touched.
  static void forgetBook(const std::string& bookPath);

  // Adapter for ShelfIndexer's onBookRemoved hook (function pointer, no heap).
  static void forgetBookThunk(void* ctx, const std::string& path);

  // Drop only the book's cover thumbnails (thumb_* files) so the next pass
  // regenerates them, leaving the reader's cached parsing artifacts in place.
  static void refreshCovers(const std::string& bookPath);

 private:
  struct Attempt {
    std::string thumbPath;
    uint8_t transientAttempts = 0;
  };

  Attempt* findAttempt(const std::string& thumbPath);
  Attempt& noteFailure(const std::string& thumbPath);

  // Bounded on purpose: at most ~1000 indexed books, and a session rarely opens
  // more than a screenful of covers.
  static constexpr size_t kMaxTrackedAttempts = 128;
  std::vector<Attempt> attempts_;
};
