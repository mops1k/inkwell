#include "ShelfIndexStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <utility>

namespace shelf {

ShelfPaths ShelfPaths::forDirectory(std::string dir) {
  ShelfPaths paths;
  paths.dir = std::move(dir);
  paths.file = paths.dir + "/books.json";
  paths.tmp = paths.dir + "/books.json.tmp";
  paths.bak = paths.dir + "/books.json.bak";
  return paths;
}

ShelfIndexStore::ShelfIndexStore(ShelfPaths paths) : where(std::move(paths)) {}

bool ShelfIndexStore::readFile(const std::string& path, std::string& out) const {
  out.clear();
  HalFile file;
  if (!Storage.openFileForRead("SHELF", path, file)) return false;
  char buf[512];
  int read = 0;
  while ((read = file.read(buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(read));
  file.close();
  return !out.empty();
}

bool ShelfIndexStore::writeFile(const std::string& path, const std::string& data) const {
  // A leftover .tmp from an interrupted save must never be appended to.
  Storage.remove(path.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("SHELF", path, file)) return false;

  constexpr size_t kChunk = 1024;
  size_t offset = 0;
  bool ok = true;
  while (offset < data.size()) {
    const size_t chunk = std::min(kChunk, data.size() - offset);
    if (file.write(data.data() + offset, chunk) != chunk) {
      ok = false;
      break;
    }
    offset += chunk;
  }
  file.flush();
  file.close();
  if (!ok) Storage.remove(path.c_str());
  return ok;
}

bool ShelfIndexStore::load(ShelfIndex& out, bool* recoveredFromBak) {
  if (recoveredFromBak) *recoveredFromBak = false;
  std::string payload;
  if (readFile(where.file, payload) && parseIndex(payload, out)) return true;

  if (readFile(where.bak, payload) && parseIndex(payload, out)) {
    if (recoveredFromBak) *recoveredFromBak = true;
    LOG_INF("SHELF", "Index recovered from %s", where.bak.c_str());
    return true;
  }
  LOG_DBG("SHELF", "No usable index at %s", where.file.c_str());
  return false;
}

bool ShelfIndexStore::save(const ShelfIndex& index) {
  if (!Storage.ensureDirectoryExists(where.dir.c_str())) {
    LOG_ERR("SHELF", "Cannot create %s", where.dir.c_str());
    return false;
  }

  std::string payload;
  serializeIndex(index, payload);
  if (!writeFile(where.tmp, payload)) {
    LOG_ERR("SHELF", "Cannot write %s", where.tmp.c_str());
    return false;
  }

  if (Storage.exists(where.file.c_str())) {
    if (Storage.exists(where.bak.c_str())) Storage.remove(where.bak.c_str());
    if (!Storage.rename(where.file.c_str(), where.bak.c_str())) {
      // Keep the previous index untouched rather than risk losing both.
      LOG_ERR("SHELF", "Cannot rotate %s", where.file.c_str());
      Storage.remove(where.tmp.c_str());
      return false;
    }
  }

  if (!Storage.rename(where.tmp.c_str(), where.file.c_str())) {
    LOG_ERR("SHELF", "Cannot commit %s", where.file.c_str());
    return false;
  }
  return true;
}

}  // namespace shelf
