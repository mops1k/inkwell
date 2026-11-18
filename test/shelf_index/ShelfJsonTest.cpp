#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "ShelfIndex.h"

namespace {

shelf::ShelfBook makeBook(const std::string& path) {
  shelf::ShelfBook book;
  book.path = path;
  book.size = 1000;
  book.fatDateTime = 0x5A2B1234;
  book.format = shelf::BookFormat::Epub;
  book.meta.title = "Title of " + path;
  book.meta.author = "Author";
  book.metaOk = true;
  return book;
}

}  // namespace

TEST(ShelfJson, RoundTripsEveryField) {
  shelf::ShelfIndex index;
  shelf::ShelfBook book;
  book.path = "/books/Лев Толстой - Война и мир.epub";
  book.size = 123456;
  book.fatDateTime = 0x5A2B1234;
  book.format = shelf::BookFormat::Fb2Zip;
  book.meta.title = "Война и \"мир\"\nвторая строка\tс табуляцией";
  book.meta.author = "Лев Толстой";
  book.meta.series = "Война и мир";
  book.meta.seriesIndex = "2";
  book.meta.language = "ru";
  book.meta.genre = "sf_history";
  book.metaOk = true;
  index.books.push_back(book);

  std::string payload;
  shelf::serializeIndex(index, payload);
  ASSERT_FALSE(payload.empty());
  // Control characters must be escaped, never emitted raw inside a value: only
  // the document's own trailing newline may appear literally.
  EXPECT_NE(std::string::npos, payload.find("\\n"));
  EXPECT_NE(std::string::npos, payload.find("\\t"));
  EXPECT_EQ(payload.find('\n'), payload.size() - 1);

  shelf::ShelfIndex parsed;
  ASSERT_TRUE(shelf::parseIndex(payload, parsed));
  ASSERT_EQ(1u, parsed.books.size());
  const auto& got = parsed.books.front();
  EXPECT_EQ(book.path, got.path);
  EXPECT_EQ(book.size, got.size);
  EXPECT_EQ(book.fatDateTime, got.fatDateTime);
  EXPECT_EQ(book.format, got.format);
  EXPECT_EQ(book.meta.title, got.meta.title);
  EXPECT_EQ(book.meta.author, got.meta.author);
  EXPECT_EQ(book.meta.series, got.meta.series);
  EXPECT_EQ(book.meta.seriesIndex, got.meta.seriesIndex);
  EXPECT_EQ(book.meta.language, got.meta.language);
  EXPECT_EQ(book.meta.genre, got.meta.genre);
  EXPECT_EQ(book.metaOk, got.metaOk);
}

TEST(ShelfJson, AnEmptyIndexRoundTrips) {
  shelf::ShelfIndex index;
  std::string payload;
  shelf::serializeIndex(index, payload);
  shelf::ShelfIndex parsed;
  EXPECT_TRUE(shelf::parseIndex(payload, parsed));
  EXPECT_TRUE(parsed.books.empty());
}

TEST(ShelfJson, TwoHundredBooksStayWithinBudget) {
  shelf::ShelfIndex index;
  for (int i = 0; i < 200; ++i) index.books.push_back(makeBook("/books/book" + std::to_string(i) + ".epub"));

  std::string payload;
  shelf::serializeIndex(index, payload);
  // Plan Ф1 budgets 20-40 KiB for ~100 books; 200 books must stay well under
  // a size that would make the HTTP/JSON parser or the heap unhappy.
  EXPECT_LT(payload.size(), static_cast<size_t>(128 * 1024));

  shelf::ShelfIndex parsed;
  ASSERT_TRUE(shelf::parseIndex(payload, parsed));
  EXPECT_EQ(index.books.size(), parsed.books.size());
  EXPECT_EQ(index.books.back().path, parsed.books.back().path);
}

TEST(ShelfJson, TruncatedDocumentIsRejected) {
  shelf::ShelfIndex index;
  index.books.push_back(makeBook("/books/a.epub"));
  index.books.push_back(makeBook("/books/b.epub"));
  std::string payload;
  shelf::serializeIndex(index, payload);

  shelf::ShelfIndex parsed;
  EXPECT_FALSE(shelf::parseIndex(payload.substr(0, payload.size() / 2), parsed));
  EXPECT_TRUE(parsed.books.empty());
}

TEST(ShelfJson, ForeignVersionIsRejected) {
  shelf::ShelfIndex index;
  index.books.push_back(makeBook("/books/a.epub"));
  std::string payload;
  shelf::serializeIndex(index, payload);
  // Read the current version out of the document instead of hard-coding it: the
  // test is about "a version this build does not know", not about the number.
  const size_t at = payload.find("\"version\":");
  ASSERT_NE(std::string::npos, at);
  const size_t digits = at + std::strlen("\"version\":");
  const int version = std::stoi(payload.substr(digits));
  const std::string from = "\"version\":" + std::to_string(version);
  payload.replace(at, from.size(), "\"version\":" + std::to_string(version + 1));

  shelf::ShelfIndex parsed;
  EXPECT_FALSE(shelf::parseIndex(payload, parsed));
}

// v1 indexes (no `genre` key) must keep loading: a firmware update must never
// force a full rescan, and an older index simply filters as "genre unknown".
TEST(ShelfJson, VersionOneIndexStillLoads) {
  const std::string v1 =
      "{\"version\":1,\"books\":[{\"path\":\"/books/a.epub\",\"size\":10,\"date\":1,\"format\":\"epub\","
      "\"title\":\"A\",\"author\":\"B\",\"series\":\"\",\"seriesIndex\":\"\",\"language\":\"ru\",\"metaOk\":true}]}\n";
  shelf::ShelfIndex parsed;
  ASSERT_TRUE(shelf::parseIndex(v1, parsed));
  ASSERT_EQ(1u, parsed.books.size());
  EXPECT_EQ(parsed.books.front().meta.title, "A");
  EXPECT_TRUE(parsed.books.front().meta.genre.empty());
}

TEST(ShelfJson, GarbageIsRejected) {
  shelf::ShelfIndex parsed;
  EXPECT_FALSE(shelf::parseIndex("", parsed));
  EXPECT_FALSE(shelf::parseIndex("not json at all", parsed));
  EXPECT_FALSE(shelf::parseIndex("{\"version\":1}", parsed));  // no books array
  EXPECT_FALSE(shelf::parseIndex("[1,2,3]", parsed));          // not an object
}

TEST(ShelfJson, LongStringsAreCappedButReadable) {
  shelf::ShelfIndex index;
  shelf::ShelfBook book = makeBook("/books/long.epub");
  book.meta.title.assign(2000, 'A');
  index.books.push_back(book);

  std::string payload;
  shelf::serializeIndex(index, payload);
  shelf::ShelfIndex parsed;
  ASSERT_TRUE(shelf::parseIndex(payload, parsed));
  ASSERT_EQ(1u, parsed.books.size());
  // Capped to what the streaming parser can hold in one token, never dropped.
  EXPECT_GT(parsed.books.front().meta.title.size(), static_cast<size_t>(200));
  EXPECT_LE(parsed.books.front().meta.title.size(), static_cast<size_t>(480));
}
