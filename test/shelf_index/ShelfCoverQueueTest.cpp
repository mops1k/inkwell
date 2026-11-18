#include <gtest/gtest.h>

#include "CoverQueue.h"

namespace {

shelf::ShelfCoverSize cell = shelf::ShelfCoverSize::Cell;
shelf::ShelfCoverSize card = shelf::ShelfCoverSize::Card;

}  // namespace

TEST(ShelfCoverQueue, ServesRequestsInOrderWithTheirSize) {
  shelf::CoverQueue queue;
  ASSERT_TRUE(queue.push("/books/a.epub", cell));
  ASSERT_TRUE(queue.push("/books/b.fb2.zip", card));
  EXPECT_EQ(2u, queue.size());

  std::string path;
  shelf::ShelfCoverSize size = cell;
  ASSERT_TRUE(queue.pop(path, size));
  EXPECT_EQ("/books/a.epub", path);
  EXPECT_EQ(cell, size);

  ASSERT_TRUE(queue.pop(path, size));
  EXPECT_EQ("/books/b.fb2.zip", path);
  EXPECT_EQ(card, size);

  EXPECT_TRUE(queue.empty());
  EXPECT_FALSE(queue.pop(path, size));
}

TEST(ShelfCoverQueue, OneRequestPerBook) {
  shelf::CoverQueue queue;
  ASSERT_TRUE(queue.push("/books/a.epub", cell));
  // A repaint or a page return asks again before the first decode finished; the
  // worker must not decode the same cover twice.
  EXPECT_FALSE(queue.push("/books/a.epub", cell));
  EXPECT_EQ(1u, queue.size());
}

TEST(ShelfCoverQueue, DropsEmptyPathsAndOverflow) {
  shelf::CoverQueue queue;
  EXPECT_FALSE(queue.push("", cell));
  for (size_t i = 0; i < shelf::CoverQueue::kMaxPending; ++i) {
    ASSERT_TRUE(queue.push("/books/" + std::to_string(i) + ".epub", cell)) << i;
  }
  EXPECT_FALSE(queue.push("/books/overflow.epub", cell));
  EXPECT_EQ(shelf::CoverQueue::kMaxPending, queue.size());
}

TEST(ShelfCoverQueue, EraseForgetsOneBookOnly) {
  shelf::CoverQueue queue;
  ASSERT_TRUE(queue.push("/books/a.epub", cell));
  ASSERT_TRUE(queue.push("/books/b.epub", cell));
  queue.erase("/books/a.epub");

  std::string path;
  shelf::ShelfCoverSize size = cell;
  ASSERT_TRUE(queue.pop(path, size));
  EXPECT_EQ("/books/b.epub", path);
  EXPECT_TRUE(queue.empty());
}

TEST(ShelfCoverQueue, ClearDropsEverything) {
  shelf::CoverQueue queue;
  ASSERT_TRUE(queue.push("/books/a.epub", cell));
  queue.clear();
  EXPECT_TRUE(queue.empty());
  EXPECT_TRUE(queue.push("/books/a.epub", cell));
}
