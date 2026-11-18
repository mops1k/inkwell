#include "Fb2Cover.h"

#include <HalStorage.h>
#include <Logging.h>

#include "../Fb2/native/Fb2Parser.h"
#include "../Fb2/native/FsFileReader.h"
#include "Fb2Metadata.h"

namespace shelf {

namespace {

// The scanner reports the cover as the id used inside the document ("#cover.jpg"),
// while <binary> entries store the bare id.
std::string stripHash(const std::string& id) { return (!id.empty() && id[0] == '#') ? id.substr(1) : id; }

}  // namespace

bool extractFb2Cover(const std::string& path, BookFormat format, const std::string& outPath) {
  std::string xmlPath;
  bool temporary = false;
  if (!openFb2XmlForScan(path, format, xmlPath, temporary)) return false;

  HalFile file;
  bool wrote = false;
  if (Storage.openFileForRead("SHELF", xmlPath, file)) {
    FsFileReader reader(file);
    Fb2Parser parser;
    Fb2ScanResult result;
    if (parser.scan(reader, result) && !result.metadata.coverBinaryId.empty()) {
      const std::string wanted = stripHash(result.metadata.coverBinaryId);
      for (const auto& binary : result.binaries) {
        if (binary.id != wanted) continue;
        HalFile out;
        if (!Storage.openFileForWrite("SHELF", outPath, out)) break;
        // Streamed straight from the base64 payload: the image is never held in RAM
        // as text and the whole book is never materialised.
        wrote = parser.decodeBinary(reader, binary,
                                    [&out](const uint8_t* data, size_t len) { (void)out.write(data, len); });
        out.flush();
        out.close();
        if (!wrote) Storage.remove(outPath.c_str());
        break;
      }
    }
    file.close();
  }

  if (temporary) Storage.remove(xmlPath.c_str());
  if (!wrote) LOG_DBG("SHELF", "No cover image in %s", path.c_str());
  return wrote;
}

}  // namespace shelf
