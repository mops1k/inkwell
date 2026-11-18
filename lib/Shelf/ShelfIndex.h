#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ShelfTypes.h"

namespace shelf {

// The in-RAM library index. ~100 books fit in a few tens of KiB (plan Ф1), so
// the whole model lives in a vector and is serialized as one JSON document.
struct ShelfIndex {
  std::vector<ShelfBook> books;

  const ShelfBook* find(std::string_view path) const;
};

// JSON codec, version 1. Both directions are tolerant of a truncated or
// foreign document: parse returns false instead of crashing, and the store
// then falls back to the .bak copy or a full rebuild.
//
// Strings are capped to StreamingJsonParser::TOKEN_BUF_SIZE so a pathological
// title can never make the whole index unreadable; the cap backs off to a
// UTF-8 lead byte, so text stays valid.
void serializeIndex(const ShelfIndex& index, std::string& out);
bool parseIndex(std::string_view json, ShelfIndex& out);

}  // namespace shelf
