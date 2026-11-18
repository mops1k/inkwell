#include "Png.h"

#include <cstdio>
#include <cstring>

namespace uisim {

namespace {

uint32_t crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xFFFFFFFFu;
}

void appendBigEndian32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void appendChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& payload) {
  appendBigEndian32(out, static_cast<uint32_t>(payload.size()));
  const size_t crcStart = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), payload.begin(), payload.end());
  appendBigEndian32(out, crc32(out.data() + crcStart, out.size() - crcStart));
}

// zlib stream made of stored (uncompressed) deflate blocks: no dependency, and
// PNG readers accept it. Screenshots are small, compression is not the goal.
std::vector<uint8_t> zlibStored(const std::vector<uint8_t>& raw) {
  std::vector<uint8_t> out;
  out.push_back(0x78);
  out.push_back(0x01);
  size_t offset = 0;
  constexpr size_t kMaxBlock = 0xFFFF;
  do {
    const size_t chunk = raw.size() - offset < kMaxBlock ? raw.size() - offset : kMaxBlock;
    const bool last = offset + chunk >= raw.size();
    out.push_back(last ? 0x01 : 0x00);
    out.push_back(static_cast<uint8_t>(chunk & 0xFF));
    out.push_back(static_cast<uint8_t>(chunk >> 8));
    const uint16_t nlen = static_cast<uint16_t>(~chunk);
    out.push_back(static_cast<uint8_t>(nlen & 0xFF));
    out.push_back(static_cast<uint8_t>(nlen >> 8));
    out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
               raw.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
    offset += chunk;
  } while (offset < raw.size());
  // Adler-32 of the uncompressed data.
  uint32_t a = 1;
  uint32_t b = 0;
  for (const uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  appendBigEndian32(out, (b << 16) | a);
  return out;
}

uint8_t sourceBit(const std::vector<uint8_t>& frame, int sourceWidthBytes, int x, int y) {
  const uint8_t byte = frame[static_cast<size_t>(y) * sourceWidthBytes + x / 8];
  return static_cast<uint8_t>((byte >> (7 - (x % 8))) & 1u);
}

}  // namespace

bool Png::write(const std::string& path, const std::vector<uint8_t>& frame, int panelWidth, int panelHeight,
                bool rotated) {
  const int width = rotated ? panelHeight : panelWidth;
  const int height = rotated ? panelWidth : panelHeight;
  const int widthBytes = width / 8;
  const int sourceWidthBytes = panelWidth / 8;

  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(height) * (widthBytes + 1));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);  // filter type: none
    for (int byteIndex = 0; byteIndex < widthBytes; ++byteIndex) {
      uint8_t value = 0;
      for (int bit = 0; bit < 8; ++bit) {
        const int x = byteIndex * 8 + bit;
        int sx;
        int sy;
        if (rotated) {
          // The panel frame holds the portrait UI rotated 90 degrees clockwise
          // (GfxRenderer's Portrait on a landscape-native panel), so rotating it
          // the same way puts the page upright.
          sx = y;
          sy = panelHeight - 1 - x;
        } else {
          sx = x;
          sy = y;
        }
        value = static_cast<uint8_t>((value << 1) | sourceBit(frame, sourceWidthBytes, sx, sy));
      }
      raw.push_back(value);
    }
  }

  std::vector<uint8_t> header;
  appendBigEndian32(header, static_cast<uint32_t>(width));
  appendBigEndian32(header, static_cast<uint32_t>(height));
  header.push_back(1);  // bit depth
  header.push_back(0);  // grayscale
  header.push_back(0);  // deflate
  header.push_back(0);  // filter: adaptive
  header.push_back(0);  // no interlace

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  appendChunk(png, "IHDR", header);
  appendChunk(png, "IDAT", zlibStored(raw));
  appendChunk(png, "IEND", {});

  FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) return false;
  const size_t written = std::fwrite(png.data(), 1, png.size(), file);
  std::fclose(file);
  return written == png.size();
}

}  // namespace uisim
