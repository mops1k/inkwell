#include "CoverTrace.h"

#include <HalStorage.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace shelfui {

namespace {

constexpr const char* kTracePath = "/.crosspoint/shelf/cover.log";
// Keep the tail only: a cover attempt writes a few lines, and the point is to see the
// last failure, not to keep a history of every run.
constexpr size_t kMaxBytes = 4096;

std::string readExisting() {
  HalFile file;
  if (!Storage.openFileForRead("SHELF", kTracePath, file)) return {};
  std::string text;
  char buffer[256];
  int read = 0;
  while ((read = file.read(buffer, sizeof(buffer))) > 0) text.append(buffer, static_cast<size_t>(read));
  file.close();
  return text;
}

}  // namespace

void coverTrace(const char* format, ...) {
  char line[200];
  va_list args;
  va_start(args, format);
  const int length = vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (length <= 0) return;

  std::string text = readExisting();
  text.append(line, static_cast<size_t>(length));
  if (!text.empty() && text.back() != '\n') text.push_back('\n');
  if (text.size() > kMaxBytes) text.erase(0, text.size() - kMaxBytes);

  HalFile file;
  if (!Storage.openFileForWrite("SHELF", kTracePath, file)) return;
  file.write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  file.close();
}

void coverTraceReset() {
  HalFile file;
  if (!Storage.openFileForWrite("SHELF", kTracePath, file)) return;
  file.close();
}

}  // namespace shelfui
