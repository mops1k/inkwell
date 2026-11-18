#include "Fb2Metadata.h"

#include <HalStorage.h>
#include <Logging.h>

#include "../Fb2/Fb2.h"
#include "../Fb2/native/Fb2Parser.h"
#include "../Fb2/native/Fb2ZipOpener.h"
#include "../Fb2/native/FsFileReader.h"

namespace shelf {

namespace {

std::string parentDirectory(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

// Scratch files live next to the book, not in a fixed "/.crosspoint": the
// shelf must not depend on the cache root being writable (host tests run on an
// ordinary filesystem), and the name starts with '.' so the indexer's own walk
// skips a leftover after a crash.
std::string scratchBase(const std::string& path) {
  return parentDirectory(path) + "/.shelf_fb2_" + std::to_string(std::hash<std::string>{}(path));
}

bool scanFb2File(const std::string& path, BookMeta& out) {
  HalFile file;
  if (!Storage.openFileForRead("SHELF", path, file)) return false;

  FsFileReader reader(file);
  Fb2Parser parser;
  Fb2ScanResult result;
  const bool scanned = parser.scan(reader, result);
  file.close();
  if (!scanned) return false;

  out.title = result.metadata.title;
  out.author = result.metadata.author;
  out.language = result.metadata.language;
  out.genre = result.metadata.genre;
  out.series = result.metadata.sequenceName;
  if (result.metadata.sequenceNumber != 0) out.seriesIndex = std::to_string(result.metadata.sequenceNumber);
  return !out.title.empty();
}

// Scans `path`, first transcoding a declared Windows-1251/KOI8-R source to a
// UTF-8 scratch copy through Fb2::transcodeSourceToUtf8() — the native parser
// passes bytes through untouched, exactly like the full converter does before
// its own scan.
bool scanWithEncoding(const std::string& path, BookMeta& out) {
  const std::string base = scratchBase(path);
  std::string source = path;
  if (!Fb2::transcodeSourceToUtf8(source, base)) {
    LOG_ERR("SHELF", "Transcode failed for %s", path.c_str());
    return false;
  }
  const bool ok = scanFb2File(source, out);
  if (source != path) Storage.remove(source.c_str());
  return ok;
}

bool scanFb2Zip(const std::string& path, BookMeta& out) {
  // The XML scanner needs random seeks (see lib/Fb2/native/Fb2ZipOpener.h), so
  // the single entry is staged to a scratch file first. Unlike Fb2::load() this
  // writes nothing into the book cache.
  const std::string scratch = scratchBase(path) + ".zip.fb2";
  Storage.remove(scratch.c_str());

  HalFile zipFile;
  if (!Storage.openFileForRead("SHELF", path, zipFile)) return false;
  FsFileReader zipReader(zipFile);

  HalFile staged;
  bool extracted = false;
  if (Storage.openFileForWrite("SHELF", scratch, staged)) {
    extracted = extractFb2FromZip(zipReader, [&staged](const uint8_t* data, size_t len) {
      // Stream errors surface as a failed scan below; a partial .fb2 is not XML.
      (void)staged.write(data, len);
    });
    staged.flush();
    staged.close();
  }
  zipFile.close();

  if (!extracted) {
    LOG_ERR("SHELF", "No FB2 entry extracted from %s", path.c_str());
    Storage.remove(scratch.c_str());
    return false;
  }

  const bool ok = scanWithEncoding(scratch, out);
  Storage.remove(scratch.c_str());
  return ok;
}

}  // namespace

bool readFb2Metadata(const std::string& path, BookFormat format, BookMeta& out) {
  switch (format) {
    case BookFormat::Fb2:
      return scanWithEncoding(path, out);
    case BookFormat::Fb2Zip:
      return scanFb2Zip(path, out);
    default:
      return false;
  }
}

}  // namespace shelf
