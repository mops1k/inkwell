#include "XtcMetadata.h"

#include <Logging.h>

#include "../Xtc/Xtc/XtcParser.h"

namespace shelf {

bool readXtcMetadata(const std::string& path, BookMeta& out) {
  xtc::XtcParser parser;
  const xtc::XtcError opened = parser.open(path.c_str());
  if (opened != xtc::XtcError::OK) {
    LOG_DBG("SHELF", "XTC header unreadable: %s", path.c_str());
    return false;
  }
  out.title = parser.getTitle();
  out.author = parser.getAuthor();
  parser.close();
  return !out.title.empty();
}

}  // namespace shelf
