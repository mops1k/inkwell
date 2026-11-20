// Host tests for the OTA release metadata parser (plan p_09ab8e3e56a1, Ф1).
//
// The device installs firmware only from the Inkwell release channel, whose
// images are named `inkwell-<version>.bin`. Upstream releases publish
// `firmware.bin` instead, so the parser must keep accepting that legacy name
// while preferring the Inkwell asset when a release carries both.

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

#include "ReleaseJsonParser.h"

namespace {

constexpr char inkwellRelease[] = R"({
  "tag_name": "1.1.0",
  "assets": [
    {
      "name": "SHA256SUMS",
      "browser_download_url": "https://github.com/mops1k/inkwell/releases/download/1.1.0/SHA256SUMS",
      "size": 96
    },
    {
      "name": "inkwell-1.1.0.bin",
      "browser_download_url": "https://github.com/mops1k/inkwell/releases/download/1.1.0/inkwell-1.1.0.bin",
      "size": 5003537
    }
  ]
})";

constexpr char legacyRelease[] = R"({
  "tag_name": "2.3.2",
  "assets": [
    {
      "name": "firmware.elf",
      "browser_download_url": "https://example.invalid/firmware.elf",
      "size": 9000000
    },
    {
      "name": "firmware.bin",
      "browser_download_url": "https://example.invalid/firmware.bin",
      "size": 5164023
    }
  ]
})";

// A release that mirrors upstream's asset next to ours, in both orders.
constexpr char mixedReleaseLegacyFirst[] = R"({
  "tag_name": "1.2.0",
  "assets": [
    {
      "name": "firmware.bin",
      "browser_download_url": "https://example.invalid/legacy.bin",
      "size": 11
    },
    {
      "name": "inkwell-1.2.0.bin",
      "browser_download_url": "https://example.invalid/inkwell.bin",
      "size": 22
    }
  ]
})";

constexpr char mixedReleaseInkwellFirst[] = R"({
  "tag_name": "1.2.0",
  "assets": [
    {
      "name": "inkwell-1.2.0.bin",
      "browser_download_url": "https://example.invalid/inkwell.bin",
      "size": 22
    },
    {
      "name": "firmware.bin",
      "browser_download_url": "https://example.invalid/legacy.bin",
      "size": 11
    }
  ]
})";

constexpr char irrelevantAssets[] = R"({
  "tag_name": "1.3.0",
  "assets": [
    {"name": "bootloader.bin", "browser_download_url": "https://example.invalid/bootloader.bin", "size": 1},
    {"name": "inkwell-1.3.0.bin.sig", "browser_download_url": "https://example.invalid/sig", "size": 2},
    {"name": "firmware.map", "browser_download_url": "https://example.invalid/map", "size": 3}
  ]
})";

constexpr char releaseList[] = R"([{
  "tag_name": "1.4.0-rc.1",
  "assets": [
    {
      "name": "inkwell-1.4.0-rc.1.bin",
      "browser_download_url": "https://example.invalid/rc.bin",
      "size": 42
    }
  ]
}])";

std::unique_ptr<ReleaseJsonParser> parse(const std::string& json, size_t chunkSize = 0) {
  auto parser = std::make_unique<ReleaseJsonParser>();
  if (chunkSize == 0) {
    parser->feed(json.data(), json.size());
    return parser;
  }
  for (size_t offset = 0; offset < json.size(); offset += chunkSize) {
    const size_t len = std::min(chunkSize, json.size() - offset);
    parser->feed(json.data() + offset, len);
  }
  return parser;
}

}  // namespace

TEST(ReleaseJsonParser, FindsInkwellAsset) {
  const auto parser = parse(inkwellRelease);

  ASSERT_TRUE(parser->foundTag());
  EXPECT_STREQ(parser->getTagName(), "1.1.0");
  ASSERT_TRUE(parser->foundFirmware());
  EXPECT_STREQ(parser->getFirmwareUrl(), "https://github.com/mops1k/inkwell/releases/download/1.1.0/inkwell-1.1.0.bin");
  EXPECT_EQ(parser->getFirmwareSize(), 5003537u);
}

TEST(ReleaseJsonParser, AcceptsLegacyFirmwareBin) {
  const auto parser = parse(legacyRelease);

  ASSERT_TRUE(parser->foundFirmware());
  EXPECT_STREQ(parser->getFirmwareUrl(), "https://example.invalid/firmware.bin");
  EXPECT_EQ(parser->getFirmwareSize(), 5164023u);
}

TEST(ReleaseJsonParser, PrefersInkwellAssetWhenLegacyComesFirst) {
  const auto parser = parse(mixedReleaseLegacyFirst);

  ASSERT_TRUE(parser->foundFirmware());
  EXPECT_STREQ(parser->getFirmwareUrl(), "https://example.invalid/inkwell.bin");
  EXPECT_EQ(parser->getFirmwareSize(), 22u);
}

TEST(ReleaseJsonParser, PrefersInkwellAssetWhenInkwellComesFirst) {
  const auto parser = parse(mixedReleaseInkwellFirst);

  ASSERT_TRUE(parser->foundFirmware());
  EXPECT_STREQ(parser->getFirmwareUrl(), "https://example.invalid/inkwell.bin");
  EXPECT_EQ(parser->getFirmwareSize(), 22u);
}

TEST(ReleaseJsonParser, IgnoresNonFirmwareBinAssets) {
  const auto parser = parse(irrelevantAssets);

  EXPECT_TRUE(parser->foundTag());
  EXPECT_FALSE(parser->foundFirmware());
  EXPECT_STREQ(parser->getFirmwareUrl(), "");
}

TEST(ReleaseJsonParser, ParsesReleaseListArray) {
  const auto parser = parse(releaseList);

  ASSERT_TRUE(parser->foundFirmware());
  EXPECT_STREQ(parser->getTagName(), "1.4.0-rc.1");
  EXPECT_STREQ(parser->getFirmwareUrl(), "https://example.invalid/rc.bin");
}

TEST(ReleaseJsonParser, ChunkedFeedMatchesSingleFeed) {
  for (const size_t chunk : {size_t{1}, size_t{7}, size_t{64}}) {
    const auto streamed = parse(inkwellRelease, chunk);
    ASSERT_TRUE(streamed->foundFirmware()) << "chunk size " << chunk;
    EXPECT_STREQ(streamed->getTagName(), "1.1.0") << "chunk size " << chunk;
    EXPECT_EQ(streamed->getFirmwareSize(), 5003537u) << "chunk size " << chunk;
  }
}

TEST(ReleaseJsonParser, ResetClearsPreviousRelease) {
  auto parser = parse(inkwellRelease);
  ASSERT_TRUE(parser->foundFirmware());

  parser->reset();
  EXPECT_FALSE(parser->foundTag());
  EXPECT_FALSE(parser->foundFirmware());

  parser->feed(legacyRelease, sizeof(legacyRelease) - 1);
  ASSERT_TRUE(parser->foundFirmware());
  EXPECT_STREQ(parser->getFirmwareUrl(), "https://example.invalid/firmware.bin");
}
