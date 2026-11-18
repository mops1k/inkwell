#pragma once

#include <string>

#include "ShelfTypes.h"

namespace shelf {

// The single metadata entry point for the shelf: dispatches on the already
// detected format and reuses the existing per-format readers (Epub, Fb2, Xtc).
// Returns false when the container could not be read; the caller then keeps the
// book with a filename-derived title (see ShelfIndexer).
bool readBookMetadata(const std::string& path, BookFormat format, BookMeta& out);

// Adapter for ShelfIndexer::MetadataFn; the context is unused.
bool readBookMetadataThunk(void* ctx, const std::string& path, BookFormat format, BookMeta& out);

}  // namespace shelf
