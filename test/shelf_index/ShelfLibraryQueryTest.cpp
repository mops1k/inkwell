#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "LibraryQuery.h"

namespace {

shelf::ShelfBook makeBook(const char* path, const char* title, const char* author, const char* series,
                          const char* genre) {
  shelf::ShelfBook book;
  book.path = path;
  book.meta.title = title;
  book.meta.author = author;
  book.meta.series = series;
  book.meta.genre = genre;
  book.metaOk = true;
  return book;
}

shelf::ShelfIndex makeIndex() {
  shelf::ShelfIndex index;
  index.books.push_back(
      makeBook("/books/b.epub", "Братья Карамазовы", "Достоевский", "Русская классика", "prose_classic"));
  index.books.push_back(makeBook("/books/a.epub", "Анна Каренина", "Толстой", "Русская классика", "prose_classic"));
  index.books.push_back(makeBook("/books/c.epub", "Дюна", "Герберт", "Хроники Дюны", "sf"));
  index.books.push_back(makeBook("/books/d.txt", "Заметки", "", "", ""));
  return index;
}

}  // namespace

TEST(LibraryQuery, EmptyFilterMatchesEveryBook) {
  const shelf::ShelfIndex index = makeIndex();
  shelf::LibraryQuery query;
  EXPECT_FALSE(query.filter.active());

  std::vector<uint16_t> out;
  query.apply(index, out);
  EXPECT_EQ(index.books.size(), out.size());
}

TEST(LibraryQuery, FiltersByAuthorSeriesAndGenre) {
  const shelf::ShelfIndex index = makeIndex();

  shelf::LibraryQuery byAuthor;
  byAuthor.filter = {shelf::FilterField::Author, "Толстой"};
  std::vector<uint16_t> out;
  byAuthor.apply(index, out);
  ASSERT_EQ(1u, out.size());
  EXPECT_EQ("Анна Каренина", index.books[out[0]].meta.title);

  shelf::LibraryQuery bySeries;
  bySeries.filter = {shelf::FilterField::Series, "Русская классика"};
  bySeries.apply(index, out);
  ASSERT_EQ(2u, out.size());

  shelf::LibraryQuery byGenre;
  byGenre.filter = {shelf::FilterField::Genre, "sf"};
  byGenre.apply(index, out);
  ASSERT_EQ(1u, out.size());
  EXPECT_EQ("Дюна", index.books[out[0]].meta.title);

  // A value nobody has yields an empty library, not the whole one.
  shelf::LibraryQuery unknown;
  unknown.filter = {shelf::FilterField::Genre, "poetry"};
  unknown.apply(index, out);
  EXPECT_TRUE(out.empty());
}

TEST(LibraryQuery, SortsByTitleAndAuthor) {
  const shelf::ShelfIndex index = makeIndex();
  std::vector<uint16_t> out;

  shelf::LibraryQuery byTitle;
  byTitle.sort = shelf::SortKey::Title;
  byTitle.apply(index, out);

  // Author sort falls back to the title for books without an author, so
  // "Заметки" (no author) lands by its own title between "Достоевский" and
  // "Толстой" — the empty author never collapses to the front or the end.
  shelf::LibraryQuery byAuthor;
  byAuthor.sort = shelf::SortKey::Author;
  byAuthor.apply(index, out);
  ASSERT_EQ(index.books.size(), out.size());
  EXPECT_EQ("Дюна", index.books[out[0]].meta.title);               // Герберт
  EXPECT_EQ("Братья Карамазовы", index.books[out[1]].meta.title);  // Достоевский
  EXPECT_EQ("Заметки", index.books[out[2]].meta.title);            // no author -> title
  EXPECT_EQ("Анна Каренина", index.books[out[3]].meta.title);      // Толстой
}

TEST(LibraryQuery, CollectsDistinctValues) {
  const shelf::ShelfIndex index = makeIndex();
  std::vector<std::string> values;

  shelf::collectValues(index, shelf::FilterField::Series, values);
  ASSERT_EQ(2u, values.size());
  EXPECT_EQ("Русская классика", values[0]);
  EXPECT_EQ("Хроники Дюны", values[1]);

  shelf::collectValues(index, shelf::FilterField::Author, values);
  ASSERT_EQ(3u, values.size());  // the book without an author is skipped

  shelf::collectValues(index, shelf::FilterField::Genre, values, 1);
  EXPECT_EQ(1u, values.size());

  shelf::collectValues(index, shelf::FilterField::None, values);
  EXPECT_TRUE(values.empty());
}

TEST(LibraryQuery, CountsBooksBehindEachValue) {
  const shelf::ShelfIndex index = makeIndex();
  shelf::LibraryQuery query;

  std::vector<shelf::FilterValueCount> authors;
  query.collectValueCounts(index, shelf::FilterField::Author, authors);
  ASSERT_EQ(authors.size(), 3u);
  EXPECT_EQ(authors[0].value, "Герберт");
  EXPECT_EQ(authors[0].count, 1);

  std::vector<shelf::FilterValueCount> series;
  query.collectValueCounts(index, shelf::FilterField::Series, series);
  ASSERT_EQ(series.size(), 2u);
  for (const auto& entry : series) {
    EXPECT_EQ(entry.count, entry.value == "Русская классика" ? 2 : 1);
  }

  // Hidden books never contribute to the counters, exactly like the value lists.
  shelf::LibraryQuery hiding;
  hiding.hiddenPaths = {"/books/a.epub"};
  std::vector<shelf::FilterValueCount> hiddenSeries;
  hiding.collectValueCounts(index, shelf::FilterField::Series, hiddenSeries);
  for (const auto& entry : hiddenSeries) {
    if (entry.value == "Русская классика") EXPECT_EQ(entry.count, 1);
  }
}

TEST(LibraryQuery, SortsByLastOpened) {
  const shelf::ShelfIndex index = makeIndex();
  std::vector<uint16_t> out;

  // index order: 0 Братья, 1 Анна, 2 Дюна, 3 Заметки. 0 = never opened.
  std::vector<shelf::SortStat> stats(index.books.size());
  stats[0].lastOpenedEpoch = 1000;
  stats[2].lastOpenedEpoch = 3000;
  stats[1].lastOpenedEpoch = 2000;

  // The direction is the user's choice; "recently opened" naturally starts at
  // newest-first, which is what the picker pre-selects (naturalDescending).
  shelf::LibraryQuery byRecent;
  byRecent.sort = shelf::SortKey::LastOpened;
  byRecent.descending = true;
  byRecent.apply(index, out, &stats);
  ASSERT_EQ(index.books.size(), out.size());
  EXPECT_EQ("Дюна", index.books[out[0]].meta.title);  // newest first
  EXPECT_EQ("Анна Каренина", index.books[out[1]].meta.title);
  EXPECT_EQ("Братья Карамазовы", index.books[out[2]].meta.title);
  EXPECT_EQ("Заметки", index.books[out[3]].meta.title);  // never opened goes last

  // The opposite direction is a real order, not the same list twice — and books
  // that were never opened still stay at the end.
  byRecent.descending = false;
  byRecent.apply(index, out, &stats);
  EXPECT_EQ("Братья Карамазовы", index.books[out[0]].meta.title);
  EXPECT_EQ("Заметки", index.books[out[3]].meta.title);

  // Without statistics the sort degrades to title order, never to garbage.
  byRecent.apply(index, out, nullptr);
  ASSERT_EQ(index.books.size(), out.size());
  EXPECT_EQ("Анна Каренина", index.books[out[0]].meta.title);
}

// Hidden books leave the grid, the filter values and the series walk, but keep
// their index entry so unhiding needs no rescan.
TEST(LibraryQuery, HiddenBooksAreExcluded) {
  shelf::ShelfIndex index = makeIndex();
  shelf::LibraryQuery query;
  query.hiddenPaths = {"/books/b.epub"};

  std::vector<uint16_t> out;
  query.apply(index, out);
  ASSERT_EQ(out.size(), index.books.size() - 1);
  for (const uint16_t id : out) {
    EXPECT_NE(index.books[id].path, "/books/b.epub");
  }

  std::vector<std::string> values;
  query.collectValues(index, shelf::FilterField::Author, values);
  EXPECT_EQ(std::find(values.begin(), values.end(), "Достоевский"), values.end());

  // The untrimmed overload still sees every book.
  shelf::collectValues(index, shelf::FilterField::Author, values);
  EXPECT_NE(std::find(values.begin(), values.end(), "Достоевский"), values.end());
}

// Reading status is derived from the statistics snapshot, never from metadata:
// 100 % is finished, anything in between is "reading", and a book that was never
// opened (or sits at 0 %) is unread, because "непрочитанные" is about the text.
TEST(LibraryQuery, FiltersByReadingStatus) {
  const shelf::ShelfIndex index = makeIndex();
  std::vector<shelf::SortStat> stats(index.books.size());
  stats[0].progressPercent = 100;  // Братья
  stats[1].progressPercent = 35;   // Анна
  stats[2].progressPercent = 0;    // Дюна — was opened, still 0 %
  stats[2].lastOpenedEpoch = 42;
  // Заметки — нет истории вовсе.

  const auto titlesFor = [&](shelf::ReadingStatus status) {
    shelf::LibraryQuery query;
    query.filter = {shelf::FilterField::ReadingStatus, shelf::readingStatusName(status)};
    std::vector<uint16_t> out;
    query.apply(index, out, &stats);
    std::vector<std::string> titles;
    for (const uint16_t id : out) titles.push_back(index.books[id].meta.title);
    return titles;
  };

  const auto finished = titlesFor(shelf::ReadingStatus::Finished);
  ASSERT_EQ(1u, finished.size());
  EXPECT_EQ("Братья Карамазовы", finished[0]);

  const auto reading = titlesFor(shelf::ReadingStatus::Reading);
  ASSERT_EQ(1u, reading.size());
  EXPECT_EQ("Анна Каренина", reading[0]);

  const auto unread = titlesFor(shelf::ReadingStatus::Unread);
  ASSERT_EQ(2u, unread.size());

  // Without statistics no book can claim a status, so the bucket is empty
  // instead of silently listing everything.
  shelf::LibraryQuery noStats;
  noStats.filter = {shelf::FilterField::ReadingStatus, shelf::readingStatusName(shelf::ReadingStatus::Unread)};
  std::vector<uint16_t> out;
  noStats.apply(index, out, nullptr);
  EXPECT_TRUE(out.empty());

  // The picker always lists all three buckets, each with its own count.
  std::vector<shelf::FilterValueCount> counts;
  shelf::LibraryQuery query;
  query.collectValueCounts(index, shelf::FilterField::ReadingStatus, counts, 200, &stats);
  ASSERT_EQ(3u, counts.size());
  EXPECT_EQ("unread", counts[0].value);
  EXPECT_EQ(2, counts[0].count);
  EXPECT_EQ("reading", counts[1].value);
  EXPECT_EQ(1, counts[1].count);
  EXPECT_EQ("finished", counts[2].value);
  EXPECT_EQ(1, counts[2].count);
}

TEST(LibraryQuery, NamesRoundTrip) {
  for (const auto field : {shelf::FilterField::Author, shelf::FilterField::Series, shelf::FilterField::Genre,
                           shelf::FilterField::Format, shelf::FilterField::ReadingStatus}) {
    EXPECT_EQ(field, shelf::filterFieldFromName(shelf::filterFieldName(field)));
  }
  EXPECT_EQ(shelf::FilterField::None, shelf::filterFieldFromName("nonsense"));
  for (const auto status :
       {shelf::ReadingStatus::Unread, shelf::ReadingStatus::Reading, shelf::ReadingStatus::Finished}) {
    EXPECT_EQ(status, shelf::readingStatusFromName(shelf::readingStatusName(status)));
  }
  for (size_t i = 0; i < shelf::kSortKeyCount; ++i) {
    const shelf::SortKey key = shelf::sortKeys()[i];
    EXPECT_EQ(key, shelf::sortKeyFromName(shelf::sortKeyName(key)));
  }
  EXPECT_EQ(shelf::SortKey::Title, shelf::sortKeyFromName("nonsense"));
}

// Reading order inside a series: decimal <sequence number> first, unnumbered
// books last, ties by title. The card's Left/Right uses exactly this order.
TEST(LibraryQuery, OrdersSeriesBooksByNumber) {
  shelf::ShelfIndex index;
  const auto book = [](const char* path, const char* title, const char* number) {
    shelf::ShelfBook b;
    b.path = path;
    b.meta.title = title;
    b.meta.series = "Цикл";
    b.meta.seriesIndex = number;
    b.metaOk = true;
    return b;
  };
  index.books.push_back(book("/b/three.fb2", "Третья", "3"));
  index.books.push_back(book("/b/one.fb2", "Первая", "1"));
  index.books.push_back(book("/b/extra.fb2", "Вне цикла", ""));
  index.books.push_back(book("/b/two.fb2", "Вторая", "2"));
  index.books.push_back(book("/b/other.fb2", "Чужое", ""));
  index.books.back().meta.series = "Другая";

  std::vector<uint16_t> out;
  shelf::collectSeriesBooks(index, "Цикл", out);
  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(index.books[out[0]].path, "/b/one.fb2");
  EXPECT_EQ(index.books[out[1]].path, "/b/two.fb2");
  EXPECT_EQ(index.books[out[2]].path, "/b/three.fb2");
  EXPECT_EQ(index.books[out[3]].path, "/b/extra.fb2");

  shelf::collectSeriesBooks(index, "", out);
  EXPECT_TRUE(out.empty());
}
TEST(LibraryQuery, FiltersByFormatLabel) {
  shelf::ShelfIndex index = makeIndex();
  index.books[0].format = shelf::BookFormat::Epub;
  index.books[1].format = shelf::BookFormat::Epub;
  index.books[2].format = shelf::BookFormat::Fb2;
  index.books[3].format = shelf::BookFormat::Txt;

  shelf::LibraryQuery query;
  query.filter = {shelf::FilterField::Format, "FB2"};
  std::vector<uint16_t> out;
  query.apply(index, out);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(index.books[out[0]].path, "/books/c.epub");

  std::vector<std::string> values;
  shelf::collectValues(index, shelf::FilterField::Format, values);
  ASSERT_EQ(values.size(), 3u);
  EXPECT_EQ(values[0], "EPUB");
  EXPECT_EQ(values[1], "FB2");
  EXPECT_EQ(values[2], "TXT");
}

// The browse row shows how many books carry a value for that dimension; hidden
// books do not count, exactly like the value lists below it.
TEST(LibraryQuery, CountsBooksPerDimension) {
  const shelf::ShelfIndex index = makeIndex();
  shelf::LibraryQuery query;

  EXPECT_EQ(3, query.countWith(index, shelf::FilterField::Author));
  EXPECT_EQ(3, query.countWith(index, shelf::FilterField::Series));
  EXPECT_EQ(3, query.countWith(index, shelf::FilterField::Genre));
  // The fixture books carry no detected format, so the format label is empty and
  // the dimension counts zero books — the same rule the format filter follows.
  EXPECT_EQ(0, query.countWith(index, shelf::FilterField::Format));

  query.hiddenPaths = {"/books/c.epub"};
  EXPECT_EQ(2, query.countWith(index, shelf::FilterField::Author));
  EXPECT_EQ(0, query.countWith(index, shelf::FilterField::Format));
  EXPECT_EQ(0, query.countWith(index, shelf::FilterField::None));
}

// The direction flips the primary key only: the same books come back, in the
// mirror order, and the tie-break by title still holds.
TEST(LibraryQuery, DirectionFlipsPrimaryKeyOnly) {
  const shelf::ShelfIndex index = makeIndex();
  std::vector<uint16_t> ascending;
  std::vector<uint16_t> descending;

  shelf::LibraryQuery query;
  query.sort = shelf::SortKey::Title;
  query.apply(index, ascending);
  query.descending = true;
  query.apply(index, descending);

  ASSERT_EQ(ascending.size(), descending.size());
  ASSERT_EQ(ascending.size(), 4u);
  EXPECT_EQ("Анна Каренина", index.books[ascending.front()].meta.title);
  EXPECT_EQ("Анна Каренина", index.books[descending.back()].meta.title);
  EXPECT_EQ(ascending.front(), descending.back());
  EXPECT_EQ(ascending.back(), descending.front());
  EXPECT_EQ("Заметки", index.books[descending.front()].meta.title);
}

// Series order is the series name, then the number inside it, then the title.
// Books without a series stay last in both directions.
TEST(LibraryQuery, SortsBySeriesThenNumber) {
  shelf::ShelfIndex index;
  const auto book = [](const char* path, const char* title, const char* series, const char* number) {
    shelf::ShelfBook b;
    b.path = path;
    b.meta.title = title;
    b.meta.series = series;
    b.meta.seriesIndex = number;
    b.metaOk = true;
    return b;
  };
  index.books.push_back(book("/b/three.fb2", "Третья", "Цикл", "3"));
  index.books.push_back(book("/b/solo.fb2", "Одиночная", "", ""));
  index.books.push_back(book("/b/one.fb2", "Первая", "Цикл", "1"));
  index.books.push_back(book("/b/other.fb2", "Другой цикл", "Другое", "1"));
  index.books.push_back(book("/b/two.fb2", "Вторая", "Цикл", "2"));

  std::vector<uint16_t> out;
  shelf::LibraryQuery query;
  query.sort = shelf::SortKey::Series;
  query.apply(index, out);
  ASSERT_EQ(out.size(), 5u);
  // "Другое" fold-compares before "Цикл"; inside "Цикл" the numbers order the books.
  EXPECT_EQ("/b/other.fb2", index.books[out[0]].path);
  EXPECT_EQ("/b/one.fb2", index.books[out[1]].path);
  EXPECT_EQ("/b/two.fb2", index.books[out[2]].path);
  EXPECT_EQ("/b/three.fb2", index.books[out[3]].path);
  // A book with no series is not a name that ordering can place: it stays last.
  EXPECT_EQ("/b/solo.fb2", index.books[out[4]].path);

  // Inside one series the numbers decide; the unnumbered book sits after them.
  std::vector<uint16_t> cycle;
  shelf::LibraryQuery inCycle;
  inCycle.sort = shelf::SortKey::Series;
  inCycle.filter = {shelf::FilterField::Series, "Цикл"};
  inCycle.apply(index, cycle);
  ASSERT_EQ(cycle.size(), 3u);
  EXPECT_EQ("Первая", index.books[cycle[0]].meta.title);
  EXPECT_EQ("Вторая", index.books[cycle[1]].meta.title);
  EXPECT_EQ("Третья", index.books[cycle[2]].meta.title);

  // Descending reverses the series order but never pulls "no series" to the front.
  inCycle.filter.clear();
  inCycle.descending = true;
  inCycle.apply(index, out);
  ASSERT_EQ(out.size(), 5u);
  EXPECT_EQ("/b/three.fb2", index.books[out.front()].path);
  EXPECT_EQ("/b/solo.fb2", index.books[out.back()].path);
}

TEST(LibraryQuery, SortsBySizeAddedProgressAndYear) {
  shelf::ShelfIndex index = makeIndex();
  index.books[0].size = 500;
  index.books[1].size = 100;
  index.books[2].size = 900;
  index.books[3].size = 300;
  index.books[0].fatDateTime = 0x12;  // ascending FAT timestamps
  index.books[1].fatDateTime = 0x30;
  index.books[2].fatDateTime = 0x05;
  index.books[3].fatDateTime = 0x22;
  index.books[0].meta.year = "1965";
  index.books[1].meta.year = "1880";
  index.books[2].meta.year = "";
  index.books[3].meta.year = "1984";

  std::vector<shelf::SortStat> stats(index.books.size());
  stats[0].progressPercent = 40;
  stats[1].progressPercent = 90;
  stats[2].progressPercent = 0;
  stats[3].progressPercent = 10;

  std::vector<uint16_t> out;
  shelf::LibraryQuery query;

  query.sort = shelf::SortKey::Size;
  query.descending = true;
  query.apply(index, out);
  EXPECT_EQ(900u, index.books[out.front()].size);

  query.sort = shelf::SortKey::Added;
  query.apply(index, out);
  EXPECT_EQ(0x30u, index.books[out.front()].fatDateTime);

  query.sort = shelf::SortKey::Progress;
  query.apply(index, out);
  EXPECT_EQ(90, stats[out.front()].progressPercent);

  query.sort = shelf::SortKey::Year;
  query.descending = false;
  query.apply(index, out);
  EXPECT_EQ("1880", index.books[out[0]].meta.year);
  EXPECT_EQ("1965", index.books[out[1]].meta.year);
  EXPECT_EQ("1984", index.books[out[2]].meta.year);
  EXPECT_EQ("", index.books[out[3]].meta.year);  // no year declared: last
}

// The picker pre-selects these, so a wrong table would turn "Недавние" into
// "oldest first" without the user doing anything.
TEST(LibraryQuery, NaturalDirectionsMatchTheUsefulEnd) {
  EXPECT_FALSE(shelf::naturalDescending(shelf::SortKey::Title));
  EXPECT_FALSE(shelf::naturalDescending(shelf::SortKey::Author));
  EXPECT_FALSE(shelf::naturalDescending(shelf::SortKey::Series));
  EXPECT_TRUE(shelf::naturalDescending(shelf::SortKey::LastOpened));
  EXPECT_TRUE(shelf::naturalDescending(shelf::SortKey::Progress));
  EXPECT_TRUE(shelf::naturalDescending(shelf::SortKey::Size));
  EXPECT_TRUE(shelf::naturalDescending(shelf::SortKey::Added));
  EXPECT_TRUE(shelf::naturalDescending(shelf::SortKey::Year));
}
