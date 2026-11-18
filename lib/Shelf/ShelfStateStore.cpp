#include "ShelfStateStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <StreamingJsonParser.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace shelf {

namespace {

constexpr int kStateVersion = 1;

void appendEscaped(std::string& out, std::string_view raw) {
  out.push_back('"');
  for (const char ch : raw) {
    const auto c = static_cast<unsigned char>(ch);
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (c < 0x20) {
      // Control characters cannot appear in a one-line document.
      out.push_back(' ');
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('"');
}

class StateReader {
 public:
  explicit StateReader(LibraryState& out)
      : target(out),
        parser(JsonCallbacks{this, &StateReader::keyTrampoline, &StateReader::stringTrampoline,
                             &StateReader::numberTrampoline, &StateReader::boolTrampoline, &StateReader::nullTrampoline,
                             &StateReader::objectStartTrampoline, &StateReader::objectEndTrampoline,
                             &StateReader::arrayStartTrampoline, &StateReader::arrayEndTrampoline}) {}

  void feed(const char* data, size_t len) { parser.feed(data, len); }

  bool finish() {
    if (parser.hasError() || !sawVersion || version != kStateVersion || !rootClosed) return false;
    LibraryState state;
    state.filter.field = filterFieldFromName(fieldName);
    state.filter.value = value;
    if (!state.filter.active()) state.filter.clear();  // a field without a value is "all books"
    state.sort = sortKeyFromName(sortName);
    target = std::move(state);
    return true;
  }

 private:
  static void keyTrampoline(void* ctx, const char* key, size_t len) {
    static_cast<StateReader*>(ctx)->key.assign(key, len);
  }
  static void stringTrampoline(void* ctx, const char* value, size_t len) {
    static_cast<StateReader*>(ctx)->handleString(value, len);
  }
  static void numberTrampoline(void* ctx, const char* value, size_t len) {
    static_cast<StateReader*>(ctx)->handleNumber(value, len);
  }
  static void boolTrampoline(void*, bool) {}
  static void nullTrampoline(void*) {}
  static void objectStartTrampoline(void* ctx) { static_cast<StateReader*>(ctx)->depth++; }
  static void objectEndTrampoline(void* ctx) {
    auto* self = static_cast<StateReader*>(ctx);
    if (self->depth == 1) self->rootClosed = true;
    if (self->depth > 0) self->depth--;
  }
  static void arrayStartTrampoline(void* ctx) { static_cast<StateReader*>(ctx)->depth++; }
  static void arrayEndTrampoline(void* ctx) { static_cast<StateReader*>(ctx)->depth--; }

  void handleString(const char* text, size_t len) {
    if (depth != 1) return;
    if (key == "field")
      fieldName.assign(text, len);
    else if (key == "value")
      value.assign(text, len);
    else if (key == "sort")
      sortName.assign(text, len);
  }

  void handleNumber(const char* text, size_t len) {
    if (depth != 1 || key != "version" || len >= 8) return;
    char buf[8];
    std::memcpy(buf, text, len);
    buf[len] = '\0';
    version = std::atoi(buf);
    sawVersion = true;
  }

  LibraryState& target;
  StreamingJsonParser parser;
  std::string key;
  std::string fieldName;
  std::string value;
  std::string sortName;
  int depth = 0;
  int version = -1;
  bool sawVersion = false;
  bool rootClosed = false;
};

std::string dirOf(const std::string& file) {
  const size_t slash = file.find_last_of('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return file.substr(0, slash);
}

}  // namespace

void serializeLibraryState(const LibraryState& state, std::string& out) {
  out.clear();
  out.reserve(96);
  out += "{\"version\":";
  out += std::to_string(kStateVersion);
  out += ",\"field\":";
  appendEscaped(out, state.filter.active() ? filterFieldName(state.filter.field) : filterFieldName(FilterField::None));
  out += ",\"value\":";
  appendEscaped(out, state.filter.active() ? state.filter.value : std::string_view{});
  out += ",\"sort\":";
  appendEscaped(out, sortKeyName(state.sort));
  out += "}\n";
}

bool parseLibraryState(std::string_view json, LibraryState& out) {
  StateReader reader(out);
  reader.feed(json.data(), json.size());
  return reader.finish();
}

ShelfStateStore::ShelfStateStore(std::string dir) {
  file = std::move(dir) + "/state.json";
  tmp = file + ".tmp";
  bak = file + ".bak";
}

bool ShelfStateStore::load(LibraryState& out) {
  for (const std::string& path : {file, bak}) {
    HalFile handle;
    if (!Storage.openFileForRead("SHELF", path, handle)) continue;
    std::string payload;
    char buf[256];
    int read = 0;
    while ((read = handle.read(buf, sizeof(buf))) > 0) payload.append(buf, static_cast<size_t>(read));
    handle.close();
    if (!payload.empty() && parseLibraryState(payload, out)) return true;
  }
  LOG_DBG("SHELF", "No usable library state next to %s", file.c_str());
  return false;
}

bool ShelfStateStore::save(const LibraryState& state) {
  const std::string dir = dirOf(file);
  if (!Storage.ensureDirectoryExists(dir.c_str())) {
    LOG_ERR("SHELF", "Cannot create %s", dir.c_str());
    return false;
  }

  std::string payload;
  serializeLibraryState(state, payload);

  Storage.remove(tmp.c_str());
  HalFile handle;
  if (!Storage.openFileForWrite("SHELF", tmp, handle)) return false;
  const bool wrote = handle.write(payload.data(), payload.size()) == payload.size();
  handle.flush();
  handle.close();
  if (!wrote) {
    Storage.remove(tmp.c_str());
    return false;
  }

  if (Storage.exists(file.c_str())) {
    if (Storage.exists(bak.c_str())) Storage.remove(bak.c_str());
    if (!Storage.rename(file.c_str(), bak.c_str())) {
      Storage.remove(tmp.c_str());
      return false;
    }
  }
  if (!Storage.rename(tmp.c_str(), file.c_str())) {
    LOG_ERR("SHELF", "Cannot commit %s", file.c_str());
    return false;
  }
  return true;
}

}  // namespace shelf
