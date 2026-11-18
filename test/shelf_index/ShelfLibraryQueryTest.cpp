#include <gtest/gtest.h>

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

TEST(LibraryQuery, NamesRoundTrip) {
  for (const auto field : {shelf::FilterField::Author, shelf::FilterField::Series, shelf::FilterField::Genre}) {
    EXPECT_EQ(field, shelf::filterFieldFromName(shelf::filterFieldName(field)));
  }
  EXPECT_EQ(shelf::FilterField::None, shelf::filterFieldFromName("nonsense"));
  for (const auto key : {shelf::SortKey::Title, shelf::SortKey::Author}) {
    EXPECT_EQ(key, shelf::sortKeyFromName(shelf::sortKeyName(key)));
  }
  EXPECT_EQ(shelf::SortKey::Title, shelf::sortKeyFromName("nonsense"));
}
