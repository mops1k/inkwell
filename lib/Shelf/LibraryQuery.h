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
};

const char* filterFieldName(FilterField field);  // stable JSON name, never UI text
FilterField filterFieldFromName(std::string_view name);

enum class SortKey : uint8_t {
  Title = 0,
  Author,
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

  bool matches(const ShelfBook& book) const { return filter.matches(book); }

  // Fills `out` with indices into `index.books`, filtered and ordered. The
  // caller keeps the index alive; the shelf passes the result to the grid.
  void apply(const ShelfIndex& index, std::vector<uint16_t>& out) const;
};

// Distinct values of `field` present in the index, deduplicated and sorted
// case-insensitively; books with an empty field are skipped. `cap` bounds the
// list so a pathological library cannot grow a picker past RAM.
void collectValues(const ShelfIndex& index, FilterField field, std::vector<std::string>& out, size_t cap = 120);

}  // namespace shelf
