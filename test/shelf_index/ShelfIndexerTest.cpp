#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "BookNameMeta.h"
#include "ShelfIndexer.h"
#include "TempDir.h"

namespace {

struct FakeMeta {
  int calls = 0;
};

bool fakeMetadata(void* ctx, const std::string& path, shelf::BookFormat, shelf::BookMeta& out) {
  ++static_cast<FakeMeta*>(ctx)->calls;
  out.title = "T:" + shelf::fileNameStem(path);
  return true;
}

void writeFile(const std::string& path, const std::string& content) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
}

shelf::ShelfScanOptions optionsFor(const shelftest::TempDir& dir) {
  shelf::ShelfScanOptions options;
  options.root = dir.path();
  options.booksRoot = dir.join("books");
  return options;
}

size_t countPaths(const shelf::ShelfIndex& index, const std::string& needle) {
  size_t found = 0;
  for (const auto& book : index.books) {
    if (book.path.find(needle) != std::string::npos) ++found;
  }
  return found;
}

}  // namespace

TEST(ShelfIndexer, WalksRootShallowlyAndBooksRecursively) {
  shelftest::TempDir dir("indexer_walk");
  writeFile(dir.join("top.txt"), "root book");
  writeFile(dir.join("sub/deep.epub"), "root subfolder must not be walked");
  writeFile(dir.join("books/nested/novel.md"), "nested markdown");
  writeFile(dir.join("books/note.pdf"), "not a book");
  writeFile(dir.join(".hidden-book.txt"), "hidden");
  writeFile(dir.join(".crosspoint/cache/skip.epub"), "cache dir");

  FakeMeta ctx;
  shelf::ShelfIndexer indexer(fakeMetadata, &ctx);
  shelf::ShelfIndex index;
  const shelf::ShelfScanStats stats = indexer.scan(index, optionsFor(dir));

  EXPECT_EQ(2u, index.books.size());
  EXPECT_EQ(1u, countPaths(index, "top.txt"));
  EXPECT_EQ(1u, countPaths(index, "novel.md"));
  EXPECT_EQ(0u, countPaths(index, "sub/deep.epub"));
  EXPECT_EQ(0u, countPaths(index, "hidden"));
  EXPECT_EQ(0u, countPaths(index, ".crosspoint"));
  EXPECT_EQ(2u, stats.refreshed);
  EXPECT_EQ(2, ctx.calls);
}

TEST(ShelfIndexer, UnchangedFilesAreNotReopened) {
  shelftest::TempDir dir("indexer_increment");
  writeFile(dir.join("top.txt"), "root book");
  writeFile(dir.join("books/a.md"), "markdown");

  FakeMeta ctx;
  shelf::ShelfIndexer indexer(fakeMetadata, &ctx);
  shelf::ShelfIndex index;
  indexer.scan(index, optionsFor(dir));
  ASSERT_EQ(2, ctx.calls);

  const shelf::ShelfScanStats second = indexer.scan(index, optionsFor(dir));
  EXPECT_EQ(2u, second.kept);
  EXPECT_EQ(0u, second.refreshed);
  EXPECT_EQ(2, ctx.calls) << "an unchanged file must not be parsed again";
}

TEST(ShelfIndexer, ChangedFilesAreRefreshed) {
  shelftest::TempDir dir("indexer_changed");
  const std::string book = dir.join("books/a.md");
  writeFile(book, "short");

  FakeMeta ctx;
  shelf::ShelfIndexer indexer(fakeMetadata, &ctx);
  shelf::ShelfIndex index;
  indexer.scan(index, optionsFor(dir));
  ASSERT_EQ(1, ctx.calls);

  // Different size and timestamp: the file signature changed.
  writeFile(book, "a much longer body that changes both size and mtime");
  const shelf::ShelfScanStats second = indexer.scan(index, optionsFor(dir));
  EXPECT_EQ(1u, second.refreshed);
  EXPECT_EQ(0u, second.kept);
  EXPECT_EQ(2, ctx.calls);
}

TEST(ShelfIndexer, DeletedBooksAreDropped) {
  shelftest::TempDir dir("indexer_removed");
  writeFile(dir.join("books/a.md"), "one");
  writeFile(dir.join("books/b.md"), "two");

  FakeMeta ctx;
  shelf::ShelfIndexer indexer(fakeMetadata, &ctx);
  shelf::ShelfIndex index;
  indexer.scan(index, optionsFor(dir));
  ASSERT_EQ(2u, index.books.size());

  std::filesystem::remove(dir.join("books/b.md"));
  const shelf::ShelfScanStats second = indexer.scan(index, optionsFor(dir));
  EXPECT_EQ(1u, second.removed);
  EXPECT_EQ(1u, index.books.size());
  EXPECT_EQ(1u, countPaths(index, "a.md"));
}

TEST(ShelfIndexer, MaxBooksLimitTruncatesTheScan) {
  shelftest::TempDir dir("indexer_limit");
  writeFile(dir.join("a.txt"), "a");
  writeFile(dir.join("b.txt"), "b");
  writeFile(dir.join("c.txt"), "c");

  FakeMeta ctx;
  shelf::ShelfIndexer indexer(fakeMetadata, &ctx);
  shelf::ShelfIndex index;
  shelf::ShelfScanOptions options = optionsFor(dir);
  options.maxBooks = 2;
  const shelf::ShelfScanStats stats = indexer.scan(index, options);

  EXPECT_TRUE(stats.truncated);
  EXPECT_EQ(2u, index.books.size());
}

TEST(ShelfIndexer, UnreadableRootsKeepTheLastKnownLibrary) {
  shelftest::TempDir dir("indexer_unreadable");
  writeFile(dir.join("books/a.md"), "one");

  FakeMeta ctx;
  shelf::ShelfIndexer indexer(fakeMetadata, &ctx);
  shelf::ShelfIndex index;
  indexer.scan(index, optionsFor(dir));
  ASSERT_EQ(1u, index.books.size());

  shelf::ShelfScanOptions gone = optionsFor(dir);
  gone.root = "/nonexistent-shelf-root";
  gone.booksRoot = "/nonexistent-shelf-books";
  const shelf::ShelfScanStats stats = indexer.scan(index, gone);

  EXPECT_EQ(0u, stats.removed);
  EXPECT_EQ(1u, index.books.size()) << "a missing card must not wipe the library";
}

TEST(ShelfIndexer, FailedMetadataFallsBackToFilename) {
  shelftest::TempDir dir("indexer_fail");
  writeFile(dir.join("books/broken.epub"), "not a zip");

  FakeMeta ctx;
  auto failingMetadata = [](void* context, const std::string&, shelf::BookFormat, shelf::BookMeta&) {
    ++static_cast<FakeMeta*>(context)->calls;
    return false;
  };
  (void)fakeMetadata;
  shelf::ShelfIndexer indexer(failingMetadata, &ctx);
  shelf::ShelfIndex index;
  indexer.scan(index, optionsFor(dir));

  ASSERT_EQ(1u, index.books.size());
  EXPECT_FALSE(index.books.front().metaOk);
  EXPECT_EQ("broken", index.books.front().meta.title);
}
