#include "LibraryQuery.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

namespace shelf {

namespace {

constexpr FilterField kFields[] = {FilterField::Author, FilterField::Series, FilterField::Genre, FilterField::Format,
                                   FilterField::ReadingStatus};

std::string formatValue(const ShelfBook& book) { return bookFormatLabel(book.format); }

// Display order of the sort picker: the two keys the shelf started with, then
// the ones the parity work added. Kept here rather than in the activity so the
// name table and the picker can never list different sets.
constexpr SortKey kSortKeys[] = {SortKey::Title,  SortKey::Author,   SortKey::Series, SortKey::LastOpened,
                                 SortKey::Progress, SortKey::Size, SortKey::Added,  SortKey::Year};
static_assert(sizeof(kSortKeys) / sizeof(kSortKeys[0]) == kSortKeyCount, "sort key table must list every key");

// A four-digit publication year; anything else is "unknown" and sorts last.
bool parseYear(const std::string& text, int64_t& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || value <= 0) return false;
  out = value;
  return true;
}

// Series position ("2", "2.5", "II" → unknown) inside one series.
bool parseSeriesIndex(const std::string& text, double& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str()) return false;
  out = value;
  return true;
}

const std::string& fieldValue(const BookMeta& meta, FilterField field) {
  static const std::string empty;
  switch (field) {
    case FilterField::Author:
      return meta.author;
    case FilterField::Series:
      return meta.series;
    case FilterField::Genre:
      return meta.genre;
    case FilterField::None:
      break;
  }
  return empty;
}

// ASCII-only fold: metadata is UTF-8, and case-insensitive ordering only has
// to be stable for Latin titles; Cyrillic has one case for the leading letter
// in practice, so a full Unicode table would cost flash for no visible gain.
int foldChar(unsigned char c) { return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }

int compareFold(std::string_view a, std::string_view b) {
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    const int ca = foldChar(static_cast<unsigned char>(a[i]));
    const int cb = foldChar(static_cast<unsigned char>(b[i]));
    if (ca != cb) return ca < cb ? -1 : 1;
  }
  if (a.size() == b.size()) return 0;
  return a.size() < b.size() ? -1 : 1;
}

}  // namespace

const char* filterFieldName(FilterField field) {
  switch (field) {
    case FilterField::Author:
      return "author";
    case FilterField::Series:
      return "series";
    case FilterField::Genre:
      return "genre";
    case FilterField::Format:
      return "format";
    case FilterField::ReadingStatus:
      return "readingStatus";
    case FilterField::None:
      break;
  }
  return "none";
}

const char* readingStatusName(ReadingStatus status) {
  switch (status) {
    case ReadingStatus::Unread:
      return "unread";
    case ReadingStatus::Reading:
      return "reading";
    case ReadingStatus::Finished:
      return "finished";
  }
  return "unread";
}

ReadingStatus readingStatusFromName(std::string_view name) {
  for (const ReadingStatus status : {ReadingStatus::Unread, ReadingStatus::Reading, ReadingStatus::Finished}) {
    if (name == readingStatusName(status)) return status;
  }
  return ReadingStatus::Unread;
}

ReadingStatus readingStatusOf(const SortStat& stat) {
  // 100 % means finished, anything above zero means started. A book that was
  // opened but is still at 0 % counts as unread: "непрочитанные" is about the
  // text, not about the fact that the card was viewed once.
  if (stat.progressPercent >= 100) return ReadingStatus::Finished;
  if (stat.progressPercent > 0) return ReadingStatus::Reading;
  return ReadingStatus::Unread;
}

FilterField filterFieldFromName(std::string_view name) {
  for (const FilterField field : kFields) {
    if (name == filterFieldName(field)) return field;
  }
  return FilterField::None;
}

const char* sortKeyName(SortKey key) {
  switch (key) {
    case SortKey::Author:
      return "author";
    case SortKey::Series:
      return "series";
    case SortKey::LastOpened:
      return "lastOpened";
    case SortKey::Progress:
      return "progress";
    case SortKey::Size:
      return "size";
    case SortKey::Added:
      return "added";
    case SortKey::Year:
      return "year";
    case SortKey::Title:
      break;
  }
  return "title";
}

SortKey sortKeyFromName(std::string_view name) {
  for (const SortKey key : kSortKeys) {
    if (name == sortKeyName(key)) return key;
  }
  return SortKey::Title;
}

const SortKey* sortKeys() { return kSortKeys; }

bool naturalDescending(SortKey key) {
  switch (key) {
    case SortKey::LastOpened:
    case SortKey::Progress:
    case SortKey::Size:
    case SortKey::Added:
    case SortKey::Year:
      // "what I opened last", "biggest", "added recently", "newest book": the
      // useful end of these scales is the far one, so the picker starts there.
      return true;
    case SortKey::Title:
    case SortKey::Author:
    case SortKey::Series:
      return false;
  }
  return false;
}

bool LibraryFilter::matches(const ShelfBook& book, const SortStat* stat) const {
  if (!active()) return true;
  // Format is not part of BookMeta: it is the detected container type, and its
  // label is the same string the cover chip shows.
  if (field == FilterField::Format) {
    const std::string format = formatValue(book);
    return !format.empty() && format == value;
  }
  if (field == FilterField::ReadingStatus) {
    // No history to ask -> the book cannot be in any status bucket, so a status
    // filter without statistics shows nothing rather than everything.
    if (stat == nullptr) return false;
    return value == readingStatusName(readingStatusOf(*stat));
  }
  const std::string& fieldText = fieldValue(book.meta, field);
  return !fieldText.empty() && fieldText == value;
}

bool LibraryQuery::isHidden(const ShelfBook& book) const {
  return std::find(hiddenPaths.begin(), hiddenPaths.end(), book.path) != hiddenPaths.end();
}

void LibraryQuery::apply(const ShelfIndex& index, std::vector<uint16_t>& out,
                         const std::vector<SortStat>* stats) const {
  out.clear();
  out.reserve(index.books.size());
  const auto statAt = [stats](size_t bookIndex) -> const SortStat* {
    if (stats == nullptr || bookIndex >= stats->size()) return nullptr;
    return &(*stats)[bookIndex];
  };
  for (size_t i = 0; i < index.books.size(); ++i) {
    if (!matches(index.books[i], statAt(i))) continue;
    // The grid addresses cells by uint16_t page offsets; a library larger than
    // that cannot be shown in one pass anyway (plan budget is ~100 books).
    if (i > UINT16_MAX) break;
    out.push_back(static_cast<uint16_t>(i));
  }

  const auto statOf = [stats](uint16_t bookIndex) -> SortStat {
    if (stats == nullptr || bookIndex >= stats->size()) return SortStat{};
    return (*stats)[bookIndex];
  };

  // The key is decoded once per visible book: the comparator then compares only
  // numbers and string views, which keeps a full-library sort cheap on the C3
  // and stops the per-key rules (series number, unknown last) from being spread
  // across two passes.
  struct Keyed {
    uint16_t index = 0;
    std::string_view text;
    int64_t number = 0;
    double sub = 0.0;
    bool unknown = false;
  };
  std::vector<Keyed> rows;
  rows.reserve(out.size());
  for (const uint16_t bookIndex : out) {
    const ShelfBook& book = index.books[bookIndex];
    Keyed row;
    row.index = bookIndex;
    switch (sort) {
      case SortKey::Title:
        row.text = book.meta.title;
        break;
      case SortKey::Author:
        // A book without an author sorts by its title, so it stays in one
        // alphabetical run instead of clumping at the top of the shelf.
        row.text = book.meta.author.empty() ? std::string_view(book.meta.title) : std::string_view(book.meta.author);
        break;
      case SortKey::Series:
        row.text = book.meta.series;
        row.unknown = book.meta.series.empty();
        if (!row.unknown) parseSeriesIndex(book.meta.seriesIndex, row.sub);
        break;
      case SortKey::LastOpened: {
        const uint32_t epoch = statOf(bookIndex).lastOpenedEpoch;
        row.unknown = epoch == 0;  // never opened: not "oldest", just unknown
        row.number = epoch;
        break;
      }
      case SortKey::Progress:
        row.number = statOf(bookIndex).progressPercent;
        break;
      case SortKey::Size:
        row.number = book.size;
        break;
      case SortKey::Added:
        row.number = book.fatDateTime;
        break;
      case SortKey::Year:
        row.unknown = !parseYear(book.meta.year, row.number);
        break;
    }
    rows.push_back(row);
  }

  const auto byTitleThenPath = [&index](uint16_t a, uint16_t b) {
    const int cmp = compareFold(index.books[a].meta.title, index.books[b].meta.title);
    if (cmp != 0) return cmp < 0;
    return index.books[a].path < index.books[b].path;
  };
  std::stable_sort(rows.begin(), rows.end(), [&](const Keyed& a, const Keyed& b) {
    // Unknown values stay last in both directions: "no series" is not a name
    // that ordering can meaningfully put first.
    if (a.unknown != b.unknown) return b.unknown;
    if (!a.unknown) {
      int cmp = 0;
      if (sort == SortKey::Title || sort == SortKey::Author) {
        cmp = compareFold(a.text, b.text);
      } else if (sort == SortKey::Series) {
        cmp = compareFold(a.text, b.text);
        if (cmp == 0 && a.sub != b.sub) cmp = a.sub < b.sub ? -1 : 1;
      } else if (a.number != b.number) {
        cmp = a.number < b.number ? -1 : 1;
      }
      if (cmp != 0) return descending ? cmp > 0 : cmp < 0;
    }
    return byTitleThenPath(a.index, b.index);
  });

  out.clear();
  out.reserve(rows.size());
  for (const Keyed& row : rows) out.push_back(row.index);
}

void LibraryQuery::collectValues(const ShelfIndex& index, FilterField field, std::vector<std::string>& out, size_t cap,
                                 const std::vector<SortStat>* stats) const {
  out.clear();
  if (field == FilterField::None) return;
  if (field == FilterField::ReadingStatus) {
    // The three buckets always exist, even when no book is in one of them: the
    // picker must offer "прочитанные" before the first book is finished.
    (void)index;
    (void)stats;
    for (const ReadingStatus status : {ReadingStatus::Unread, ReadingStatus::Reading, ReadingStatus::Finished}) {
      if (out.size() >= cap) break;
      out.emplace_back(readingStatusName(status));
    }
    return;
  }
  for (const auto& book : index.books) {
    if (isHidden(book)) continue;
    const std::string value = field == FilterField::Format ? formatValue(book) : fieldValue(book.meta, field);
    if (value.empty()) continue;
    if (std::find(out.begin(), out.end(), value) != out.end()) continue;
    if (out.size() >= cap) break;
    out.push_back(value);
  }
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return compareFold(a, b) < 0; });
}

void LibraryQuery::collectValueCounts(const ShelfIndex& index, FilterField field,
                                      std::vector<FilterValueCount>& out, size_t cap,
                                      const std::vector<SortStat>* stats) const {
  out.clear();
  if (field == FilterField::None) return;
  if (field == FilterField::ReadingStatus) {
    // Statistics are the only source of a status, so without them every bucket
    // still exists but counts zero books — the picker stays truthful about that.
    for (const ReadingStatus status : {ReadingStatus::Unread, ReadingStatus::Reading, ReadingStatus::Finished}) {
      if (out.size() >= cap) break;
      out.push_back(FilterValueCount{readingStatusName(status), 0});
    }
    if (stats != nullptr) {
      for (size_t i = 0; i < index.books.size() && i < stats->size(); ++i) {
        if (isHidden(index.books[i])) continue;
        const char* name = readingStatusName(readingStatusOf((*stats)[i]));
        for (auto& entry : out) {
          if (entry.value == name) {
            ++entry.count;
            break;
          }
        }
      }
    }
    return;
  }
  for (const auto& book : index.books) {
    if (isHidden(book)) continue;
    const std::string value = field == FilterField::Format ? formatValue(book) : fieldValue(book.meta, field);
    if (value.empty()) continue;
    const auto existing = std::find_if(out.begin(), out.end(),
                                       [&value](const FilterValueCount& entry) { return entry.value == value; });
    if (existing != out.end()) {
      ++existing->count;
      continue;
    }
    if (out.size() >= cap) continue;
    out.push_back(FilterValueCount{value, 1});
  }
  std::sort(out.begin(), out.end(),
            [](const FilterValueCount& a, const FilterValueCount& b) { return compareFold(a.value, b.value) < 0; });
}

void collectValues(const ShelfIndex& index, FilterField field, std::vector<std::string>& out, size_t cap) {
  LibraryQuery{}.collectValues(index, field, out, cap);
}

int LibraryQuery::countWith(const ShelfIndex& index, FilterField field, const std::vector<SortStat>* stats) const {
  if (field == FilterField::None) return 0;
  (void)stats;
  int count = 0;
  for (const auto& book : index.books) {
    if (isHidden(book)) continue;
    // Every visible book has a reading status, so that dimension counts the
    // whole visible library; the metadata dimensions count non-empty values.
    if (field == FilterField::ReadingStatus) {
      ++count;
      continue;
    }
    const std::string value = field == FilterField::Format ? formatValue(book) : fieldValue(book.meta, field);
    if (!value.empty()) ++count;
  }
  return count;
}

void collectSeriesBooks(const ShelfIndex& index, const std::string& series, std::vector<uint16_t>& out) {
  out.clear();
  if (series.empty()) return;
  std::vector<std::pair<double, uint16_t>> ordered;
  std::vector<uint16_t> unnumbered;
  for (uint16_t i = 0; i < index.books.size(); ++i) {
    const shelf::ShelfBook& book = index.books[i];
    if (book.meta.series != series) continue;
    if (!book.meta.seriesIndex.empty()) {
      const char* raw = book.meta.seriesIndex.c_str();
      char* end = nullptr;
      const double number = std::strtod(raw, &end);
      if (end != raw) {
        ordered.emplace_back(number, i);
        continue;
      }
    }
    unnumbered.push_back(i);
  }
  const auto byTitle = [&index](uint16_t a, uint16_t b) {
    return compareFold(index.books[a].meta.title, index.books[b].meta.title) < 0;
  };
  std::stable_sort(ordered.begin(), ordered.end(), [&byTitle](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first < b.first;
    return byTitle(a.second, b.second);
  });
  std::stable_sort(unnumbered.begin(), unnumbered.end(), byTitle);
  for (const auto& entry : ordered) out.push_back(entry.second);
  out.insert(out.end(), unnumbered.begin(), unnumbered.end());
}

}  // namespace shelf
