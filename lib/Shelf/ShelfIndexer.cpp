#include "ShelfIndexer.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "BookFormat.h"
#include "BookNameMeta.h"

namespace shelf {

namespace {

bool isHiddenName(const char* name) { return name[0] == '.' || name[0] == '\0'; }

// Caches (/.crosspoint), hidden folders and OS bookkeeping are never books.
bool isSkippedDirectory(const char* name) {
  if (isHiddenName(name)) return true;
  if (std::strcmp(name, "$RECYCLE.BIN") == 0) return true;
  if (std::strcmp(name, "System Volume Information") == 0) return true;
  return false;
}

std::string joinPath(const std::string& dir, const char* name) {
  if (dir.empty() || dir == "/") return std::string("/") + name;
  if (dir.back() == '/') return dir + name;
  return dir + "/" + name;
}

}  // namespace

struct ShelfIndexer::WalkState {
  const ShelfIndex& old;
  std::vector<ShelfBook>& fresh;
  const ShelfScanOptions& options;
  ShelfScanStats stats;
  size_t matchedOld = 0;
  size_t openedDirs = 0;
  bool truncated = false;
};

void ShelfIndexer::walkDirectory(WalkState& state, const std::string& dir, uint16_t depth, bool recursive,
                                 uint16_t depthLimit) {
  HalFile handle;
  if (!Storage.openFileForRead("SHELF", dir, handle) || !handle.isDirectory()) {
    LOG_DBG("SHELF", "Skipping unreadable %s", dir.c_str());
    return;
  }
  ++state.openedDirs;

  while (true) {
    HalFile entry = handle.openNextFile();
    if (!entry) break;
    char name[256];
    const size_t len = entry.getName(name, sizeof(name) - 1);
    if (len == 0) continue;
    name[len] = '\0';
    ++state.stats.entriesScanned;

    if (entry.isDirectory()) {
      if (!recursive || state.truncated) continue;
      if (isSkippedDirectory(name)) continue;
      if (depth >= depthLimit) continue;
      walkDirectory(state, joinPath(dir, name), static_cast<uint16_t>(depth + 1), true, depthLimit);
      continue;
    }

    if (isHiddenName(name)) continue;
    const std::string path = joinPath(dir, name);
    const BookFormat format = detectBookFormat(path);
    if (format == BookFormat::Unknown) continue;
    ++state.stats.filesSeen;
    if (state.fresh.size() >= state.options.maxBooks) {
      state.truncated = true;
      break;
    }

    uint16_t date = 0;
    uint16_t time = 0;
    entry.getModifyDateTime(&date, &time);
    const uint32_t fatDateTime = (static_cast<uint32_t>(date) << 16) | time;
    considerFile(state, path, static_cast<uint32_t>(entry.fileSize()), fatDateTime, format);
  }
}

void ShelfIndexer::considerFile(WalkState& state, const std::string& path, uint32_t size, uint32_t fatDateTime,
                                BookFormat format) {
  // Roots can nest (a downloads folder inside /books, or /books itself), and a
  // book must appear exactly once: the first walk that reaches the file wins.
  for (const auto& listed : state.fresh) {
    if (listed.path == path) return;
  }

  if (state.fresh.size() >= state.options.maxBooks) {
    state.truncated = true;
    return;
  }

  ShelfBook book;
  book.path = path;
  book.size = size;
  book.fatDateTime = fatDateTime;

  const ShelfBook* previous = state.old.find(path);
  if (previous != nullptr) ++state.matchedOld;

  if (previous != nullptr && previous->sameFileSignature(book)) {
    // Unchanged file: reuse the previous entry verbatim, including a failed
    // metadata extraction — retrying it on every scan would defeat increment.
    book = *previous;
    ++state.stats.kept;
    state.fresh.push_back(std::move(book));
    return;
  }

  book.format = format;
  book.metaOk = fetch != nullptr && fetch(fetchCtx, path, format, book.meta);
  if (!book.metaOk) {
    // A broken container must stay on the shelf with a readable name.
    const BookMeta fallback = parseBookName(path);
    if (book.meta.title.empty()) book.meta.title = fallback.title;
    if (book.meta.author.empty()) book.meta.author = fallback.author;
  }
  if (book.meta.title.empty()) book.meta.title = fileNameStem(path);
  ++state.stats.refreshed;
  state.fresh.push_back(std::move(book));
}

ShelfScanStats ShelfIndexer::scan(ShelfIndex& index, const ShelfScanOptions& options) {
  std::vector<ShelfBook> fresh;
  fresh.reserve(std::min(options.maxBooks, index.books.size() + 16));

  WalkState state{index, fresh, options, {}, 0, 0, false};

  if (!options.root.empty()) walkDirectory(state, options.root, 0, true, options.rootDepth);
  // Same directory twice (root == booksRoot) would double-list every book.
  if (!options.booksRoot.empty() && options.booksRoot != options.root) {
    walkDirectory(state, options.booksRoot, 0, true, options.maxDepth);
  }
  // Books downloaded through OPDS land in the configured download folder; it is
  // walked recursively so periodicals/subfolders added by OPDS clients still
  // show up. Skipped when it coincides with a root already walked.
  if (!options.downloadsRoot.empty() && options.downloadsRoot != options.root &&
      options.downloadsRoot != options.booksRoot) {
    walkDirectory(state, options.downloadsRoot, 0, true, options.maxDepth);
  }

  if (state.openedDirs == 0) {
    // Card went away mid-scan: keep the last known library instead of wiping it.
    LOG_ERR("SHELF", "Library roots unreadable, index left untouched");
    return state.stats;
  }

  state.stats.removed = index.books.size() > state.matchedOld ? index.books.size() - state.matchedOld : 0;
  state.stats.truncated = state.truncated;

  if (removed != nullptr && !index.books.empty()) {
    // Report every book that was in the old index but no longer on the card, so
    // the caller can drop its caches. Linear in books-per-scan (target ~100).
    for (const auto& previous : index.books) {
      bool stillPresent = false;
      for (const auto& book : fresh) {
        if (book.path == previous.path) {
          stillPresent = true;
          break;
        }
      }
      if (!stillPresent) removed(removedCtx, previous.path);
    }
  }

  index.books = std::move(fresh);
  return state.stats;
}

}  // namespace shelf
