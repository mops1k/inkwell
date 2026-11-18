#include "CoverQueue.h"

namespace shelf {

bool CoverQueue::push(const std::string& path, ShelfCoverSize size) {
  if (path.empty() || items_.size() >= kMaxPending) return false;
  for (const auto& item : items_) {
    // The same book can be requested twice before its cover lands (page rebuilt,
    // book re-measured); one decode is enough.
    if (item.path == path) return false;
  }
  items_.push_back(Item{path, size});
  return true;
}

bool CoverQueue::pop(std::string& path, ShelfCoverSize& size) {
  if (items_.empty()) return false;
  path = items_.front().path;
  size = items_.front().size;
  items_.erase(items_.begin());
  return true;
}

void CoverQueue::erase(const std::string& path) {
  for (auto it = items_.begin(); it != items_.end();) {
    it = it->path == path ? items_.erase(it) : it + 1;
  }
}

}  // namespace shelf
