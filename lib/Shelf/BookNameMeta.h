#pragma once

#include <string>

#include "ShelfTypes.h"

namespace shelf {

// "Author - Title.epub" -> "Author - Title". Strips the book extension(s),
// including the double ".fb2.zip" form. Never returns an empty string for a
// non-empty path.
std::string fileNameStem(const std::string& path);

// Metadata for containers that store none of their own (TXT, Markdown), and
// the fallback title for a broken EPUB/FB2/XTC. Filenames produced by OPDS
// downloads follow "Author - Title" (see src/util/OpdsFilename.h), so those are
// split; anything else becomes the whole title. Both en dash and em dash
// separators are accepted because OPDS servers and Calibre export both.
BookMeta parseBookName(const std::string& path);

}  // namespace shelf
