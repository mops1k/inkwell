#include "FirmwareManifestParser.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace firmware_manifest {

void formatSha256Hex(const uint8_t* digest, size_t digestLen, char* out, size_t outSize) {
  static constexpr char hexDigits[] = "0123456789abcdef";
  if (!out || outSize < 2) return;
  const size_t maxChars = outSize - 1;
  size_t written = 0;
  for (size_t i = 0; i < digestLen && written + 1 < maxChars; ++i) {
    out[written++] = hexDigits[digest[i] >> 4];
    out[written++] = hexDigits[digest[i] & 0x0F];
  }
  out[written] = '\0';
}

bool sha256HexEquals(const char* actual, const char* expected) {
  if (!actual || !expected) return false;
  if (strlen(actual) != FirmwareManifestParser::kShaHexLength) return false;
  if (strlen(expected) != FirmwareManifestParser::kShaHexLength) return false;
  for (size_t i = 0; i < FirmwareManifestParser::kShaHexLength; ++i) {
    if (tolower(static_cast<unsigned char>(actual[i])) != tolower(static_cast<unsigned char>(expected[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace firmware_manifest

namespace {

void copyLimited(char* dst, size_t dstSize, const char* src, size_t srcLen) {
  const size_t n = srcLen < dstSize - 1 ? srcLen : dstSize - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

bool isHexChar(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool isSha256Hex(const char* text) {
  const size_t len = strlen(text);
  if (len != FirmwareManifestParser::kShaHexLength) return false;
  for (size_t i = 0; i < len; ++i) {
    if (!isHexChar(text[i])) return false;
  }
  return true;
}

}  // namespace

FirmwareManifestParser::FirmwareManifestParser(const char* const* devices, size_t deviceCount)
    : parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                           sOnArrayStart, sOnArrayEnd}),
      supportedDevices(devices),
      supportedDeviceCount(deviceCount) {
  reset();
}

void FirmwareManifestParser::reset() {
  parser.reset();
  position = Position::TOP_LEVEL;
  lastKey = LastKey::NONE;
  depth = 0;
  applicationDepth = 0;
  inDevicesArray = false;
  versionText[0] = '\0';
  versionFound = false;
  selectedValid = false;
  selectedFileText[0] = '\0';
  selectedShaText[0] = '\0';
  selectedSize = 0;
  resetApplication();
}

void FirmwareManifestParser::resetApplication() {
  applicationFile[0] = '\0';
  applicationSha[0] = '\0';
  applicationBytes = 0;
  applicationDeviceMatch = false;
}

void FirmwareManifestParser::feed(const char* data, size_t len) { parser.feed(data, len); }

bool FirmwareManifestParser::inObjectAtDepth(uint8_t wanted) const { return depth == wanted; }

bool FirmwareManifestParser::deviceMatches(const char* value, size_t len) const {
  for (size_t i = 0; i < supportedDeviceCount; ++i) {
    const char* name = supportedDevices[i];
    if (!name) continue;
    const size_t nameLen = strlen(name);
    if (nameLen == len && memcmp(name, value, len) == 0) return true;
  }
  return false;
}

void FirmwareManifestParser::finalizeApplication() {
  // Fail closed: a matching device is not enough, the entry must carry a
  // well-formed SHA-256 and a file name, otherwise it cannot be verified.
  if (!selectedValid && applicationDeviceMatch && isSha256Hex(applicationSha) && applicationFile[0] != '\0') {
    copyLimited(selectedFileText, sizeof(selectedFileText), applicationFile, strlen(applicationFile));
    copyLimited(selectedShaText, sizeof(selectedShaText), applicationSha, strlen(applicationSha));
    selectedSize = applicationBytes;
    selectedValid = true;
  }
  resetApplication();
}

void FirmwareManifestParser::sOnKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<FirmwareManifestParser*>(ctx);
  self->lastKey = LastKey::NONE;

  if (self->position == Position::TOP_LEVEL && self->inObjectAtDepth(1)) {
    if (len == 7 && memcmp(key, "version", 7) == 0) {
      self->lastKey = LastKey::VERSION;
    } else if (len == 12 && memcmp(key, "applications", 12) == 0) {
      self->lastKey = LastKey::APPLICATIONS;
    }
    return;
  }

  if (self->position == Position::IN_APPLICATION && self->applicationDepth == 1) {
    if (len == 4 && memcmp(key, "file", 4) == 0) {
      self->lastKey = LastKey::FILE;
    } else if (len == 6 && memcmp(key, "sha256", 6) == 0) {
      self->lastKey = LastKey::SHA256;
    } else if (len == 7 && memcmp(key, "devices", 7) == 0) {
      self->lastKey = LastKey::DEVICES;
    } else if (len == 5 && memcmp(key, "bytes", 5) == 0) {
      self->lastKey = LastKey::BYTES;
    }
  }
}

void FirmwareManifestParser::sOnString(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<FirmwareManifestParser*>(ctx);
  const LastKey key = self->lastKey;
  self->lastKey = LastKey::NONE;

  if (key == LastKey::VERSION && self->position == Position::TOP_LEVEL && self->inObjectAtDepth(1)) {
    copyLimited(self->versionText, sizeof(self->versionText), value, len);
    self->versionFound = true;
    return;
  }

  if (self->position != Position::IN_APPLICATION) return;

  if (self->inDevicesArray && self->applicationDepth == 1) {
    if (self->deviceMatches(value, len)) self->applicationDeviceMatch = true;
    return;
  }

  if (key == LastKey::FILE && self->applicationDepth == 1) {
    copyLimited(self->applicationFile, sizeof(self->applicationFile), value, len);
  } else if (key == LastKey::SHA256 && self->applicationDepth == 1) {
    copyLimited(self->applicationSha, sizeof(self->applicationSha), value, len);
  }
}

void FirmwareManifestParser::sOnNumber(void* ctx, const char* value, size_t /*len*/) {
  auto* self = static_cast<FirmwareManifestParser*>(ctx);
  const LastKey key = self->lastKey;
  self->lastKey = LastKey::NONE;

  if (key == LastKey::BYTES && self->position == Position::IN_APPLICATION && self->applicationDepth == 1) {
    self->applicationBytes = static_cast<size_t>(strtoul(value, nullptr, 10));
  }
}

void FirmwareManifestParser::sOnBool(void* ctx, bool /*value*/) {
  static_cast<FirmwareManifestParser*>(ctx)->lastKey = LastKey::NONE;
}

void FirmwareManifestParser::sOnNull(void* ctx) { static_cast<FirmwareManifestParser*>(ctx)->lastKey = LastKey::NONE; }

void FirmwareManifestParser::sOnObjectStart(void* ctx) {
  auto* self = static_cast<FirmwareManifestParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      self->depth++;
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_APPLICATIONS:
      self->position = Position::IN_APPLICATION;
      self->applicationDepth = 1;
      self->inDevicesArray = false;
      self->resetApplication();
      self->lastKey = LastKey::NONE;
      break;
    case Position::IN_APPLICATION:
      self->applicationDepth++;
      self->lastKey = LastKey::NONE;
      break;
  }
}

void FirmwareManifestParser::sOnObjectEnd(void* ctx) {
  auto* self = static_cast<FirmwareManifestParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth > 0) self->depth--;
      break;
    case Position::IN_APPLICATION:
      if (self->applicationDepth > 0) self->applicationDepth--;
      if (self->applicationDepth == 0) {
        self->finalizeApplication();
        self->position = Position::IN_APPLICATIONS;
      }
      break;
    default:
      break;
  }
  self->lastKey = LastKey::NONE;
}

void FirmwareManifestParser::sOnArrayStart(void* ctx) {
  auto* self = static_cast<FirmwareManifestParser*>(ctx);

  if (self->position == Position::TOP_LEVEL && self->inObjectAtDepth(1) && self->lastKey == LastKey::APPLICATIONS) {
    self->position = Position::IN_APPLICATIONS;
  } else if (self->position == Position::IN_APPLICATION && self->applicationDepth == 1 &&
             self->lastKey == LastKey::DEVICES) {
    self->inDevicesArray = true;
  } else if (self->position == Position::TOP_LEVEL) {
    self->depth++;
  } else if (self->position == Position::IN_APPLICATION) {
    self->applicationDepth++;
  }
  self->lastKey = LastKey::NONE;
}

void FirmwareManifestParser::sOnArrayEnd(void* ctx) {
  auto* self = static_cast<FirmwareManifestParser*>(ctx);

  switch (self->position) {
    case Position::TOP_LEVEL:
      if (self->depth > 0) self->depth--;
      break;
    case Position::IN_APPLICATIONS:
      self->position = Position::TOP_LEVEL;
      break;
    case Position::IN_APPLICATION:
      if (self->inDevicesArray) {
        self->inDevicesArray = false;
      } else if (self->applicationDepth > 0) {
        self->applicationDepth--;
      }
      break;
  }
  self->lastKey = LastKey::NONE;
}
