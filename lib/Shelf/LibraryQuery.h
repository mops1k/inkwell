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
  // Reading status is not metadata: it is derived from the reading statistics
  // (progress), so every collector for this field takes the statistics snapshot.
  ReadingStatus,
};

const char* filterFieldName(FilterField field);  // stable JSON name, never UI text
FilterField filterFieldFromName(std::string_view name);

enum class SortKey : uint8_t {
  Title = 0,
  Author,
  Series,
  LastOpened,
  Progress,
  Size,
  Added,  // date added to the card (= file modification time, FAT timestamp)
  Year,   // publication year the book declares
};

const char* sortKeyName(SortKey key);
SortKey sortKeyFromName(std::string_view name);

// How many keys the shelf offers; also the count the picker lists.
constexpr size_t kSortKeyCount = 8;
const SortKey* sortKeys();  // display order of the picker

// The direction that makes a key read naturally when the user has not chosen
// one yet: "недавние" and "размер" make sense newest/largest first, while a
// title or an author reads A→Z. Selecting a field in the picker pre-selects
// this, so the second step never has to be guessed.
bool naturalDescending(SortKey key);

// One book's reading statistics, index-aligned with ShelfIndex::books. The
// library keeps statistics in its own store; passing them in keeps the query
// free of I/O and testable without a card.
struct SortStat {
  uint32_t lastOpenedEpoch = 0;  // 0 = never opened
  uint8_t progressPercent = 0;   // 0 = unread
};

// Reading status derived from those statistics: the three buckets the library
// filters by. Stored as stable names (never UI text) in the filter value.
enum class ReadingStatus : uint8_t { Unread = 0, Reading, Finished };

const char* readingStatusName(ReadingStatus status);
ReadingStatus readingStatusFromName(std::string_view name);
ReadingStatus readingStatusOf(const SortStat& stat);

// A single active filter. `value` is one of the values returned by
// collectValues() for `field`, so matching is exact string equality.
struct LibraryFilter {
  FilterField field = FilterField::None;
  std::string value;

  bool active() const { return field != FilterField::None && !value.empty(); }
  // `stat` is required for the reading-status field and ignored by the others:
  // metadata filters need no history, a status filter has nothing else.
  bool matches(const ShelfBook& book, const SortStat* stat = nullptr) const;
  void clear() {
    field = FilterField::None;
    value.clear();
  }
};

// A filter value and the number of visible books behind it (author, series, genre…).
struct FilterValueCount {
  std::string value;
  int count = 0;
};

// Everything the shelf screen needs to turn the index into visible cells.
struct LibraryQuery {
  LibraryFilter filter;
  SortKey sort = SortKey::Title;
  // Reverse order. Applied to the primary key only: a book with no series/year
  // stays at the end in both directions, because "unknown" is not a value that
  // ordering can place meaningfully at the front.
  bool descending = false;
  // Books the user hid from the library. Paths, because a hidden book keeps its
  // index entry (a rescan must not resurrect it) yet never appears in the grid,
  // the filter value lists or the series walk.
  std::vector<std::string> hiddenPaths;

  bool isHidden(const ShelfBook& book) const;
  bool matches(const ShelfBook& book, const SortStat* stat = nullptr) const {
    return filter.matches(book, stat) && !isHidden(book);
  }

  // Fills `out` with indices into `index.books`, filtered and ordered. The
  // caller keeps the index alive; the shelf passes the result to the grid.
  // `stats` is needed by the time- and progress-ordered keys and by the reading
  // status filter: one entry per index book, nullptr when the shelf has no
  // statistics to offer (tests, export).
  void apply(const ShelfIndex& index, std::vector<uint16_t>& out,
             const std::vector<SortStat>* stats = nullptr) const;

  // Distinct values of `field` present in the visible library, deduplicated and
  // sorted case-insensitively; books with an empty field or hidden from the
  // library are skipped. `cap` bounds the list so a pathological library cannot
  // grow a picker past RAM.
  void collectValues(const ShelfIndex& index, FilterField field, std::vector<std::string>& out, size_t cap = 120,
                     const std::vector<SortStat>* stats = nullptr) const;

  // Distinct values of `field` together with how many visible books carry each one.
  // The browse screens need the counter next to the name ("Кликин (2)"), the same way
  // the Android library shows category rows, and computing it here keeps the counting
  // rule in one place instead of per screen.
  void collectValueCounts(const ShelfIndex& index, FilterField field, std::vector<FilterValueCount>& out,
                          size_t cap = 200, const std::vector<SortStat>* stats = nullptr) const;

  // How many visible books (hidden excluded, no filter applied) carry a non-empty
  // value for `field`: the number the browse row for that field shows.
  int countWith(const ShelfIndex& index, FilterField field, const std::vector<SortStat>* stats = nullptr) const;
};

// Convenience overload for callers with no hidden books (tests, exporters).
void collectValues(const ShelfIndex& index, FilterField field, std::vector<std::string>& out, size_t cap = 120);

// Fills `out` with indices into `index.books` of the books that belong to
// `series`, in reading order: the decimal <sequence number> first (books without
// a usable number go last), ties broken by title. Used by the book card so
// Left/Right walk the series instead of leaving the card.
void collectSeriesBooks(const ShelfIndex& index, const std::string& series, std::vector<uint16_t>& out);

}  // namespace shelf
