#include <gtest/gtest.h>

#include "BookNameMeta.h"

TEST(ShelfNameMeta, StripsKnownExtensions) {
  EXPECT_EQ("book", shelf::fileNameStem("/books/book.epub"));
  EXPECT_EQ("book", shelf::fileNameStem("/books/book.fb2"));
  EXPECT_EQ("book", shelf::fileNameStem("/books/book.fb2.zip"));
  // Case is preserved in the displayed name; only the extension match ignores it.
  EXPECT_EQ("BOOK", shelf::fileNameStem("/books/BOOK.TXT"));
  EXPECT_EQ("book", shelf::fileNameStem("/books/book.md"));
  EXPECT_EQ("book", shelf::fileNameStem("/books/book.xtch"));
  // Unknown extensions are part of the name, not stripped.
  EXPECT_EQ("book.rar", shelf::fileNameStem("/books/book.rar"));
}

TEST(ShelfNameMeta, SplitsAuthorAndTitle) {
  const shelf::BookMeta hyphen = shelf::parseBookName("/books/Лев Толстой - Война и мир.txt");
  EXPECT_EQ("Лев Толстой", hyphen.author);
  EXPECT_EQ("Война и мир", hyphen.title);

  // En dash and em dash are both produced by real OPDS servers and Calibre.
  const shelf::BookMeta enDash = shelf::parseBookName("/books/Author \xE2\x80\x93 Title.epub");
  EXPECT_EQ("Author", enDash.author);
  EXPECT_EQ("Title", enDash.title);

  const shelf::BookMeta emDash = shelf::parseBookName("/books/Author \xE2\x80\x94 Title - with dash.md");
  EXPECT_EQ("Author", emDash.author);
  EXPECT_EQ("Title - with dash", emDash.title);
}

TEST(ShelfNameMeta, FallsBackToWholeStem) {
  const shelf::BookMeta noAuthor = shelf::parseBookName("/books/just-title.md");
  EXPECT_EQ("just-title", noAuthor.title);
  EXPECT_TRUE(noAuthor.author.empty());

  // A leading separator must not produce an empty author.
  const shelf::BookMeta leading = shelf::parseBookName("/books/ - Title.txt");
  EXPECT_EQ("- Title", leading.title);
  EXPECT_TRUE(leading.author.empty());
}
