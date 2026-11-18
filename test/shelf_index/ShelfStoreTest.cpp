#include <gtest/gtest.h>
#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "ShelfIndexStore.h"
#include "TempDir.h"

namespace {

shelf::ShelfIndex makeIndex(const char* title) {
  shelf::ShelfIndex index;
  shelf::ShelfBook book;
  book.path = "/books/a.epub";
  book.size = 42;
  book.format = shelf::BookFormat::Epub;
  book.meta.title = title;
  book.metaOk = true;
  index.books.push_back(book);
  return index;
}

}  // namespace

TEST(ShelfStore, SavesAndLoads) {
  shelftest::TempDir dir("store_roundtrip");
  shelf::ShelfIndexStore store(shelf::ShelfPaths::forDirectory(dir.join("shelf")));

  ASSERT_TRUE(store.save(makeIndex("First")));
  EXPECT_TRUE(std::filesystem::exists(store.paths().file));

  shelf::ShelfIndex loaded;
  ASSERT_TRUE(store.load(loaded));
  ASSERT_EQ(1u, loaded.books.size());
  EXPECT_EQ("First", loaded.books.front().meta.title);
}

TEST(ShelfStore, CreatesItsDirectory) {
  shelftest::TempDir dir("store_mkdir");
  shelf::ShelfIndexStore store(shelf::ShelfPaths::forDirectory(dir.join("nested/shelf")));
  EXPECT_TRUE(store.save(makeIndex("Nested")));
  EXPECT_TRUE(std::filesystem::exists(store.paths().file));
}

TEST(ShelfStore, RecoveringFromBakAfterCorruption) {
  shelftest::TempDir dir("store_bak");
  shelf::ShelfIndexStore store(shelf::ShelfPaths::forDirectory(dir.join("shelf")));
  ASSERT_TRUE(store.save(makeIndex("First")));
  ASSERT_TRUE(store.save(makeIndex("Second")));
  EXPECT_TRUE(std::filesystem::exists(store.paths().bak));

  // Simulate a power loss that left a half-written current document.
  std::ofstream(store.paths().file, std::ios::binary | std::ios::trunc) << "{\"version\":1,\"books\":[{\"path\"";

  shelf::ShelfIndex loaded;
  bool fromBak = false;
  ASSERT_TRUE(store.load(loaded, &fromBak));
  EXPECT_TRUE(fromBak);
  ASSERT_EQ(1u, loaded.books.size());
  EXPECT_EQ("First", loaded.books.front().meta.title);
}

TEST(ShelfStore, BothCopiesCorruptReportFailure) {
  shelftest::TempDir dir("store_corrupt");
  shelf::ShelfIndexStore store(shelf::ShelfPaths::forDirectory(dir.join("shelf")));
  ASSERT_TRUE(store.save(makeIndex("First")));
  ASSERT_TRUE(store.save(makeIndex("Second")));

  std::ofstream(store.paths().file, std::ios::binary | std::ios::trunc) << "garbage";
  std::ofstream(store.paths().bak, std::ios::binary | std::ios::trunc) << "garbage too";

  shelf::ShelfIndex loaded;
  EXPECT_FALSE(store.load(loaded));
  EXPECT_TRUE(loaded.books.empty());
}

TEST(ShelfStore, MissingEverythingReportsFailure) {
  shelftest::TempDir dir("store_missing");
  shelf::ShelfIndexStore store(shelf::ShelfPaths::forDirectory(dir.join("shelf")));
  shelf::ShelfIndex loaded;
  EXPECT_FALSE(store.load(loaded));
}

TEST(ShelfStore, LeftoverTempFileIsIgnored) {
  shelftest::TempDir dir("store_tmp");
  shelf::ShelfIndexStore store(shelf::ShelfPaths::forDirectory(dir.join("shelf")));
  ASSERT_TRUE(store.save(makeIndex("Good")));
  // A crash mid-save can leave a partial .tmp behind; it must never win over
  // the committed document.
  std::ofstream(store.paths().tmp, std::ios::binary | std::ios::trunc) << "{\"version\":1,\"books\":[{\"path\":\"x\"}";

  shelf::ShelfIndex loaded;
  ASSERT_TRUE(store.load(loaded));
  ASSERT_EQ(1u, loaded.books.size());
  EXPECT_EQ("Good", loaded.books.front().meta.title);
}

// Fault injection: an unwritable directory makes the write-aside fail, and the
// previous index must still be readable afterwards.
TEST(ShelfStore, UnwritableDirectoryKeepsPreviousIndex) {
  shelftest::TempDir dir("store_ro");
  const std::string shelfDir = dir.join("shelf");
  shelf::ShelfIndexStore store(shelf::ShelfPaths::forDirectory(shelfDir));
  ASSERT_TRUE(store.save(makeIndex("Previous")));

  ASSERT_EQ(0, ::chmod(shelfDir.c_str(), 0555));
  const bool saved = store.save(makeIndex("Should not land"));
  ::chmod(shelfDir.c_str(), 0755);

  EXPECT_FALSE(saved);
  shelf::ShelfIndex loaded;
  ASSERT_TRUE(store.load(loaded));
  ASSERT_EQ(1u, loaded.books.size());
  EXPECT_EQ("Previous", loaded.books.front().meta.title);
}
