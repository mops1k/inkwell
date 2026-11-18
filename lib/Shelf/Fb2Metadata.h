#pragma once

#include <string>

#include "ShelfTypes.h"

namespace shelf {

// Title/author/series for an FB2 book, read WITHOUT converting it to an
// EPUB-shaped package and WITHOUT writing anything to the book cache: the
// shelf must inspect ~100 books without leaving 100 converted packages behind.
//
// .fb2.zip is decompressed to a scratch file under the shelf directory first,
// because the XML scanner needs random seeks (see lib/Fb2/native/Fb2ZipOpener.h);
// the scratch file is always removed.
bool readFb2Metadata(const std::string& path, BookFormat format, BookMeta& out);

}  // namespace shelf
