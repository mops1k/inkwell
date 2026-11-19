#pragma once

#include <string>
#include <vector>

#include "LibraryQuery.h"

namespace shelf {

// What the shelf screen remembers between boots: the active filter, the sort
// order and the books the user hid. Kept apart from books.json — a rescan
// rewrites the index, while the user's library view must survive it.
struct LibraryState {
  LibraryFilter filter;
  SortKey sort = SortKey::Title;
  // Reverse order of `sort`. Stored per library, not per key: the user chose a
  // direction and it must survive a reboot exactly like the field itself.
  bool descending = false;
  // Presentation of the library ("grid" or "list"), stored as a stable name so
  // the shelf layer never depends on a UI enum. Unknown values fall back to grid.
  std::string view = "grid";
  std::vector<std::string> hiddenPaths;
};

// Same crash-safe pattern as ShelfIndexStore (AGENTS.md: ".tmp + rename,
// при повреждении — восстановление, а не падение"). A missing or broken
// state file is not an error: the shelf simply opens with no filter.
class ShelfStateStore {
 public:
  explicit ShelfStateStore(std::string dir = "/.crosspoint/shelf");

  bool load(LibraryState& out);
  bool save(const LibraryState& state);

 private:
  std::string file;
  std::string tmp;
  std::string bak;
};

// JSON codec, exposed for host tests (the store itself only does file I/O).
void serializeLibraryState(const LibraryState& state, std::string& out);
bool parseLibraryState(std::string_view json, LibraryState& out);

}  // namespace shelf
