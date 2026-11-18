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

TEST(LibraryQuery, SortsByLastOpened) {
  const shelf::ShelfIndex index = makeIndex();
  std::vector<uint16_t> out;

  // index order: 0 Братья, 1 Анна, 2 Дюна, 3 Заметки. 0 = never opened.
  std::vector<uint32_t> epochs(index.books.size(), 0);
  epochs[0] = 1000;
  epochs[2] = 3000;
  epochs[1] = 2000;

  shelf::LibraryQuery byRecent;
  byRecent.sort = shelf::SortKey::LastOpened;
  byRecent.apply(index, out, &epochs);
  ASSERT_EQ(index.books.size(), out.size());
  EXPECT_EQ("Дюна", index.books[out[0]].meta.title);  // newest first
  EXPECT_EQ("Анна Каренина", index.books[out[1]].meta.title);
  EXPECT_EQ("Братья Карамазовы", index.books[out[2]].meta.title);
  EXPECT_EQ("Заметки", index.books[out[3]].meta.title);  // never opened goes last

  // Without epoch data the sort degrades to title order, never to garbage.
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

TEST(LibraryQuery, NamesRoundTrip) {
  for (const auto field : {shelf::FilterField::Author, shelf::FilterField::Series, shelf::FilterField::Genre,
                           shelf::FilterField::Format}) {
    EXPECT_EQ(field, shelf::filterFieldFromName(shelf::filterFieldName(field)));
  }
  EXPECT_EQ(shelf::FilterField::None, shelf::filterFieldFromName("nonsense"));
  for (const auto key : {shelf::SortKey::Title, shelf::SortKey::Author, shelf::SortKey::LastOpened}) {
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
