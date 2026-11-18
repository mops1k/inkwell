// Host stubs for firmware subsystems the simulator does not exercise but the
// real GfxRenderer.cpp links against: the SD-card font pipeline (off in the
// simulator, which draws built-in fonts) and the compressed-font decompressor.
// Every stub keeps the device contract's benign outcome: "no glyph" / "no scan".
#include <EpdFontData.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <SdCardFont.h>

bool FontCacheManager::isScanning() const { return false; }

void FontCacheManager::recordText(const char*, int, EpdFontFamily::Style) {}

const uint8_t* FontDecompressor::getBitmap(const EpdFontData*, const EpdGlyph*, uint32_t) { return nullptr; }

const uint8_t* SdCardFont::getGlyphBitmap(const EpdFontData*, const EpdGlyph*) { return nullptr; }

int SdCardFont::prewarm(const char*, uint8_t, bool, bool) { return 0; }

void SdCardFont::clearAccumulation() {}

void SdCardFont::unloadMetadata() {}

bool SdCardFont::reloadMetadata() { return false; }
