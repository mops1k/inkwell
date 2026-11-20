// Host tests for firmware-manifest.json handling (plan p_09ab8e3e56a1, Ф2).
//
// The OTA installer takes the expected SHA-256 from the manifest the release
// ships, so the parser must pick only an application that matches the running
// board and carries a well-formed hash and file name. Everything else must fail
// closed: a release that cannot be verified must never look installable.

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>

#include "FirmwareManifestParser.h"

namespace {

constexpr char manifestX3[] = R"({
  "version": "1.1.0",
  "source_commit": "523b3d1c908eca79ebb281c5c54c6741f4a363d4",
  "file": "firmware.bin",
  "bytes": 5114560,
  "sha256": "54f1ba361413367fbf8d24e944eb52f587c9cb68bdae988190d9798544ab34c5",
  "devices": ["X3", "X4"],
  "hardware_tested": false,
  "applications": [
    {
      "file": "firmware-x3.bin",
      "bytes": 5114560,
      "sha256": "54f1ba361413367fbf8d24e944eb52f587c9cb68bdae988190d9798544ab34c5",
      "devices": ["X3"]
    },
    {
      "file": "firmware-x4.bin",
      "bytes": 5114560,
      "sha256": "54f1ba361413367fbf8d24e944eb52f587c9cb68bdae988190d9798544ab34c5",
      "devices": ["X4"]
    }
  ]
})";

constexpr char manifestX4Only[] = R"({
  "version": "1.1.0",
  "applications": [
    {
      "file": "inkwell-1.1.0-x4.bin",
      "bytes": 123,
      "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "devices": ["X4"]
    }
  ]
})";

constexpr char manifestBadSha[] = R"({
  "version": "1.1.0",
  "applications": [
    {
      "file": "inkwell-1.1.0.bin",
      "bytes": 123,
      "sha256": "not-a-sha256",
      "devices": ["X3"]
    },
    {
      "file": "inkwell-1.1.0-truncated.bin",
      "bytes": 123,
      "sha256": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      "devices": ["X3"]
    }
  ]
})";

constexpr char manifestNoFile[] = R"({
  "version": "1.1.0",
  "applications": [
    {
      "bytes": 123,
      "sha256": "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
      "devices": ["X3"]
    }
  ]
})";

constexpr char manifestEmptyApplications[] = R"({"version": "1.1.0", "applications": []})";

constexpr char manifestNoDevices[] = R"({
  "version": "1.1.0",
  "applications": [
    {
      "file": "inkwell-1.1.0.bin",
      "bytes": 123,
      "sha256": "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
    }
  ]
})";

constexpr char manifestAliases[] = R"({
  "version": "1.1.0",
  "file": "inkwell-1.1.0.bin",
  "devices": ["X3", "X4"],
  "applications": [
    {
      "file": "inkwell-1.1.0.bin",
      "bytes": 5042399,
      "sha256": "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
      "devices": ["X3", "X4"]
    },
    {
      "file": "firmware-x3.bin",
      "bytes": 5042399,
      "sha256": "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
      "devices": ["X3"]
    }
  ]
})";

const char* const kX3[] = {"X3"};
const char* const kX4[] = {"X4"};

std::unique_ptr<FirmwareManifestParser> parse(const char* json, const char* const* devices, size_t count,
                                              size_t chunkSize = 0) {
  auto parser = std::make_unique<FirmwareManifestParser>(devices, count);
  const std::string text(json);
  if (chunkSize == 0) {
    parser->feed(text.data(), text.size());
    return parser;
  }
  for (size_t offset = 0; offset < text.size(); offset += chunkSize) {
    const size_t len = std::min(chunkSize, text.size() - offset);
    parser->feed(text.data() + offset, len);
  }
  return parser;
}

}  // namespace

TEST(FirmwareManifestParser, SelectsApplicationForCurrentBoard) {
  const auto parser = parse(manifestX3, kX3, 1);

  ASSERT_TRUE(parser->hasSelected());
  EXPECT_STREQ(parser->selectedFile(), "firmware-x3.bin");
  EXPECT_STREQ(parser->selectedSha256(), "54f1ba361413367fbf8d24e944eb52f587c9cb68bdae988190d9798544ab34c5");
  EXPECT_EQ(parser->selectedBytes(), 5114560u);
}

TEST(FirmwareManifestParser, SelectsX4ApplicationWhenRunningOnX4) {
  const auto parser = parse(manifestX3, kX4, 1);

  ASSERT_TRUE(parser->hasSelected());
  EXPECT_STREQ(parser->selectedFile(), "firmware-x4.bin");
}

TEST(FirmwareManifestParser, SkipsReleaseWithoutOurBoard) {
  const auto parser = parse(manifestX4Only, kX3, 1);

  EXPECT_FALSE(parser->hasSelected());
}

TEST(FirmwareManifestParser, RejectsMalformedSha256) {
  const auto parser = parse(manifestBadSha, kX3, 1);

  // The first entry is skipped (sha256 is not 64 hex chars); the second is
  // selected, so a broken neighbour cannot block a valid application.
  ASSERT_TRUE(parser->hasSelected());
  EXPECT_STREQ(parser->selectedFile(), "inkwell-1.1.0-truncated.bin");
}

TEST(FirmwareManifestParser, RejectsApplicationWithoutFileName) {
  const auto parser = parse(manifestNoFile, kX3, 1);

  EXPECT_FALSE(parser->hasSelected());
}

TEST(FirmwareManifestParser, ReadsManifestVersion) {
  const auto parser = parse(manifestX3, kX3, 1);

  ASSERT_TRUE(parser->hasVersion());
  EXPECT_STREQ(parser->version(), "1.1.0");
}

TEST(FirmwareManifestParser, ChunkedFeedMatchesSingleFeed) {
  for (const size_t chunk : {size_t{1}, size_t{5}, size_t{64}}) {
    const auto parser = parse(manifestX3, kX3, 1, chunk);
    ASSERT_TRUE(parser->hasSelected()) << "chunk size " << chunk;
    EXPECT_STREQ(parser->selectedFile(), "firmware-x3.bin") << "chunk size " << chunk;
    EXPECT_STREQ(parser->version(), "1.1.0") << "chunk size " << chunk;
  }
}

TEST(FirmwareManifestParser, EmptyApplicationsArraySelectsNothing) {
  const auto parser = parse(manifestEmptyApplications, kX3, 1);

  EXPECT_FALSE(parser->hasSelected());
  EXPECT_TRUE(parser->hasVersion());
}

TEST(FirmwareManifestParser, SelectsPrimaryAssetWhenManifestListsAliases) {
  // scripts/generate_manifest.py puts the inkwell-<version>.bin entry first,
  // because the installer requires the downloaded asset to be named in the
  // manifest; the model-named aliases that follow carry the same bytes.
  const auto parser = parse(manifestAliases, kX3, 1);

  ASSERT_TRUE(parser->hasSelected());
  EXPECT_STREQ(parser->selectedFile(), "inkwell-1.1.0.bin");
}

TEST(FirmwareManifestParser, ApplicationWithoutDevicesIsNotSelectable) {
  // An entry that names no board cannot claim to fit ours: the device must not
  // fall back to "any image" when the manifest is malformed.
  const auto parser = parse(manifestNoDevices, kX3, 1);

  EXPECT_FALSE(parser->hasSelected());
}

TEST(FirmwareManifestParser, FormatsDigestAsLowerCaseHex) {
  const uint8_t digest[] = {0x00, 0x0F, 0xA5, 0xFF};
  char out[9] = {};
  firmware_manifest::formatSha256Hex(digest, sizeof(digest), out, sizeof(out));

  EXPECT_STREQ(out, "000fa5ff");
}

TEST(FirmwareManifestParser, ComparesSha256CaseInsensitively) {
  const char* lower = "54f1ba361413367fbf8d24e944eb52f587c9cb68bdae988190d9798544ab34c5";
  const char* upper = "54F1BA361413367FBF8D24E944EB52F587C9CB68BDAE988190D9798544AB34C5";

  EXPECT_TRUE(firmware_manifest::sha256HexEquals(lower, upper));
  EXPECT_FALSE(
      firmware_manifest::sha256HexEquals(lower, "54f1ba361413367fbf8d24e944eb52f587c9cb68bdae988190d9798544ab34c4"));
  EXPECT_FALSE(firmware_manifest::sha256HexEquals(lower, "54f1ba"));
}
