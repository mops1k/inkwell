#include "CoverFixtures.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace uisim {
namespace {

constexpr int kWidth = 180;
constexpr int kHeight = 240;

// The shelf draws covers into the 1-bit framebuffer: gray levels collapse, so
// fixture art is deliberately two-tone. That is also what the device shows for
// real covers, whose 2-bit thumbnails are dithered by the reader pipeline.
class Canvas {
 public:
  Canvas() : pixels_(static_cast<size_t>(kWidth) * kHeight, false) {}

  void set(int x, int y, bool black) {
    if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
    pixels_[static_cast<size_t>(y) * kWidth + x] = black;
  }

  void fillRect(int x, int y, int w, int h, bool black) {
    for (int row = y; row < y + h; ++row) {
      for (int column = x; column < x + w; ++column) set(column, row, black);
    }
  }

  void fillCircle(int cx, int cy, int radius, bool black) {
    for (int y = cy - radius; y <= cy + radius; ++y) {
      for (int x = cx - radius; x <= cx + radius; ++x) {
        const int dx = x - cx;
        const int dy = y - cy;
        if (dx * dx + dy * dy <= radius * radius) set(x, y, black);
      }
    }
  }

  void frame(bool black) {
    fillRect(0, 0, kWidth, 2, black);
    fillRect(0, kHeight - 2, kWidth, 2, black);
    fillRect(0, 0, 2, kHeight, black);
    fillRect(kWidth - 2, 0, 2, kHeight, black);
  }

  void diagonalBand(int from, int thickness, bool black) {
    for (int y = 0; y < kHeight; ++y) {
      for (int dx = 0; dx < thickness; ++dx) set(from + y + dx, y, black);
    }
  }

  bool write(const std::string& path) const {
    const int rowBytes = ((kWidth + 31) / 32) * 4;
    const uint32_t imageSize = static_cast<uint32_t>(rowBytes) * kHeight;
    const uint32_t offBits = 14 + 40 + 8;  // headers + 2-entry palette

    std::vector<uint8_t> row(static_cast<size_t>(rowBytes), 0);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    auto w16 = [&out](uint16_t v) {
      const uint8_t bytes[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
      out.write(reinterpret_cast<const char*>(bytes), 2);
    };
    auto w32 = [&out](uint32_t v) {
      const uint8_t bytes[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                                static_cast<uint8_t>(v >> 24)};
      out.write(reinterpret_cast<const char*>(bytes), 4);
    };

    out.write("BM", 2);
    w32(offBits + imageSize);
    w16(0);
    w16(0);
    w32(offBits);
    w32(40);
    w32(kWidth);
    w32(static_cast<uint32_t>(-kHeight));  // negative height: rows stored top-down
    w16(1);
    w16(1);  // 1 bit per pixel, two-tone
    w32(0);
    w32(imageSize);
    w32(2835);
    w32(2835);
    w32(2);
    w32(0);
    const uint8_t black[4] = {0, 0, 0, 0};
    const uint8_t white[4] = {255, 255, 255, 0};
    out.write(reinterpret_cast<const char*>(black), 4);
    out.write(reinterpret_cast<const char*>(white), 4);

    for (int y = 0; y < kHeight; ++y) {
      for (int x = 0; x < kWidth; ++x) {
        // 1-bit BMP: bit set = palette index 1 = white, so only non-black
        // pixels get their bit set.
        if (pixels_[static_cast<size_t>(y) * kWidth + x]) continue;
        row[static_cast<size_t>(x / 8)] |= static_cast<uint8_t>(0x80u >> (x % 8));  // MSB first
      }
      out.write(reinterpret_cast<const char*>(row.data()), rowBytes);
      std::fill(row.begin(), row.end(), 0);
    }
    return static_cast<bool>(out);
  }

 private:
  std::vector<bool> pixels_;
};

// Six distinct silhouettes: enough to prove the grid draws real artwork per book
// instead of repeating one pattern or falling back to the placeholder frame.
void drawWarAndPeace(Canvas& canvas) {
  canvas.fillRect(0, 0, kWidth, kHeight, true);
  canvas.fillCircle(90, 118, 62, false);
  canvas.fillRect(0, 0, kWidth, 22, false);
  canvas.fillRect(0, 218, kWidth, 22, false);
  canvas.diagonalBand(28, 12, false);
  canvas.frame(false);
}

void drawMobyDick(Canvas& canvas) {
  canvas.fillCircle(76, 98, 48, true);
  canvas.fillRect(118, 62, 44, 26, true);   // tail fin
  canvas.fillRect(40, 132, 30, 16, false);  // eye
  for (int i = 0; i < 5; ++i) canvas.fillRect(0, 172 + i * 13, kWidth, 6, true);
  canvas.frame(true);
}

void drawFathersAndSons(Canvas& canvas) {
  canvas.fillRect(24, 60, 34, 130, true);
  canvas.fillRect(72, 80, 34, 110, true);
  canvas.fillRect(120, 50, 34, 140, true);
  canvas.fillRect(14, 194, 152, 6, true);
  canvas.frame(true);
}

void drawMasterAndMargarita(Canvas& canvas) {
  canvas.fillRect(0, 0, kWidth, kHeight, true);
  canvas.fillCircle(112, 70, 40, false);  // moon
  canvas.fillCircle(90, 152, 52, false);  // cat face
  canvas.fillCircle(74, 104, 10, false);  // ears
  canvas.fillCircle(106, 104, 10, false);
  canvas.fillRect(80, 140, 8, 8, true);
  canvas.fillRect(100, 140, 8, 8, true);
  canvas.fillRect(74, 172, 34, 6, true);
  canvas.frame(false);
}

void drawCrimeAndPunishment(Canvas& canvas) {
  const int widths[] = {18, 30, 12, 26, 20, 34, 14};
  int x = 12;
  for (int i = 0; i < 7; ++i) {
    canvas.fillRect(x, 70 - (i % 3) * 16, widths[i], 170 + (i % 3) * 16, true);
    x += widths[i] + 8;
  }
  canvas.fillRect(0, 0, kWidth, 22, true);
  canvas.frame(true);
}

void drawQuietDon(Canvas& canvas) {
  canvas.fillCircle(130, 70, 34, true);  // sun
  for (int y = 150; y < 240; y += 3) {   // river
    const int x = 20 + (y - 120) / 2;
    canvas.fillRect(x, y, 26, 2, true);
  }
  for (int i = 0; i < 4; ++i) canvas.fillRect(0, 210 + i * 8, kWidth, 3, true);
  canvas.frame(true);
}

}  // namespace

std::vector<std::string> writeFixtureCovers(const std::string& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) return {};

  const std::vector<std::string> names = {"cover_war.bmp",    "cover_moby.bmp",  "cover_fathers.bmp",
                                          "cover_master.bmp", "cover_crime.bmp", "cover_don.bmp"};
  const std::vector<void (*)(Canvas&)> painters = {drawWarAndPeace,        drawMobyDick,           drawFathersAndSons,
                                                   drawMasterAndMargarita, drawCrimeAndPunishment, drawQuietDon};

  std::vector<std::string> paths;
  for (size_t i = 0; i < names.size(); ++i) {
    Canvas canvas;
    painters[i](canvas);
    const std::string path = dir + "/" + names[i];
    if (!canvas.write(path)) return {};
    paths.push_back(path);
  }
  return paths;
}

}  // namespace uisim
