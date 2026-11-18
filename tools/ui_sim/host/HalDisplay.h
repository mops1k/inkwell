#pragma once

// Host stand-in for lib/hal/HalDisplay.h. The shelf simulator must exercise the
// REAL GfxRenderer pixel path, so this header reproduces the device interface
// (same names, same buffer conventions) while keeping the frame in host memory:
// displayBuffer()/triggerDisplay()/displayWindow() snapshot the framebuffer and
// the PNG writer picks it up. 1bpp, MSB-first, a SET bit is WHITE — identical to
// FreeInkDisplay's convention, so firmware drawing code needs no changes.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

class HalDisplay {
 public:
  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };

  static constexpr uint16_t DISPLAY_WIDTH = 792;  // X3 panel, landscape-native
  static constexpr uint16_t DISPLAY_HEIGHT = 528;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  HalDisplay() {
    buffer_.assign(BUFFER_SIZE, 0xFF);
    grayscale_.assign(BUFFER_SIZE, 0xFF);
  }
  ~HalDisplay() = default;

  void begin(bool = false) {}
  void clearScreen(uint8_t color = 0xFF) const {
    std::memset(const_cast<uint8_t*>(buffer_.data()), color, buffer_.size());
  }
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool = false) const {
    blit(imageData, x, y, w, h, false);
  }
  void drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            bool = false) const {
    blit(imageData, x, y, w, h, true);
  }

  void displayBuffer(RefreshMode = FAST_REFRESH, bool = false) { capture(); }
  void refreshDisplay(RefreshMode = FAST_REFRESH, bool = false) { capture(); }
  void displayWindow(uint16_t, uint16_t, uint16_t, uint16_t, bool = false) { capture(); }
  void triggerDisplay(RefreshMode = FAST_REFRESH, bool = false) { capture(); }
  void completeDisplay() {}
  void triggerDisplayAsync(RefreshMode = FAST_REFRESH, bool = false) { capture(); }
  void finishDisplayAsync() {}
  bool isRefreshPending() const { return false; }
  bool isRedRamSynced() const { return true; }
  RefreshMode getLastRefreshMode() const { return FAST_REFRESH; }
  uint8_t getLastDisplayModeByte() const { return 0x0C; }
  void requestResync(uint8_t = 0) {}
  void setDarkMode(bool enabled) { darkMode_ = enabled; }
  bool isDarkMode() const { return darkMode_; }
  void deepSleep() {}

  uint8_t* getFrameBuffer() const { return const_cast<uint8_t*>(buffer_.data()); }
  void syncWriteBufferFromActive() const {}
  void releaseBuffers() {}
  bool releaseSecondaryBuffer() { return false; }
  bool reallocSecondaryBuffer() { return true; }
  bool reallocBuffers() { return true; }
  bool hasSecondaryBuffer() const { return true; }
  uint8_t* borrowSecondaryBuffer(size_t* size) {
    if (size) *size = grayscale_.size();
    return grayscale_.data();
  }
  bool returnSecondaryBuffer() { return true; }
  void setSingleBufferFastDiff(bool) {}

  void copyGrayscaleBuffers(const uint8_t*, const uint8_t*) {}
  void copyGrayscaleLsbBuffers(const uint8_t*) {}
  void copyGrayscaleMsbBuffers(const uint8_t*) {}
  void syncRedRamFromFrameBuffer() {}
  void cleanupGrayscaleBuffers(const uint8_t*) {}
  void cleanupGrayscaleWithPreviousBuffer() {}
  void displayGrayBuffer(bool = false) { capture(); }

  bool deviceIsX3() const { return true; }
  void setFastGrayscaleLut(bool fast) { fastLut_ = fast; }
  bool getFastGrayscaleLut() const { return fastLut_; }

  uint16_t getDisplayWidth() const { return DISPLAY_WIDTH; }
  uint16_t getDisplayHeight() const { return DISPLAY_HEIGHT; }
  uint16_t getDisplayWidthBytes() const { return DISPLAY_WIDTH_BYTES; }
  uint32_t getBufferSize() const { return BUFFER_SIZE; }

  // Simulator-only: the last frame handed to the "panel".
  const std::vector<uint8_t>& lastFrame() const { return captured_; }

 private:
  // 1bpp MSB-first blit, matching the device's HalDisplay semantics closely
  // enough for the simulator: `transparent` keeps the background wherever the
  // source bit is clear.
  void blit(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool transparent) const {
    if (imageData == nullptr) return;
    auto* fb = const_cast<uint8_t*>(buffer_.data());
    const size_t srcRowBytes = (static_cast<size_t>(w) + 7) / 8;
    for (uint16_t row = 0; row < h; ++row) {
      const uint16_t dy = static_cast<uint16_t>(y + row);
      if (dy >= DISPLAY_HEIGHT) break;
      for (uint16_t col = 0; col < w; ++col) {
        const uint8_t bit = static_cast<uint8_t>((imageData[row * srcRowBytes + col / 8] >> (7 - (col % 8))) & 1u);
        if (transparent && bit == 0) continue;
        const uint16_t dx = static_cast<uint16_t>(x + col);
        if (dx >= DISPLAY_WIDTH) continue;
        const size_t index = static_cast<size_t>(dy) * DISPLAY_WIDTH_BYTES + dx / 8;
        const uint8_t mask = static_cast<uint8_t>(0x80u >> (dx % 8));
        if (bit != 0) {
          fb[index] |= mask;
        } else {
          fb[index] = static_cast<uint8_t>(fb[index] & ~mask);
        }
      }
    }
  }

  void capture() { captured_ = buffer_; }

  std::vector<uint8_t> buffer_;
  std::vector<uint8_t> grayscale_;
  std::vector<uint8_t> captured_{std::vector<uint8_t>(BUFFER_SIZE, 0xFF)};
  bool darkMode_ = false;
  bool fastLut_ = false;
};
