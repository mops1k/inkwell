#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

void recordRemoved(void* ctx, const std::string& path) { static_cast<std::vector<std::string>*>(ctx)->push_back(path); }

}  // namespace

TEST(ShelfIndexer, WalksRootTwoLevelsAndBooksRecursively) {
  shelftest::TempDir dir("indexer_walk");
  writeFile(dir.join("top.txt"), "root book");
  writeFile(dir.join("My Downloads/inbox.epub"), "download folder on the card root");
  writeFile(dir.join("books/nested/novel.md"), "nested markdown");
  writeFile(dir.join("books/note.pdf"), "not a book");
  writeFile(dir.join(".hidden-book.txt"), "hidden");
  writeFile(dir.join(".crosspoint/cache/skip.epub"), "cache dir");
  writeFile(dir.join("deep/a/b/too-far.epub"), "third level below root is not walked");

  FakeMeta ctx;
  shelf::ShelfIndexer indexer(fakeMetadata, &ctx);
  shelf::ShelfIndex index;
  const shelf::ShelfScanStats stats = indexer.scan(index, optionsFor(dir));

  EXPECT_EQ(3u, index.books.size());
  EXPECT_EQ(1u, countPaths(index, "top.txt"));
  EXPECT_EQ(1u, countPaths(index, "inbox.epub"));
  EXPECT_EQ(1u, countPaths(index, "novel.md"));
  EXPECT_EQ(0u, countPaths(index, "too-far.epub"));
  EXPECT_EQ(0u, countPaths(index, "hidden"));
  EXPECT_EQ(0u, countPaths(index, ".crosspoint"));
  EXPECT_EQ(3u, stats.refreshed);
  EXPECT_EQ(3, ctx.calls);
}

TEST(ShelfIndexer, CrashReportAtTheCardRootIsNotABook) {
  // Upstream HalSystem writes its diagnostic log to /crash_report.txt; the shelf
  // must never show it as a TXT book (the card filesystem is case-insensitive).
  shelftest::TempDir dir("indexer_crashlog");
  writeFile(dir.join("crash_report.txt"), "Guru Meditation Error: Core 0 panic'ed");
  writeFile(dir.join("CRASH_REPORT.TXT"), "same file, upper-case name");
  writeFile(dir.join("books/good.txt"), "a real book");

  FakeMeta ctx;
  shelf::ShelfIndexer indexer(fakeMetadata, &ctx);
  shelf::ShelfIndex index;
  indexer.scan(index, optionsFor(dir));

  ASSERT_EQ(1u, index.books.size());
  EXPECT_EQ(1u, countPaths(index, "good.txt"));
  EXPECT_EQ(0u, countPaths(index, "crash_report"));
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

TEST(ShelfIndexer, IndexesConfiguredDownloadsFolderRecursively) {
  shelftest::TempDir dir("indexer_downloads");
  writeFile(dir.join("books/classic.txt"), "books");
  writeFile(dir.join("My Downloads/fresh.epub"), "downloaded");
  writeFile(dir.join("My Downloads/series/deep.fb2"), "nested download");

  FakeMeta ctx;
  shelf::ShelfIndexer indexer(fakeMetadata, &ctx);
  shelf::ShelfIndex index;
  shelf::ShelfScanOptions options = optionsFor(dir);
  options.downloadsRoot = dir.join("My Downloads");
  indexer.scan(index, options);

  EXPECT_EQ(3u, index.books.size());
  EXPECT_EQ(1u, countPaths(index, "fresh.epub"));
  // Subfolders inside the download folder are walked, not just its top level.
  EXPECT_EQ(1u, countPaths(index, "My Downloads/series/deep.fb2"));
}

TEST(ShelfIndexer, DownloadsFolderInsideBooksIsNotDuplicated) {
  shelftest::TempDir dir("indexer_downloads_overlap");
  writeFile(dir.join("books/downloads/a.epub"), "one file, two roots");

  FakeMeta ctx;
  shelf::ShelfIndexer indexer(fakeMetadata, &ctx);
  shelf::ShelfIndex index;
  shelf::ShelfScanOptions options = optionsFor(dir);
  options.downloadsRoot = dir.join("books/downloads");
  indexer.scan(index, options);

  EXPECT_EQ(1u, index.books.size());
  EXPECT_EQ(1u, countPaths(index, "a.epub"));
}

TEST(ShelfIndexer, MissingDownloadsFolderIsHarmless) {
  shelftest::TempDir dir("indexer_downloads_missing");
  writeFile(dir.join("books/a.md"), "one");

  FakeMeta ctx;
  shelf::ShelfIndexer indexer(fakeMetadata, &ctx);
  shelf::ShelfIndex index;
  shelf::ShelfScanOptions options = optionsFor(dir);
  options.downloadsRoot = dir.join("not-created-yet");
  const shelf::ShelfScanStats stats = indexer.scan(index, options);

  EXPECT_EQ(1u, index.books.size());
  EXPECT_EQ(0u, stats.removed);
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

// The shelf removes a deleted book's thumbnail cache, so the indexer has to name
// exactly the books that disappeared — not the ones that stayed or the new ones.
TEST(ShelfIndexer, ReportsRemovedBooksOnly) {
  shelftest::TempDir dir("indexer_report_removed");
  writeFile(dir.join("books/a.md"), "one");
  writeFile(dir.join("books/b.md"), "two");

  FakeMeta ctx;
  std::vector<std::string> removedPaths;
  shelf::ShelfIndexer indexer(fakeMetadata, &ctx, recordRemoved, &removedPaths);
  shelf::ShelfIndex index;
  indexer.scan(index, optionsFor(dir));
  ASSERT_TRUE(removedPaths.empty()) << "nothing was removed on the first scan";

  std::filesystem::remove(dir.join("books/b.md"));
  writeFile(dir.join("books/c.md"), "three");
  indexer.scan(index, optionsFor(dir));

  ASSERT_EQ(1u, removedPaths.size());
  EXPECT_NE(std::string::npos, removedPaths.front().find("b.md"));
  EXPECT_EQ(2u, index.books.size());
}
