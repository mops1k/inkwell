#include "BookNameMeta.h"

#include <cstring>
#include <string_view>

namespace shelf {

namespace {

// UTF-8 separators used by OPDS downloads and Calibre exports.
constexpr std::string_view kHyphenSep = " - ";
constexpr std::string_view kEnDashSep = " \xE2\x80\x93 ";  // " – "
constexpr std::string_view kEmDashSep = " \xE2\x80\x94 ";  // " — "

constexpr std::string_view kBookExtensions[] = {".epub", ".fb2", ".txt", ".md", ".markdown", ".xtc", ".xtch", ".zip"};

bool endsWithIgnoreCase(std::string_view text, std::string_view suffix) {
  if (text.size() < suffix.size()) return false;
  const size_t offset = text.size() - suffix.size();
  for (size_t i = 0; i < suffix.size(); ++i) {
    char a = text[offset + i];
    char b = suffix[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  return text;
}

// One pass per known extension, at most twice, so "a.fb2.zip" loses both.
void stripBookExtensions(std::string& name) {
  for (int pass = 0; pass < 2; ++pass) {
    bool stripped = false;
    for (const auto ext : kBookExtensions) {
      if (endsWithIgnoreCase(name, ext) && name.size() > ext.size()) {
        name.resize(name.size() - ext.size());
        stripped = true;
        break;
      }
    }
    if (!stripped) break;
  }
}

}  // namespace

std::string fileNameStem(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  stripBookExtensions(name);
  return name.empty() ? path : name;
}

BookMeta parseBookName(const std::string& path) {
  BookMeta meta;
  const std::string stem = fileNameStem(path);
  const std::string_view text = trim(stem);

  // Earliest separator wins, so a title that itself contains a dash keeps it.
  const std::string_view* bestSep = nullptr;
  size_t bestPos = std::string_view::npos;
  for (const auto* sep : {&kHyphenSep, &kEnDashSep, &kEmDashSep}) {
    const size_t pos = text.find(*sep);
    if (pos != std::string_view::npos && pos < bestPos) {
      bestPos = pos;
      bestSep = sep;
    }
  }

  if (bestSep != nullptr) {
    const std::string_view author = trim(text.substr(0, bestPos));
    const std::string_view title = trim(text.substr(bestPos + bestSep->size()));
    if (!author.empty() && !title.empty()) {
      meta.author = std::string(author);
      meta.title = std::string(title);
      return meta;
    }
  }

  meta.title = std::string(text);
  return meta;
}

}  // namespace shelf
