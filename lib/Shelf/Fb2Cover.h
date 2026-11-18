#pragma once

#include <string>

#include "ShelfTypes.h"

namespace shelf {

// Extracts ONLY the cover picture of an FB2 book into `outPath` (JPEG or PNG bytes,
// exactly as stored in the file), without converting the book to an EPUB package.
//
// Why this exists: a full Fb2::load() builds package.epub, book.bin, the spine, the
// TOC and every inline image — minutes of work for a 1000-book library and a lot of
// SD writes, all to show one thumbnail. The native scanner already indexes the
// <binary> payloads, so the cover can be streamed out on its own.
//
// Returns false when the book has no referenced cover, the id does not resolve, or
// the file cannot be written. A book without artwork is not an error: the caller
// marks it as "no cover" and stops asking.
bool extractFb2Cover(const std::string& path, BookFormat format, const std::string& outPath);

}  // namespace shelf
