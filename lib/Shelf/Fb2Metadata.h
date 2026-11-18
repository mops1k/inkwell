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

// Makes the FB2 XML of `path` readable with random access: a plain .fb2 is used
// directly, the single .fb2.zip entry is staged next to the book. `temporary` says
// whether the caller must delete `xmlPath` when done. Shared by the metadata reader
// and the cover extractor so both stage archives the same way.
bool openFb2XmlForScan(const std::string& path, BookFormat format, std::string& xmlPath, bool& temporary);

}  // namespace shelf
