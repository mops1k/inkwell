#include <gtest/gtest.h>

#include <string>

#include "BookFormat.h"
#include "BookMetadata.h"
#include "util/BookArchiveUtils.h"

namespace {

std::string shelfFixture(const char* name) { return std::string(SHELF_FIXTURES) + "/" + name; }

std::string fixture(const char* name) { return std::string(TEST_FIXTURES) + "/" + name; }

}  // namespace

TEST(ShelfFormat, DetectsByExtension) {
  EXPECT_EQ(shelf::BookFormat::Epub, shelf::detectBookFormat("/books/a.epub"));
  EXPECT_EQ(shelf::BookFormat::Fb2, shelf::detectBookFormat("/books/a.fb2"));
  EXPECT_EQ(shelf::BookFormat::Fb2Zip, shelf::detectBookFormat("/books/a.fb2.zip"));
  EXPECT_EQ(shelf::BookFormat::Txt, shelf::detectBookFormat("/books/a.txt"));
  EXPECT_EQ(shelf::BookFormat::Markdown, shelf::detectBookFormat("/books/a.md"));
  EXPECT_EQ(shelf::BookFormat::Xtc, shelf::detectBookFormat("/books/a.xtc"));
  EXPECT_EQ(shelf::BookFormat::Xtc, shelf::detectBookFormat("/books/a.xtch"));
  EXPECT_EQ(shelf::BookFormat::Epub, shelf::detectBookFormat("/books/BOOK.EPUB"));
  EXPECT_EQ(shelf::BookFormat::Unknown, shelf::detectBookFormat("/books/a.pdf"));
  EXPECT_EQ(shelf::BookFormat::Unknown, shelf::detectBookFormat("/books/a"));
}

TEST(ShelfFormat, SniffsArchiveContentsNotNames) {
  EXPECT_EQ(shelf::BookFormat::Epub, shelf::detectBookFormat(shelfFixture("epub_inside.zip")));
  EXPECT_EQ(shelf::BookFormat::Fb2Zip, shelf::detectBookFormat(shelfFixture("fb2_inside.zip")));
  // A plain .zip is never trusted by name; a missing archive is not a book.
  EXPECT_EQ(shelf::BookFormat::Unknown, shelf::detectBookFormat("/nonexistent/shelf/missing.zip"));
  // A ".fb2.zip" name is trusted by extension, exactly like the reader's own
  // isFb2BookPath(); a broken one then fails metadata extraction and falls back
  // to its filename instead of disappearing from the shelf.
  EXPECT_EQ(shelf::BookFormat::Fb2Zip, shelf::detectBookFormat(shelfFixture("broken.fb2.zip")));
}

// The upstream wrapper now delegates to lib/Shelf; its contract must not change.
TEST(ShelfFormat, ArchiveTypeWrapperKeepsItsContract) {
  EXPECT_EQ(BookArchiveType::Epub, detectBookArchiveType(shelfFixture("epub_inside.zip")));
  EXPECT_EQ(BookArchiveType::Fb2, detectBookArchiveType(shelfFixture("fb2_inside.zip")));
  // The wrapper stays content-based: a zip with no FB2 entry is None, even
  // when its name ends in .fb2.zip.
  EXPECT_EQ(BookArchiveType::None, detectBookArchiveType(shelfFixture("broken.fb2.zip")));
  // Only ZIP paths are sniffed upstream, a plain .fb2 still reports None.
  EXPECT_EQ(BookArchiveType::None, detectBookArchiveType(fixture("fb2_split_anchors.fb2")));
}

TEST(ShelfFormat, BrokenContainerKeepsFilenameTitle) {
  shelf::BookMeta meta;
  // Extension says EPUB, content is garbage: detection succeeds, metadata fails.
  EXPECT_EQ(shelf::BookFormat::Epub, shelf::detectBookFormat(shelfFixture("broken.epub")));
  EXPECT_FALSE(shelf::readBookMetadata(shelfFixture("broken.epub"), shelf::BookFormat::Epub, meta));
}
