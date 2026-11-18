#include "Scenario.h"

#include <StreamingJsonParser.h>

#include <cstdio>
#include <string>

namespace uisim {

namespace {

struct Reader {
  std::vector<std::string>* keys;
  int depth = 0;
  bool inKeys = false;
  std::string rootKey;
};

void onKey(void* ctx, const char* key, size_t len) {
  auto* reader = static_cast<Reader*>(ctx);
  if (reader->depth == 1) reader->rootKey.assign(key, len);
}

void onString(void* ctx, const char* value, size_t len) {
  auto* reader = static_cast<Reader*>(ctx);
  if (reader->depth == 2 && reader->inKeys) reader->keys->emplace_back(value, len);
}

void onNumber(void*, const char*, size_t) {}
void onBool(void*, bool) {}
void onNull(void*) {}
void onObjectStart(void* ctx) { ++static_cast<Reader*>(ctx)->depth; }
void onObjectEnd(void* ctx) { --static_cast<Reader*>(ctx)->depth; }

void onArrayStart(void* ctx) {
  auto* reader = static_cast<Reader*>(ctx);
  if (reader->depth == 1 && reader->rootKey == "keys") reader->inKeys = true;
  ++reader->depth;
}

void onArrayEnd(void* ctx) {
  auto* reader = static_cast<Reader*>(ctx);
  --reader->depth;
  if (reader->depth == 1) reader->inKeys = false;
}

}  // namespace

std::vector<std::string> readScenarioKeys(const std::string& path) {
  std::vector<std::string> keys;
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) return keys;
  std::string payload;
  char buffer[512];
  size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) payload.append(buffer, read);
  std::fclose(file);
  if (payload.empty()) return keys;

  Reader reader{&keys};
  const JsonCallbacks callbacks{&reader, &onKey,         &onString,    &onNumber,     &onBool,
                                &onNull, &onObjectStart, &onObjectEnd, &onArrayStart, &onArrayEnd};
  StreamingJsonParser parser(callbacks);
  parser.feed(payload.data(), payload.size());
  if (parser.hasError()) return {};
  return keys;
}

}  // namespace uisim
