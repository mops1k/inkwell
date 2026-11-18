#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "CoverPolicy.h"

namespace shelf {

// Pending cover work for the grid, in the order the books became visible.
//
// This is the pure part of the background-cover pipeline: it holds no RTOS
// objects and does no I/O, so the queueing rules (one request per book, newest
// page first, bounded memory) are unit-testable on the host. The firmware-side
// CoverWorker owns the task and consumes this queue.
class CoverQueue {
 public:
  // A grid page never shows more covers than this; the bound also keeps a scan of
  // a large library from growing the queue without limit.
  static constexpr size_t kMaxPending = 24;

  // Returns false when the path is already queued (a repaint, a page return) or
  // the queue is full — both are normal, not errors.
  bool push(const std::string& path, ShelfCoverSize size);

  // Removes the front request. False when empty.
  bool pop(std::string& path, ShelfCoverSize& size);

  bool empty() const { return items_.empty(); }
  size_t size() const { return items_.size(); }
  void clear() { items_.clear(); }
  void erase(const std::string& path);

 private:
  struct Item {
    std::string path;
    ShelfCoverSize size = ShelfCoverSize::Cell;
  };
  std::vector<Item> items_;
};

}  // namespace shelf
