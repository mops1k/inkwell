#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace shelf {

// Formats the shelf can index. Detection lives in BookFormat.h — never infer
// the type from a filename alone when the file is an archive (see AGENTS.md).
enum class BookFormat : uint8_t {
  Unknown = 0,
  Epub,
  Fb2,
  Fb2Zip,
  Txt,
  Markdown,
  Xtc,
};

// Stable short name used by the on-SD index JSON. Never shown as UI text.
const char* bookFormatName(BookFormat format);
BookFormat bookFormatFromName(std::string_view name);

// Metadata shown on the shelf. Fields are optional: every supported container
// can lack some of them (FB2 without <lang>, EPUB without <dc:series>, …).
struct BookMeta {
  std::string title;
  std::string author;
  std::string series;
  std::string seriesIndex;
  std::string language;
  std::string genre;  // first genre/subject; drives the shelf's genre filter
};

// One indexed file. `path` is the absolute SD path and stays the book identity
// (same convention as the reader and KOReader progress keys).
struct ShelfBook {
  std::string path;
  uint32_t size = 0;
  // FAT timestamp as (date << 16) | time — the same packing FileIndex uses, so
  // one 16-bit pair per file is enough to detect a change.
  uint32_t fatDateTime = 0;
  BookFormat format = BookFormat::Unknown;
  BookMeta meta;
  // False when the container could not be parsed and title fell back to the
  // filename. The book still stays on the shelf — a broken file must not
  // silently disappear from the user's library.
  bool metaOk = false;

  // Cheap change detection: size AND timestamp, because FAT timestamps have
  // two-second granularity and a same-size rewrite within that window is real.
  bool sameFileSignature(const ShelfBook& other) const {
    return size == other.size && fatDateTime == other.fatDateTime;
  }
};

// Target library is ~100 books; the cap keeps a runaway folder tree from
// exhausting the C3 heap (see plan Ф1, AGENTS.md "целевая библиотека ~100 книг").
constexpr size_t kMaxIndexBooks = 1000;
constexpr uint8_t kMaxLibraryDepth = 5;

}  // namespace shelf
