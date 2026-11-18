#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ShelfIndex.h"

namespace shelf {

// Which metadata field the shelf narrows down to. "All books" is FilterField::None:
// the library always starts from the whole index and at most one filter is active —
// a cut-down BookShelf-style library, not a multi-criteria query builder.
enum class FilterField : uint8_t {
  None = 0,
  Author,
  Series,
  Genre,
  Format,
};

const char* filterFieldName(FilterField field);  // stable JSON name, never UI text
FilterField filterFieldFromName(std::string_view name);

enum class SortKey : uint8_t {
  Title = 0,
  Author,
  LastOpened,
};

const char* sortKeyName(SortKey key);
SortKey sortKeyFromName(std::string_view name);

// A single active filter. `value` is one of the values returned by
// collectValues() for `field`, so matching is exact string equality.
struct LibraryFilter {
  FilterField field = FilterField::None;
  std::string value;

  bool active() const { return field != FilterField::None && !value.empty(); }
  bool matches(const ShelfBook& book) const;
  void clear() {
    field = FilterField::None;
    value.clear();
  }
};

// Everything the shelf screen needs to turn the index into visible cells.
struct LibraryQuery {
  LibraryFilter filter;
  SortKey sort = SortKey::Title;
  // Books the user hid from the library. Paths, because a hidden book keeps its
  // index entry (a rescan must not resurrect it) yet never appears in the grid,
  // the filter value lists or the series walk.
  std::vector<std::string> hiddenPaths;

  bool isHidden(const ShelfBook& book) const;
  bool matches(const ShelfBook& book) const { return filter.matches(book) && !isHidden(book); }

  // Fills `out` with indices into `index.books`, filtered and ordered. The
  // caller keeps the index alive; the shelf passes the result to the grid.
  // `lastOpenedEpochs` is only needed for SortKey::LastOpened: one entry per
  // index book, 0 = never opened (those sort after everything opened).
  void apply(const ShelfIndex& index, std::vector<uint16_t>& out,
             const std::vector<uint32_t>* lastOpenedEpochs = nullptr) const;

  // Distinct values of `field` present in the visible library, deduplicated and
  // sorted case-insensitively; books with an empty field or hidden from the
  // library are skipped. `cap` bounds the list so a pathological library cannot
  // grow a picker past RAM.
  void collectValues(const ShelfIndex& index, FilterField field, std::vector<std::string>& out, size_t cap = 120) const;
};

// Convenience overload for callers with no hidden books (tests, exporters).
void collectValues(const ShelfIndex& index, FilterField field, std::vector<std::string>& out, size_t cap = 120);

// Fills `out` with indices into `index.books` of the books that belong to
// `series`, in reading order: the decimal <sequence number> first (books without
// a usable number go last), ties broken by title. Used by the book card so
// Left/Right walk the series instead of leaving the card.
void collectSeriesBooks(const ShelfIndex& index, const std::string& series, std::vector<uint16_t>& out);

}  // namespace shelf
