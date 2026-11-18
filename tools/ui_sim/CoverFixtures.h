#pragma once

#include <string>
#include <vector>

// Fixture cover art for the host harness. Covers in the real firmware are 2-bit
// BMPs produced by ReaderActivity's thumbnail pipeline, so the harness writes the
// same format here: the shelf renderer then exercises its real drawing path
// (Bitmap + drawBitmap) instead of the placeholder branch.
namespace uisim {

// Writes one 180x240 2-bit BMP per pattern into `dir` and returns their paths.
// An empty vector means the files could not be written.
std::vector<std::string> writeFixtureCovers(const std::string& dir);

}  // namespace uisim
