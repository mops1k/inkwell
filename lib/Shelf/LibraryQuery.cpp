#include "LibraryQuery.h"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace shelf {

namespace {

constexpr FilterField kFields[] = {FilterField::Author, FilterField::Series, FilterField::Genre};
constexpr SortKey kSortKeys[] = {SortKey::Title, SortKey::Author};

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
  const std::string& value = fieldValue(book.meta, field);
  return !value.empty() && value == this->value;
}

void LibraryQuery::apply(const ShelfIndex& index, std::vector<uint16_t>& out) const {
  out.clear();
  out.reserve(index.books.size());
  for (size_t i = 0; i < index.books.size(); ++i) {
    if (!matches(index.books[i])) continue;
    // The grid addresses cells by uint16_t page offsets; a library larger than
    // that cannot be shown in one pass anyway (plan budget is ~100 books).
    if (i > UINT16_MAX) break;
    out.push_back(static_cast<uint16_t>(i));
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

void collectValues(const ShelfIndex& index, FilterField field, std::vector<std::string>& out, size_t cap) {
  out.clear();
  if (field == FilterField::None) return;
  for (const auto& book : index.books) {
    const std::string& value = fieldValue(book.meta, field);
    if (value.empty()) continue;
    if (std::find(out.begin(), out.end(), value) != out.end()) continue;
    if (out.size() >= cap) break;
    out.push_back(value);
  }
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return compareFold(a, b) < 0; });
}

}  // namespace shelf
