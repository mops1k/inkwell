#pragma once

#include <string>

#include "ShelfIndex.h"

namespace shelf {

// Every path the shelf owns on the SD card. One directory, one document, two
// side files: the write-aside temp and the previous good copy.
struct ShelfPaths {
  std::string dir;
  std::string file;
  std::string tmp;
  std::string bak;

  static ShelfPaths forDirectory(std::string dir);
};

// Crash-safe persistence for ShelfIndex (AGENTS.md: ".tmp + rename, при
// повреждении — восстановление, а не падение").
class ShelfIndexStore {
 public:
  explicit ShelfIndexStore(ShelfPaths paths = ShelfPaths::forDirectory("/.crosspoint/shelf"));

  const ShelfPaths& paths() const { return where; }

  // current first, then .bak. Returns false when neither copy parses — the
  // caller is expected to rebuild from the card, never to crash.
  bool load(ShelfIndex& out, bool* recoveredFromBak = nullptr);

  // Write-aside: serialize to .tmp, flush and close it, move the previous
  // current to .bak, then rename .tmp into place. A failure at any step leaves
  // the previous index readable.
  bool save(const ShelfIndex& index);

 private:
  bool readFile(const std::string& path, std::string& out) const;
  bool writeFile(const std::string& path, const std::string& data) const;

  ShelfPaths where;
};

}  // namespace shelf
