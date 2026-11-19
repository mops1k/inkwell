#include "BookMetadata.h"

#include <Epub.h>

#include "BookNameMeta.h"
#include "Fb2Metadata.h"
#include "XtcMetadata.h"

namespace shelf {

namespace {
// Same cache root the reader and BookInfoActivity use; metadata-only loads do
// not write into it, they only reuse caches that already exist.
constexpr char kCacheRoot[] = "/.crosspoint";
}  // namespace

bool readBookMetadata(const std::string& path, BookFormat format, BookMeta& out) {
  out = BookMeta{};
  switch (format) {
    case BookFormat::Epub: {
      Epub epub(path, kCacheRoot);
      if (!epub.loadForMetadata()) return false;
      out.title = epub.getTitle();
      out.author = epub.getAuthor();
      out.series = epub.getSeries();
      out.seriesIndex = epub.getSeriesIndex();
      out.language = epub.getLanguage();
      out.genre = epub.getGenre();
      out.description = epub.getDescription();
      out.year = epub.getYear();
      out.publisher = epub.getPublisher();
      out.pages = epub.getPages();
      return !out.title.empty();
    }
    case BookFormat::Fb2:
    case BookFormat::Fb2Zip:
      return readFb2Metadata(path, format, out);
    case BookFormat::Xtc:
      return readXtcMetadata(path, out);
    case BookFormat::Txt:
    case BookFormat::Markdown:
      out = parseBookName(path);
      return !out.title.empty();
    case BookFormat::Unknown:
      break;
  }
  return false;
}

bool readBookMetadataThunk(void*, const std::string& path, BookFormat format, BookMeta& out) {
  return readBookMetadata(path, format, out);
}

}  // namespace shelf
