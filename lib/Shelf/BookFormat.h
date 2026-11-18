#pragma once

#include <string>

#include "ShelfTypes.h"

namespace shelf {

// The single entry point for "which of the six formats is this file?".
// Extensions decide the cheap cases; an archive is sniffed by content, because
// a plain .zip may hold an EPUB or an FB2 book (see AGENTS.md and
// src/util/BookArchiveUtils.cpp, which forwards here).
BookFormat detectBookFormat(const std::string& path);

// Content-only sniffing for an archive that is already known to be a ZIP.
// False when the archive is neither EPUB-shaped nor FB2-bearing.
bool detectBookFormatFromZip(const std::string& path, BookFormat& out);

// True when the format can carry artwork of its own (embedded cover, page image).
// A plain .txt/.md has none: the shelf must draw its own title plate instead of
// asking the cover pipeline, which used to synthesise a bogus bitmap for them.
bool formatHasArtwork(BookFormat format);

}  // namespace shelf
