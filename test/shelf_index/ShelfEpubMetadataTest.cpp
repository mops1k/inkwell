#include <gtest/gtest.h>

#include <string>

#include "BookMetadata.h"

namespace {

std::string shelfFixture(const char* name) { return std::string(SHELF_FIXTURES) + "/" + name; }

std::string fixture(const char* name) { return std::string(TEST_FIXTURES) + "/" + name; }

}  // namespace

// Runs the real Epub::loadForMetadata() over the repository's Moby-Dick fixture.
TEST(ShelfEpub, ReadsOpfMetadata) {
  shelf::BookMeta meta;
  ASSERT_TRUE(shelf::readBookMetadata(fixture("moby-dick.epub"), shelf::BookFormat::Epub, meta));
  EXPECT_EQ("Moby Dick; Or, The Whale", meta.title);
  EXPECT_EQ("Herman Melville", meta.author);
  EXPECT_EQ("en", meta.language);
}

// dc:date/dc:publisher/dcterms:extent reach the shelf as year, publisher and
// pages: "320 pages" is a page count, while a size in dcterms:extent must never
// become one (see declaredPages in Epub.cpp).
TEST(ShelfEpub, ReadsPublicationMetadata) {
  shelf::BookMeta meta;
  ASSERT_TRUE(shelf::readBookMetadata(shelfFixture("publication.epub"), shelf::BookFormat::Epub, meta));
  EXPECT_EQ("1984", meta.year);
  EXPECT_EQ("Тестовое издательство", meta.publisher);
  EXPECT_EQ("320", meta.pages);
}

TEST(ShelfEpub, BrokenArchiveFails) {
  shelf::BookMeta meta;
  EXPECT_FALSE(shelf::readBookMetadata(shelfFixture("broken.epub"), shelf::BookFormat::Epub, meta));
}

// The dispatcher is the single entry point: filename-derived formats work
// through it as well.
TEST(ShelfMetadataDispatch, TextAndMarkdownUseFilenames) {
  shelf::BookMeta meta;
  ASSERT_TRUE(shelf::readBookMetadata(shelfFixture("author - title.txt"), shelf::BookFormat::Txt, meta));
  EXPECT_EQ("title", meta.title);
  EXPECT_EQ("author", meta.author);

  ASSERT_TRUE(shelf::readBookMetadata(shelfFixture("just-title.md"), shelf::BookFormat::Markdown, meta));
  EXPECT_EQ("just-title", meta.title);
  EXPECT_TRUE(meta.author.empty());
}

TEST(ShelfMetadataDispatch, UnknownFormatIsRejected) {
  shelf::BookMeta meta;
  EXPECT_FALSE(shelf::readBookMetadata(fixture("moby-dick.epub"), shelf::BookFormat::Unknown, meta));
}
