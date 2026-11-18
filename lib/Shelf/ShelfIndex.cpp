#include "ShelfIndex.h"

#include <StreamingJsonParser.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace shelf {

namespace {

// v2 added `genre` (first FB2 <genre> / EPUB dc:subject) for the shelf's genre
// filter. A v1 file still loads: its books simply have no genre until the next
// scan fills one in, so a firmware downgrade/upgrade never forces a full rescan.
constexpr int kIndexVersion = 2;
constexpr int kMinSupportedVersion = 1;
// Leave room for the surrounding quotes and any escape expansion inside the
// parser's token buffer; a value that still overflows is truncated on write,
// never on read.
constexpr size_t kMaxJsonString = StreamingJsonParser::TOKEN_BUF_SIZE - 32;

std::string_view capUtf8(std::string_view text) {
  if (text.size() <= kMaxJsonString) return text;
  size_t len = kMaxJsonString;
  // Never cut a multi-byte sequence in half: back off to its lead byte.
  while (len > 0 && (static_cast<unsigned char>(text[len]) & 0xC0) == 0x80) --len;
  return text.substr(0, len);
}

void appendString(std::string& out, std::string_view raw) {
  out.push_back('"');
  for (const char ch : capUtf8(raw)) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  out.push_back('"');
}

void appendKey(std::string& out, const char* key) {
  out.push_back('"');
  out += key;
  out += "\":";
}

// Streaming JSON reader for the v1 document. Same shape as
// lib/JsonParser/ReleaseJsonParser: the parser owns no app state, this adapter
// maps keys to fields and rejects anything that is not a complete v1 index.
class IndexReader {
 public:
  explicit IndexReader(ShelfIndex& out)
      : target(out),
        parser(JsonCallbacks{this, &IndexReader::keyTrampoline, &IndexReader::stringTrampoline,
                             &IndexReader::numberTrampoline, &IndexReader::boolTrampoline, &IndexReader::nullTrampoline,
                             &IndexReader::objectStartTrampoline, &IndexReader::objectEndTrampoline,
                             &IndexReader::arrayStartTrampoline, &IndexReader::arrayEndTrampoline}) {}

  void feed(const char* data, size_t len) { parser.feed(data, len); }

  bool finish() {
    if (error || parser.hasError()) return false;
    if (!sawVersion || version < kMinSupportedVersion || version > kIndexVersion) return false;
    if (!sawBooks || !rootClosed) return false;
    if (haveBook) return false;  // truncated mid-object
    target = std::move(parsed);
    return true;
  }

 private:
  static void keyTrampoline(void* ctx, const char* key, size_t len) {
    static_cast<IndexReader*>(ctx)->handleKey(key, len);
  }
  static void stringTrampoline(void* ctx, const char* value, size_t len) {
    static_cast<IndexReader*>(ctx)->handleString(value, len);
  }
  static void numberTrampoline(void* ctx, const char* value, size_t len) {
    static_cast<IndexReader*>(ctx)->handleNumber(value, len);
  }
  static void boolTrampoline(void* ctx, bool value) { static_cast<IndexReader*>(ctx)->handleBool(value); }
  static void nullTrampoline(void* ctx) { (void)ctx; }
  static void objectStartTrampoline(void* ctx) { static_cast<IndexReader*>(ctx)->handleObjectStart(); }
  static void objectEndTrampoline(void* ctx) { static_cast<IndexReader*>(ctx)->handleObjectEnd(); }
  static void arrayStartTrampoline(void* ctx) { static_cast<IndexReader*>(ctx)->handleArrayStart(); }
  static void arrayEndTrampoline(void* ctx) { static_cast<IndexReader*>(ctx)->handleArrayEnd(); }

  void handleKey(const char* key, size_t len) {
    if (depth == 1) {
      rootKey.assign(key, len);
    } else if (depth == 3) {
      bookKey.assign(key, len);
    }
  }

  void handleString(const char* value, size_t len) {
    if (depth != 3) return;
    const std::string_view text(value, len);
    if (bookKey == "path") {
      book.path.assign(text);
    } else if (bookKey == "title") {
      book.meta.title.assign(text);
    } else if (bookKey == "author") {
      book.meta.author.assign(text);
    } else if (bookKey == "series") {
      book.meta.series.assign(text);
    } else if (bookKey == "seriesIndex") {
      book.meta.seriesIndex.assign(text);
    } else if (bookKey == "language") {
      book.meta.language.assign(text);
    } else if (bookKey == "genre") {
      book.meta.genre.assign(text);
    } else if (bookKey == "format") {
      book.format = bookFormatFromName(text);
    }
  }

  void handleNumber(const char* value, size_t len) {
    char buf[24];
    if (len >= sizeof(buf)) return;
    std::memcpy(buf, value, len);
    buf[len] = '\0';
    const unsigned long parsed = std::strtoul(buf, nullptr, 10);
    if (depth == 1) {
      if (rootKey == "version") {
        version = static_cast<int>(parsed);
        sawVersion = true;
      }
    } else if (depth == 3) {
      if (bookKey == "size") {
        book.size = static_cast<uint32_t>(parsed);
      } else if (bookKey == "date") {
        book.fatDateTime = static_cast<uint32_t>(parsed);
      }
    }
  }

  void handleBool(bool value) {
    if (depth == 3 && bookKey == "metaOk") book.metaOk = value;
  }

  void handleObjectStart() {
    ++depth;
    if (depth == 3 && inBooks) {
      book = ShelfBook{};
      haveBook = true;
    }
  }

  void handleObjectEnd() {
    if (depth == 3 && haveBook) {
      if (!book.path.empty()) parsed.books.push_back(std::move(book));
      book = ShelfBook{};
      haveBook = false;
    } else if (depth == 1) {
      rootClosed = true;
    }
    if (depth > 0) --depth;
  }

  void handleArrayStart() {
    if (depth == 1 && rootKey == "books") {
      inBooks = true;
      sawBooks = true;
    }
    ++depth;
  }

  void handleArrayEnd() {
    if (depth > 0) --depth;
    if (depth == 1 && inBooks) inBooks = false;
  }

  ShelfIndex& target;
  StreamingJsonParser parser;
  ShelfIndex parsed;
  ShelfBook book;
  std::string rootKey;
  std::string bookKey;
  int depth = 0;
  int version = 0;
  bool error = false;
  bool sawVersion = false;
  bool sawBooks = false;
  bool inBooks = false;
  bool haveBook = false;
  bool rootClosed = false;
};

}  // namespace

const ShelfBook* ShelfIndex::find(std::string_view path) const {
  for (const auto& book : books) {
    if (book.path == path) return &book;
  }
  return nullptr;
}

void serializeIndex(const ShelfIndex& index, std::string& out) {
  out.clear();
  out.reserve(64 + index.books.size() * 220);
  out += "{\"version\":";
  out += std::to_string(kIndexVersion);
  out += ",\"books\":[";
  bool first = true;
  for (const auto& book : index.books) {
    if (!first) out.push_back(',');
    first = false;
    out.push_back('{');
    appendKey(out, "path");
    appendString(out, book.path);
    out.push_back(',');
    appendKey(out, "size");
    out += std::to_string(book.size);
    out.push_back(',');
    appendKey(out, "date");
    out += std::to_string(book.fatDateTime);
    out.push_back(',');
    appendKey(out, "format");
    appendString(out, bookFormatName(book.format));
    out.push_back(',');
    appendKey(out, "title");
    appendString(out, book.meta.title);
    out.push_back(',');
    appendKey(out, "author");
    appendString(out, book.meta.author);
    out.push_back(',');
    appendKey(out, "series");
    appendString(out, book.meta.series);
    out.push_back(',');
    appendKey(out, "seriesIndex");
    appendString(out, book.meta.seriesIndex);
    out.push_back(',');
    appendKey(out, "language");
    appendString(out, book.meta.language);
    out.push_back(',');
    appendKey(out, "genre");
    appendString(out, book.meta.genre);
    out.push_back(',');
    appendKey(out, "metaOk");
    out += book.metaOk ? "true" : "false";
    out.push_back('}');
  }
  out += "]}\n";
}

bool parseIndex(std::string_view json, ShelfIndex& out) {
  IndexReader reader(out);
  reader.feed(json.data(), json.size());
  return reader.finish();
}

}  // namespace shelf
