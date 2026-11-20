#pragma once

#include <cstddef>
#include <cstdint>

#include "StreamingJsonParser.h"

namespace firmware_manifest {

// Hex rendering and case-insensitive comparison of a SHA-256 digest. Living
// next to the manifest parser means the OTA installer and the host tests share
// one implementation instead of two subtly different copies.
void formatSha256Hex(const uint8_t* digest, size_t digestLen, char* out, size_t outSize);
bool sha256HexEquals(const char* actual, const char* expected);

}  // namespace firmware_manifest

// Parses firmware-manifest.json — the format upstream Paperio Reader ships next
// to its images:
//   {
//     "version": "1.1.0",
//     "applications": [
//       {"file": "firmware-x3.bin", "sha256": "<64 hex>", "bytes": 5114560,
//        "devices": ["X3", "X4"]}
//     ]
//   }
//
// Only the applications array is consumed. For every entry the parser records
// whether one of its `devices` matches a board this firmware supports; the first
// matching entry with a valid SHA-256 becomes the selected application. All
// buffers are fixed-size and parsing is streaming, so this runs on the device
// while the manifest travels through the HTTP downloader.
class FirmwareManifestParser {
 public:
  static constexpr size_t kShaHexLength = 64;
  static constexpr size_t kFileNameMax = 80;
  static constexpr size_t kVersionMax = 32;

  FirmwareManifestParser(const char* const* devices, size_t deviceCount);

  FirmwareManifestParser(const FirmwareManifestParser&) = delete;
  FirmwareManifestParser& operator=(const FirmwareManifestParser&) = delete;

  void reset();
  void feed(const char* data, size_t len);

  bool hasSelected() const { return selectedValid; }
  const char* selectedFile() const { return selectedFileText; }
  const char* selectedSha256() const { return selectedShaText; }
  size_t selectedBytes() const { return selectedSize; }
  bool hasVersion() const { return versionFound; }
  const char* version() const { return versionText; }

 private:
  enum class Position : uint8_t { TOP_LEVEL, IN_APPLICATIONS, IN_APPLICATION };
  enum class LastKey : uint8_t { NONE, VERSION, APPLICATIONS, FILE, SHA256, DEVICES, BYTES };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  bool inObjectAtDepth(uint8_t wanted) const;
  bool deviceMatches(const char* value, size_t len) const;
  void resetApplication();
  void finalizeApplication();

  StreamingJsonParser parser;
  const char* const* supportedDevices;
  size_t supportedDeviceCount;

  Position position;
  LastKey lastKey;
  uint8_t depth;
  uint8_t applicationDepth;
  bool inDevicesArray;

  char versionText[kVersionMax + 1];
  bool versionFound;

  char applicationFile[kFileNameMax + 1];
  char applicationSha[kShaHexLength + 1];
  size_t applicationBytes;
  bool applicationDeviceMatch;

  bool selectedValid;
  char selectedFileText[kFileNameMax + 1];
  char selectedShaText[kShaHexLength + 1];
  size_t selectedSize;
};
