#include "BookArchiveUtils.h"

#include <BookFormat.h>
#include <FsHelpers.h>

namespace {
bool hasExtension(std::string_view path, const char* extension) {
  return FsHelpers::checkFileExtension(path, extension);
}
}  // namespace

bool isFb2BookPath(const std::string_view path) { return hasExtension(path, ".fb2") || hasExtension(path, ".fb2.zip"); }

bool isBookZipPath(const std::string_view path) { return hasExtension(path, ".zip"); }

bool isFb2OrZipBookPath(const std::string_view path) { return isFb2BookPath(path) || isBookZipPath(path); }

BookArchiveType detectBookArchiveType(const std::string& path) {
  // Content sniffing has exactly one implementation, shelf::detectBookFormatFromZip
  // in lib/Shelf. This wrapper keeps the upstream call sites untouched and
  // preserves their contract exactly: only ZIP paths are sniffed, the result is
  // based on the archive's central directory (never on its name), and a plain
  // .fb2 still reports None here.
  if (!isBookZipPath(path)) return BookArchiveType::None;
  shelf::BookFormat sniffed = shelf::BookFormat::Unknown;
  if (!shelf::detectBookFormatFromZip(path, sniffed)) return BookArchiveType::None;
  switch (sniffed) {
    case shelf::BookFormat::Epub:
      return BookArchiveType::Epub;
    case shelf::BookFormat::Fb2Zip:
      return BookArchiveType::Fb2;
    default:
      return BookArchiveType::None;
  }
}
