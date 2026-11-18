#include <CoverPolicy.h>
#include <gtest/gtest.h>

#include <cstdint>

namespace {

uint32_t fatStamp(int year, int month, int day, int hour, int minute, int second) {
  const uint32_t date = ((static_cast<uint32_t>(year) - 1980) << 9) | ((static_cast<uint32_t>(month) << 5) & 0x1E0) |
                        (static_cast<uint32_t>(day) & 0x1F);
  const uint32_t time =
      (static_cast<uint32_t>(hour) << 11) | (static_cast<uint32_t>(minute) << 5) | (static_cast<uint32_t>(second) / 2);
  return (date << 16) | time;
}

shelf::CoverThumbState completeThumb(uint32_t fatDateTime) {
  shelf::CoverThumbState state;
  state.exists = true;
  state.complete = true;
  state.fatDateTime = fatDateTime;
  return state;
}

}  // namespace

TEST(ShelfCoverPolicy, BoxesAreFixedAndThreeByFour) {
  const shelf::CoverBox cell = shelf::coverBox(shelf::ShelfCoverSize::Cell);
  EXPECT_EQ(180, cell.width);
  EXPECT_EQ(240, cell.height);
  const shelf::CoverBox card = shelf::coverBox(shelf::ShelfCoverSize::Card);
  EXPECT_EQ(264, card.width);
  EXPECT_EQ(352, card.height);
  EXPECT_EQ(cell.width * 4, cell.height * 3);
  EXPECT_EQ(card.width * 4, card.height * 3);
}

// Pins the 2-bit (4 gray levels) contract from requirement 6: a regression to
// 1 bpp halves the payload.
TEST(ShelfCoverPolicy, TwoBitsPerPixelPayload) {
  EXPECT_EQ(2, shelf::kCoverBitsPerPixel);
  EXPECT_EQ(10800u, shelf::coverPixelBytes(shelf::coverBox(shelf::ShelfCoverSize::Cell)));
  EXPECT_EQ(23232u, shelf::coverPixelBytes(shelf::coverBox(shelf::ShelfCoverSize::Card)));
}

TEST(ShelfCoverPolicy, MissingOrIncompleteThumbnailIsGenerated) {
  const uint32_t book = fatStamp(2026, 5, 1, 12, 0, 0);
  EXPECT_EQ(shelf::CoverDecision::Generate, shelf::decideCoverAction(book, shelf::CoverThumbState{}));

  shelf::CoverThumbState truncated = completeThumb(book);
  truncated.complete = false;
  EXPECT_EQ(shelf::CoverDecision::Generate, shelf::decideCoverAction(book, truncated));
}

TEST(ShelfCoverPolicy, FreshThumbnailIsReused) {
  const uint32_t book = fatStamp(2026, 5, 1, 12, 0, 20);
  EXPECT_EQ(shelf::CoverDecision::Reuse, shelf::decideCoverAction(book, completeThumb(book)));
  // Two seconds is the FAT granularity: a one-tick difference is not a change.
  EXPECT_EQ(shelf::CoverDecision::Reuse,
            shelf::decideCoverAction(book, completeThumb(fatStamp(2026, 5, 1, 12, 0, 18))));
  EXPECT_EQ(shelf::CoverDecision::Reuse,
            shelf::decideCoverAction(book, completeThumb(fatStamp(2026, 5, 1, 12, 0, 30))));
}

TEST(ShelfCoverPolicy, OlderThumbnailIsRegenerated) {
  const uint32_t book = fatStamp(2026, 5, 1, 12, 0, 20);
  EXPECT_EQ(shelf::CoverDecision::Generate,
            shelf::decideCoverAction(book, completeThumb(fatStamp(2026, 5, 1, 12, 0, 10))));
  EXPECT_EQ(shelf::CoverDecision::Generate,
            shelf::decideCoverAction(book, completeThumb(fatStamp(2026, 4, 30, 23, 59, 58))));
}

TEST(ShelfCoverPolicy, UnknownTimestampsNeverChurnTheCache) {
  // A card without usable timestamps must not regenerate every cover forever.
  EXPECT_EQ(shelf::CoverDecision::Reuse, shelf::decideCoverAction(0, completeThumb(0)));
  EXPECT_EQ(shelf::CoverDecision::Reuse, shelf::decideCoverAction(0, completeThumb(fatStamp(2026, 5, 1, 12, 0, 0))));
  EXPECT_EQ(shelf::CoverDecision::Reuse, shelf::decideCoverAction(fatStamp(2026, 5, 1, 12, 0, 0), completeThumb(0)));
}

TEST(ShelfCoverPolicy, SentinelMeansPlaceholder) {
  shelf::CoverThumbState sentinel;
  sentinel.exists = true;
  sentinel.sentinel = true;
  sentinel.complete = false;
  EXPECT_EQ(shelf::CoverDecision::Placeholder, shelf::decideCoverAction(fatStamp(2026, 5, 1, 12, 0, 0), sentinel));
}

TEST(ShelfCoverPolicy, FatTimeOrdersAcrossBoundaries) {
  EXPECT_GT(shelf::fatTimeToSeconds(fatStamp(2026, 12, 31, 23, 59, 58)),
            shelf::fatTimeToSeconds(fatStamp(2026, 12, 31, 23, 59, 0)));
  EXPECT_GT(shelf::fatTimeToSeconds(fatStamp(2027, 1, 1, 0, 0, 0)),
            shelf::fatTimeToSeconds(fatStamp(2026, 12, 31, 23, 59, 58)));
  EXPECT_GT(shelf::fatTimeToSeconds(fatStamp(2026, 2, 1, 0, 0, 0)),
            shelf::fatTimeToSeconds(fatStamp(2026, 1, 31, 23, 59, 58)));
  EXPECT_EQ(0u, shelf::fatTimeToSeconds(0));
}

TEST(ShelfCoverPolicy, FailureDecisions) {
  EXPECT_EQ(shelf::CoverDecision::Reuse, shelf::decideAfterFailure(ThumbResult::Ok, 0));
  EXPECT_EQ(shelf::CoverDecision::Placeholder, shelf::decideAfterFailure(ThumbResult::StructurallyAbsent, 0));

  // First transient failure retries; the second settles on a placeholder.
  EXPECT_EQ(shelf::CoverDecision::RetryLater, shelf::decideAfterFailure(ThumbResult::TransientFail, 0));
  EXPECT_EQ(shelf::CoverDecision::Placeholder, shelf::decideAfterFailure(ThumbResult::TransientFail, 1));
  EXPECT_EQ(shelf::CoverDecision::Placeholder, shelf::decideAfterFailure(ThumbResult::TransientFail, 7));
}
