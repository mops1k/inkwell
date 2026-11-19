#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "Fb2Metadata.h"
#include "TempDir.h"

namespace {

std::string shelfFixture(const char* name) { return std::string(SHELF_FIXTURES) + "/" + name; }

std::string fixture(const char* name) { return std::string(TEST_FIXTURES) + "/" + name; }

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

// The Windows-1251 path is the one a plain XML reader silently mangles.
TEST(ShelfFb2, ReadsWindows1251Metadata) {
  shelf::BookMeta meta;
  ASSERT_TRUE(shelf::readFb2Metadata(shelfFixture("fb2_cp1251.fb2"), shelf::BookFormat::Fb2, meta));
  EXPECT_EQ("Отцы и дети", meta.title);
  EXPECT_TRUE(contains(meta.author, "Иван"));
  EXPECT_TRUE(contains(meta.author, "Тургенев"));
  EXPECT_EQ("ru", meta.language);
}

TEST(ShelfFb2, ReadsUtf8Metadata) {
  shelf::BookMeta meta;
  ASSERT_TRUE(shelf::readFb2Metadata(fixture("fb2_split_anchors.fb2"), shelf::BookFormat::Fb2, meta));
  EXPECT_EQ("Synthetic anchor regression", meta.title);
}

// Real FB2 files pad <genre> with newlines and list several genres; the shelf
// shows one genre and filters by it, so the scanner output is normalised.
TEST(ShelfFb2, GenreIsOneTrimmedLine) {
  shelf::BookMeta meta;
  ASSERT_TRUE(shelf::readFb2Metadata(fixture("fb2_split_anchors.fb2"), shelf::BookFormat::Fb2, meta));
  EXPECT_EQ(meta.genre.find('\n'), std::string::npos);
  EXPECT_EQ(meta.genre.find('\r'), std::string::npos);
  EXPECT_EQ(meta.genre.find("  "), std::string::npos);
  if (!meta.genre.empty()) {
    EXPECT_NE(meta.genre.front(), ' ');
    EXPECT_NE(meta.genre.back(), ' ');
  }
}

TEST(ShelfFb2, ReadsMetadataFromZipArchive) {
  shelf::BookMeta meta;
  ASSERT_TRUE(shelf::readFb2Metadata(shelfFixture("fb2_inside.zip"), shelf::BookFormat::Fb2Zip, meta));
  EXPECT_FALSE(meta.title.empty());
}

// <publish-info> carries the publication year and the publisher; the card shows
// them verbatim and the year also feeds the "по году" sort.
TEST(ShelfFb2, ReadsPublicationInfo) {
  shelf::BookMeta meta;
  ASSERT_TRUE(shelf::readFb2Metadata(shelfFixture("fb2_cp1251.fb2"), shelf::BookFormat::Fb2, meta));
  EXPECT_EQ("1965", meta.year);
  EXPECT_EQ("Художественная литература", meta.publisher);
  // FB2 has no page declaration: an empty field, not a guessed number.
  EXPECT_TRUE(meta.pages.empty());
}

// Without <publish-info> the year falls back to the four digits of
// <title-info><date>; the publisher stays empty instead of borrowing text.
TEST(ShelfFb2, PublicationYearFallsBackToTitleInfoDate) {
  shelf::BookMeta meta;
  ASSERT_TRUE(shelf::readFb2Metadata(shelfFixture("fb2_date_only.fb2"), shelf::BookFormat::Fb2, meta));
  EXPECT_EQ("1984", meta.year);
  EXPECT_TRUE(meta.publisher.empty());
}

TEST(ShelfFb2, RejectsBrokenContainers) {
  shelf::BookMeta meta;
  // Valid zip, but no *.fb2 entry inside.
  EXPECT_FALSE(shelf::readFb2Metadata(shelfFixture("broken.fb2.zip"), shelf::BookFormat::Fb2Zip, meta));

  shelftest::TempDir dir("fb2_broken");
  const std::string garbage = dir.join("garbage.fb2");
  std::ofstream(garbage, std::ios::binary | std::ios::trunc) << "definitely not fictionbook xml";
  EXPECT_FALSE(shelf::readFb2Metadata(garbage, shelf::BookFormat::Fb2, meta));

  // Truncated in the middle of the description.
  const std::string truncated = dir.join("truncated.fb2");
  std::ifstream source(shelfFixture("fb2_cp1251.fb2"), std::ios::binary);
  std::string head(80, '\0');
  source.read(head.data(), static_cast<std::streamsize>(head.size()));
  std::ofstream(truncated, std::ios::binary | std::ios::trunc) << head;
  EXPECT_FALSE(shelf::readFb2Metadata(truncated, shelf::BookFormat::Fb2, meta));
}

TEST(ShelfFb2, WrongFormatIsRejected) {
  shelf::BookMeta meta;
  EXPECT_FALSE(shelf::readFb2Metadata(fixture("fb2_split_anchors.fb2"), shelf::BookFormat::Epub, meta));
}
