#pragma once

#include <string>

#include "ShelfTypes.h"

namespace shelf {

// Title/author of an XTC/XTCH book. XTC stores them in its own header, so the
// shelf goes through the existing XtcParser — there must be exactly one reader
// of the XTC header (lib/Xtc). Reading the header is cheap: open() reads the
// fixed-size header, the metadata strings and the first page entry only.
bool readXtcMetadata(const std::string& path, BookMeta& out);

}  // namespace shelf
