#include "ShelfTypes.h"

#include <cstring>

namespace shelf {

const char* bookFormatName(BookFormat format) {
  switch (format) {
    case BookFormat::Epub:
      return "epub";
    case BookFormat::Fb2:
      return "fb2";
    case BookFormat::Fb2Zip:
      return "fb2zip";
    case BookFormat::Txt:
      return "txt";
    case BookFormat::Markdown:
      return "md";
    case BookFormat::Xtc:
      return "xtc";
    case BookFormat::Unknown:
      break;
  }
  return "unknown";
}

BookFormat bookFormatFromName(std::string_view name) {
  if (name == "epub") return BookFormat::Epub;
  if (name == "fb2") return BookFormat::Fb2;
  if (name == "fb2zip") return BookFormat::Fb2Zip;
  if (name == "txt") return BookFormat::Txt;
  if (name == "md") return BookFormat::Markdown;
  if (name == "xtc") return BookFormat::Xtc;
  return BookFormat::Unknown;
}

}  // namespace shelf
