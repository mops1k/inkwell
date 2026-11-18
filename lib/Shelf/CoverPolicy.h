#pragma once

#include <Epub/ThumbResult.h>

#include <cstddef>
#include <cstdint>

namespace shelf {

// The two cover boxes the shelf draws. Requirement 6 fixes them: a grid cell
// and a book card, both 3:4, rendered with the firmware's 4 gray levels, i.e.
// 2 bits per pixel. One source of truth so the grid, the card and the tests can
// never disagree about what "the cell cover" means.
struct CoverBox {
  int width;
  int height;
};

enum class ShelfCoverSize : uint8_t { Cell, Card };

constexpr CoverBox coverBox(ShelfCoverSize size) {
  return size == ShelfCoverSize::Card ? CoverBox{264, 352} : CoverBox{180, 240};
}

// Bits per pixel of the existing thumbnail BMPs (4 gray levels).
constexpr int kCoverBitsPerPixel = 2;

// Payload bytes of one cover: what the SD card must hold for the pixels alone
// (the BMP headers and palette are constant and tiny). Used by tests to pin the
// 2-bit contract: a regression to 1 bpp halves these numbers.
constexpr size_t coverPixelBytes(CoverBox box) {
  constexpr size_t bitsPerByte = 8;
  const size_t pixels = static_cast<size_t>(box.width) * static_cast<size_t>(box.height);
  return (pixels * static_cast<size_t>(kCoverBitsPerPixel) + bitsPerByte - 1) / bitsPerByte;
}

// FAT timestamps have two-second granularity, so a book and its freshly written
// thumbnail can differ by one tick without anything having changed.
constexpr uint32_t kCoverStaleSlackSeconds = 2;

enum class CoverDecision : uint8_t {
  Reuse,        // draw the existing thumbnail
  Generate,     // missing, incomplete or older than the book
  Placeholder,  // the book structurally has no cover (or retries are exhausted)
  RetryLater,   // transient failure; try again on a later pass
};

// What is currently on the SD card for one book and size.
struct CoverThumbState {
  bool exists = false;       // the BMP (or the 0-byte sentinel) is there
  bool complete = false;     // ReaderActivity::isCoverThumbComplete() passed
  bool sentinel = false;     // 0-byte "structurally absent" marker
  uint32_t fatDateTime = 0;  // FAT timestamp of the thumbnail
};

// Order matters: a sentinel is the generator's own verdict and always wins over
// a stale timestamp.
CoverDecision decideCoverAction(uint32_t bookFatDateTime, const CoverThumbState& thumb);

// After a generation attempt: Ok means draw it, a structural absence means a
// valid placeholder, and a transient failure is retried once more in this
// session before the caller settles on a placeholder (same rule HomeActivity
// uses, so a flaky decoder cannot spin forever).
CoverDecision decideAfterFailure(ThumbResult result, uint8_t transientAttempts);

// FAT -> seconds since an arbitrary but monotonic epoch, enough to order two
// timestamps. Returns 0 for an unknown (zero) timestamp.
uint32_t fatTimeToSeconds(uint32_t fatDateTime);

// True when the thumbnail predates the book by more than the FAT granularity;
// an unknown timestamp on either side never triggers a regeneration.
bool coverIsStale(uint32_t bookFatDateTime, uint32_t thumbFatDateTime);

}  // namespace shelf
