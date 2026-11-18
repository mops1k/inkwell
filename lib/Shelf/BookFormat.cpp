#include "BookFormat.h"

#include <FsHelpers.h>
#include <ZipFile.h>

#include <string_view>

namespace shelf {

namespace {

bool isFb2ZipPath(std::string_view path) { return FsHelpers::checkFileExtension(path, ".fb2.zip"); }

bool isGenericZipPath(std::string_view path) { return FsHelpers::checkFileExtension(path, ".zip"); }

}  // namespace

bool detectBookFormatFromZip(const std::string& path, BookFormat& out) {
  ZipFile zip(path);

  // Every EPUB carries META-INF/container.xml; a zero-byte one is not a book.
  size_t containerSize = 0;
  if (zip.getInflatedFileSize("META-INF/container.xml", &containerSize) && containerSize > 0) {
    out = BookFormat::Epub;
    return true;
  }

  bool containsFb2 = false;
  if (!zip.streamCentralDirectoryNames([&](std::string_view entryPath) {
        if (FsHelpers::checkFileExtension(entryPath, ".fb2")) containsFb2 = true;
      }))
    return false;

  if (!containsFb2) return false;
  out = BookFormat::Fb2Zip;
  return true;
}

BookFormat detectBookFormat(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) return BookFormat::Epub;
  if (FsHelpers::hasXtcExtension(path)) return BookFormat::Xtc;
  if (FsHelpers::hasTxtExtension(path)) return BookFormat::Txt;
  if (FsHelpers::hasMarkdownExtension(path)) return BookFormat::Markdown;
  if (FsHelpers::checkFileExtension(path, ".fb2")) return BookFormat::Fb2;
  if (isFb2ZipPath(path)) return BookFormat::Fb2Zip;

  // Any other ZIP is routed by its central directory, never by its name: a
  // book downloaded as "book.zip" is as valid as "book.fb2.zip".
  if (isGenericZipPath(path)) {
    BookFormat sniffed = BookFormat::Unknown;
    if (detectBookFormatFromZip(path, sniffed)) return sniffed;
  }
  return BookFormat::Unknown;
}

bool formatHasArtwork(BookFormat format) {
  switch (format) {
    case BookFormat::Epub:
    case BookFormat::Fb2:
    case BookFormat::Fb2Zip:
    case BookFormat::Xtc:
      // EPUB/FB2 carry an embedded cover; XTC is a page image format, so its first
      // page is legitimate artwork.
      return true;
    case BookFormat::Txt:
    case BookFormat::Markdown:
    case BookFormat::Unknown:
      // Plain text has no picture at all. Asking the cover pipeline for one used to
      // produce a garbage bitmap on the shelf; the grid draws its title plate now.
      return false;
  }
  return false;
}

}  // namespace shelf
