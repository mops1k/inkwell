#pragma once

#include <cstddef>
#include <cstdint>

#include "StreamingJsonParser.h"

// Ported from crosspoint-reader/crosspoint-reader (MIT),
// initially authored in PR #1810 by znelson and contributors.
class ReleaseJsonParser {
 public:
  ReleaseJsonParser();

  ReleaseJsonParser(const ReleaseJsonParser&) = delete;
  ReleaseJsonParser& operator=(const ReleaseJsonParser&) = delete;

  void reset();
  void feed(const char* data, size_t len);

  bool foundTag() const;
  bool foundFirmware() const;
  bool foundManifest() const;
  bool foundDigest() const;
  const char* getTagName() const;
  const char* getFirmwareUrl() const;
  size_t getFirmwareSize() const;
  const char* getManifestUrl() const;
  size_t getManifestSize() const;
  // SHA-256 of the selected firmware asset as published by GitHub's release
  // API (`digest: "sha256:<hex>"`), normalised to 64 hex characters. Empty when
  // the release does not carry digests.
  const char* getFirmwareDigest() const;

 private:
  bool inReleaseObject() const;

  enum class Position : uint8_t {
    TOP_LEVEL,
    IN_ASSETS_ARRAY,
    IN_ASSET_OBJECT,
  };

  enum class LastKey : uint8_t {
    NONE,
    TAG_NAME,
    ASSETS,
    ASSET_NAME,
    ASSET_URL,
    ASSET_SIZE,
    ASSET_DIGEST,
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void commitAsset();

  StreamingJsonParser parser;

  Position position;
  LastKey lastKey;
  uint8_t depth;
  uint8_t assetDepth;

  char tagName[32];
  char firmwareUrl[512];
  size_t firmwareSize;
  bool tagFound;
  bool firmwareFound;
  // Our releases publish `inkwell-<version>.bin`; upstream ones publish
  // `firmware.bin`. Prefer the Inkwell asset when a release carries both, so a
  // fork release with a mirrored upstream asset still installs our image.
  bool firmwarePreferred;

  // firmware-manifest.json names the image for each board and pins its
  // SHA-256; the installer refuses to run without it.
  char manifestUrl[512];
  size_t manifestSize;
  bool manifestFound;

  // GitHub's release API also reports `digest` per asset. When present it lets
  // the installer verify the download without a second HTTPS request, which is
  // a meaningful heap saving on this board.
  char firmwareDigest[65];
  bool digestFound;

  char currentAssetName[32];
  char currentAssetUrl[512];
  bool topLevelArray;
  size_t currentAssetSize;
  char currentAssetDigest[80];
};
