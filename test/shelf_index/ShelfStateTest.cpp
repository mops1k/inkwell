#include <gtest/gtest.h>

#include <string>

#include "ShelfStateStore.h"
#include "TempDir.h"

TEST(ShelfState, RoundTripsFilterAndSort) {
  shelf::LibraryState state;
  state.filter = {shelf::FilterField::Author, "Лев Толстой"};
  state.sort = shelf::SortKey::Author;

  std::string payload;
  shelf::serializeLibraryState(state, payload);

  shelf::LibraryState parsed;
  ASSERT_TRUE(shelf::parseLibraryState(payload, parsed));
  EXPECT_EQ(shelf::FilterField::Author, parsed.filter.field);
  EXPECT_EQ("Лев Толстой", parsed.filter.value);
  EXPECT_EQ(shelf::SortKey::Author, parsed.sort);
}

TEST(ShelfState, RoundTripsHiddenBooks) {
  shelf::LibraryState state;
  state.hiddenPaths = {"/books/system/readme.txt", "/books/notes.txt"};
  std::string payload;
  shelf::serializeLibraryState(state, payload);

  shelf::LibraryState parsed;
  ASSERT_TRUE(shelf::parseLibraryState(payload, parsed));
  ASSERT_EQ(parsed.hiddenPaths.size(), 2u);
  EXPECT_EQ("/books/system/readme.txt", parsed.hiddenPaths[0]);
  EXPECT_EQ("/books/notes.txt", parsed.hiddenPaths[1]);
}

TEST(ShelfState, NoFilterRoundTripsAsEmpty) {
  shelf::LibraryState state;
  state.filter.clear();
  std::string payload;
  shelf::serializeLibraryState(state, payload);

  shelf::LibraryState parsed;
  parsed.filter = {shelf::FilterField::Genre, "stale"};
  ASSERT_TRUE(shelf::parseLibraryState(payload, parsed));
  EXPECT_FALSE(parsed.filter.active());
  EXPECT_EQ(shelf::SortKey::Title, parsed.sort);
}

TEST(ShelfState, GarbageIsRejected) {
  shelf::LibraryState parsed;
  EXPECT_FALSE(shelf::parseLibraryState("", parsed));
  // A minimal document is a valid empty state ("no filter, sort by title").
  EXPECT_TRUE(shelf::parseLibraryState("{\"version\":1}\n", parsed));
  EXPECT_FALSE(parsed.filter.active());
  // Without a version the document is not ours.
  EXPECT_FALSE(shelf::parseLibraryState("{\"field\":\"author\",\"value\":\"x\",\"sort\":\"title\"}\n", parsed));
}

TEST(ShelfState, SavesAndLoadsOnDisk) {
  shelftest::TempDir dir("state_roundtrip");
  shelf::ShelfStateStore store(dir.join("shelf"));

  shelf::LibraryState state;
  state.filter = {shelf::FilterField::Series, "Хроники Дюны"};
  state.sort = shelf::SortKey::Author;
  ASSERT_TRUE(store.save(state));

  shelf::LibraryState loaded;
  ASSERT_TRUE(store.load(loaded));
  EXPECT_EQ(shelf::FilterField::Series, loaded.filter.field);
  EXPECT_EQ("Хроники Дюны", loaded.filter.value);
  EXPECT_EQ(shelf::SortKey::Author, loaded.sort);
}

TEST(ShelfState, MissingFileKeepsDefaultState) {
  shelftest::TempDir dir("state_missing");
  shelf::ShelfStateStore store(dir.join("shelf"));

  shelf::LibraryState state;
  state.filter = {shelf::FilterField::Author, "x"};
  EXPECT_FALSE(store.load(state));
  EXPECT_EQ(shelf::FilterField::Author, state.filter.field);  // untouched on failure
}
