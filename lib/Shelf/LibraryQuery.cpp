#include "LibraryQuery.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <utility>

namespace shelf {

namespace {

constexpr FilterField kFields[] = {FilterField::Author, FilterField::Series, FilterField::Genre, FilterField::Format};

std::string formatValue(const ShelfBook& book) { return bookFormatLabel(book.format); }
constexpr SortKey kSortKeys[] = {SortKey::Title, SortKey::Author, SortKey::LastOpened};

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
    case FilterField::None:
      break;
  }
  return "none";
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
    case SortKey::LastOpened:
      return "lastOpened";
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

bool LibraryFilter::matches(const ShelfBook& book) const {
  if (!active()) return true;
  // Format is not part of BookMeta: it is the detected container type, and its
  // label is the same string the cover chip shows.
  if (field == FilterField::Format) {
    const std::string format = formatValue(book);
    return !format.empty() && format == value;
  }
  const std::string& fieldText = fieldValue(book.meta, field);
  return !fieldText.empty() && fieldText == value;
}

bool LibraryQuery::isHidden(const ShelfBook& book) const {
  return std::find(hiddenPaths.begin(), hiddenPaths.end(), book.path) != hiddenPaths.end();
}

void LibraryQuery::apply(const ShelfIndex& index, std::vector<uint16_t>& out,
                         const std::vector<uint32_t>* lastOpenedEpochs) const {
  out.clear();
  out.reserve(index.books.size());
  for (size_t i = 0; i < index.books.size(); ++i) {
    if (!matches(index.books[i])) continue;
    // The grid addresses cells by uint16_t page offsets; a library larger than
    // that cannot be shown in one pass anyway (plan budget is ~100 books).
    if (i > UINT16_MAX) break;
    out.push_back(static_cast<uint16_t>(i));
  }

  const auto epochOf = [lastOpenedEpochs](uint16_t bookIndex) -> uint32_t {
    if (lastOpenedEpochs == nullptr || bookIndex >= lastOpenedEpochs->size()) return 0;
    return (*lastOpenedEpochs)[bookIndex];
  };

  if (sort == SortKey::LastOpened) {
    // Newest first; books that were never opened keep their title order at the
    // end instead of jumping between opened ones.
    std::stable_sort(out.begin(), out.end(), [&](uint16_t a, uint16_t b) {
      const uint32_t ea = epochOf(a);
      const uint32_t eb = epochOf(b);
      if (ea != eb) {
        if (ea == 0) return false;
        if (eb == 0) return true;
        return ea > eb;
      }
      const int byTitle = compareFold(index.books[a].meta.title, index.books[b].meta.title);
      if (byTitle != 0) return byTitle < 0;
      return index.books[a].path < index.books[b].path;
    });
    return;
  }

  const auto keyOf = [this, &index](uint16_t bookIndex) -> const std::string& {
    const BookMeta& meta = index.books[bookIndex].meta;
    if (sort == SortKey::Author && !meta.author.empty()) return meta.author;
    return meta.title;
  };
  std::stable_sort(out.begin(), out.end(), [&](uint16_t a, uint16_t b) {
    const int primary = compareFold(keyOf(a), keyOf(b));
    if (primary != 0) return primary < 0;
    const int byTitle = compareFold(index.books[a].meta.title, index.books[b].meta.title);
    if (byTitle != 0) return byTitle < 0;
    return index.books[a].path < index.books[b].path;
  });
}

void LibraryQuery::collectValues(const ShelfIndex& index, FilterField field, std::vector<std::string>& out,
                                 size_t cap) const {
  out.clear();
  if (field == FilterField::None) return;
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

void collectValues(const ShelfIndex& index, FilterField field, std::vector<std::string>& out, size_t cap) {
  LibraryQuery{}.collectValues(index, field, out, cap);
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
