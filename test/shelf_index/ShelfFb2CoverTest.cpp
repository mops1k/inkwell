#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Fb2Cover.h"
#include "TempDir.h"

namespace {

// A real (tiny) 1x1 JPEG, base64 as it appears inside an FB2 <binary>.
constexpr const char* kTinyJpegBase64 =
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0"
    "Hyc5PTgyPC4zNDL/wAALCAABAAEBAREA/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9"
    "AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZn"
    "aGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP0"
    "9fb3+Pn6/9oACAEBAAA/ANLPIP/Z";

std::string tinyFb2(bool withCover) {
  std::string xml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\" "
      "xmlns:l=\"http://www.w3.org/1999/xlink\">\n"
      "<description><title-info><book-title>Cover Test</book-title>";
  if (withCover) xml += "<coverpage><image l:href=\"#cover.jpg\"/></coverpage>";
  xml +=
      "<annotation><p>Annotation</p></annotation></title-info></description>\n"
      "<body><section><title><p>One</p></title><p>Text</p></section></body>\n";
  if (withCover) {
    xml += "<binary id=\"cover.jpg\" content-type=\"image/jpeg\">";
    xml += kTinyJpegBase64;
    xml += "</binary>\n";
  }
  xml += "</FictionBook>\n";
  return xml;
}

std::string readAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeAll(const std::string& path, const std::string& text) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream(path, std::ios::binary | std::ios::trunc) << text;
}

}  // namespace

TEST(ShelfFb2Cover, ExtractsOnlyTheCoverPicture) {
  shelftest::TempDir dir("fb2_cover");
  const std::string book = dir.join("book.fb2");
  writeAll(book, tinyFb2(true));
  const std::string out = dir.join("cache/cover.img");

  ASSERT_TRUE(shelf::extractFb2Cover(book, shelf::BookFormat::Fb2, out));
  const std::string bytes = readAll(out);
  ASSERT_GT(bytes.size(), 4u);
  // The payload must be the decoded JPEG, not base64 text.
  EXPECT_EQ(static_cast<unsigned char>(bytes[0]), 0xFF);
  EXPECT_EQ(static_cast<unsigned char>(bytes[1]), 0xD8);
  EXPECT_EQ(static_cast<unsigned char>(bytes[2]), 0xFF);
  EXPECT_EQ(bytes.find("cover.jpg"), std::string::npos);
}

TEST(ShelfFb2Cover, BookWithoutCoverIsNotAnError) {
  shelftest::TempDir dir("fb2_no_cover");
  const std::string book = dir.join("book.fb2");
  writeAll(book, tinyFb2(false));
  const std::string out = dir.join("cache/cover.img");

  EXPECT_FALSE(shelf::extractFb2Cover(book, shelf::BookFormat::Fb2, out));
  EXPECT_FALSE(std::filesystem::exists(out));
}
