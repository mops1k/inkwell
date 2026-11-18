#include <gtest/gtest.h>

#include "BookFormat.h"

TEST(ShelfFormatArtwork, PictureBearingFormatsGetACover) {
  // The shelf asked the cover pipeline for every format, and a plain .txt produced a
  // garbage bitmap that was drawn as a black block. Only formats that can carry a
  // picture may be asked at all.
  EXPECT_TRUE(shelf::formatHasArtwork(shelf::BookFormat::Epub));
  EXPECT_TRUE(shelf::formatHasArtwork(shelf::BookFormat::Fb2));
  EXPECT_TRUE(shelf::formatHasArtwork(shelf::BookFormat::Fb2Zip));
  EXPECT_TRUE(shelf::formatHasArtwork(shelf::BookFormat::Xtc));
}

TEST(ShelfFormatArtwork, TextFormatsHaveNoArtwork) {
  EXPECT_FALSE(shelf::formatHasArtwork(shelf::BookFormat::Txt));
  EXPECT_FALSE(shelf::formatHasArtwork(shelf::BookFormat::Markdown));
  EXPECT_FALSE(shelf::formatHasArtwork(shelf::BookFormat::Unknown));
}
