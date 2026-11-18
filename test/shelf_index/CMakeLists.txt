# Shelf index host tests (plan p_e2f3b165ca94, phase Ф1).
#
# The point of this target is that metadata extraction runs on REAL files: the
# EPUB path compiles lib/Epub/Epub.cpp with the same supporting-source set as
# test/epub_pipeline (its list is the reference if this one drifts), FB2 runs the
# native parser, XTC the real XtcParser, and storage the real-filesystem
# HalStorage shim from test/zip_entry_reader (which shadows test/shims).

set(SHELF_TEST_SOURCES
  ShelfFormatTest.cpp
  ShelfNameMetaTest.cpp
  ShelfJsonTest.cpp
  ShelfStoreTest.cpp
  ShelfIndexerTest.cpp
  ShelfCoverPolicyTest.cpp
  ShelfLibraryQueryTest.cpp
  ShelfStateTest.cpp
  ShelfFb2MetadataTest.cpp
  ShelfXtcMetadataTest.cpp
  ShelfEpubMetadataTest.cpp
  ${REPO_ROOT}/test/epub_pipeline/LoggingImpl.cpp

  # Code under test.
  ${REPO_ROOT}/lib/Shelf/ShelfTypes.cpp
  ${REPO_ROOT}/lib/Shelf/BookFormat.cpp
  ${REPO_ROOT}/lib/Shelf/BookNameMeta.cpp
  ${REPO_ROOT}/lib/Shelf/ShelfIndex.cpp
  ${REPO_ROOT}/lib/Shelf/ShelfIndexStore.cpp
  ${REPO_ROOT}/lib/Shelf/ShelfIndexer.cpp
  ${REPO_ROOT}/lib/Shelf/CoverPolicy.cpp
  ${REPO_ROOT}/lib/Shelf/LibraryQuery.cpp
  ${REPO_ROOT}/lib/Shelf/ShelfStateStore.cpp
  ${REPO_ROOT}/lib/Shelf/Fb2Metadata.cpp
  ${REPO_ROOT}/lib/Shelf/XtcMetadata.cpp
  ${REPO_ROOT}/lib/Shelf/BookMetadata.cpp
  # The upstream wrapper that now delegates archive sniffing to lib/Shelf.
  ${REPO_ROOT}/src/util/BookArchiveUtils.cpp

  # FB2 metadata path: the native scanner, exactly as test/native_fb2_zip builds it.
  ${REPO_ROOT}/lib/Fb2/Fb2Encoding.cpp
  ${REPO_ROOT}/lib/Fb2/native/Fb2Parser.cpp
  ${REPO_ROOT}/lib/Fb2/native/ZipReader.cpp
  ${REPO_ROOT}/lib/Fb2/native/Inflate.cpp
  # Fb2 itself only for Fb2::transcodeSourceToUtf8() — the one encoding table
  # shared with the full converter; the shelf never converts a book.
  ${REPO_ROOT}/lib/Fb2/Fb2.cpp
  ${REPO_ROOT}/lib/Fb2/Fb2StateMigration.cpp
  ${REPO_ROOT}/lib/Fb2/Fb2NavigationIndex.cpp
  ${REPO_ROOT}/lib/Fb2/Fb2AnchorIndex.cpp

  # XTC metadata path: the header reader only (no thumbnails, no Xtc wrapper).
  ${REPO_ROOT}/lib/Xtc/Xtc/XtcParser.cpp

  # EPUB metadata path: the real Epub.cpp and the sources it links against.
  ${REPO_ROOT}/lib/Epub/Epub.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Section.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Page.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookMetadataCache.cpp
  ${REPO_ROOT}/lib/Epub/Epub/FootnotePreviews.cpp
  ${REPO_ROOT}/lib/Epub/Epub/EpubImageManifest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContainerParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContentOpfParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/PageListSink.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/PageMapParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/PreviewBlockLocator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/TocNavParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/TocNcxParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/ImageBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/GifToFramebufferConverter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDecoderFactory.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/PngToFramebufferConverter.cpp

  # Shared support libraries (same set as test/epub_pipeline).
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/CooperativeAbort/CooperativeAbort.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/BmpToBmpConverter/BmpToBmpConverter.cpp
  ${REPO_ROOT}/lib/JpegToBmpConverter/JpegToBmpConverter.cpp
  ${REPO_ROOT}/lib/ProgressiveJpegDc/ProgressiveJpegDc.cpp
  ${REPO_ROOT}/lib/PngToBmpConverter/PngToBmpConverter.cpp
  ${REPO_ROOT}/lib/PngDecoder/PngStreamDecoder.cpp
  ${REPO_ROOT}/lib/TJpgDec/src/tjpgd.c
)

set(SHELF_TEST_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}
  # Stubs that shadow the device headers (GfxRenderer, HalDisplay, I18n, esp_*).
  ${REPO_ROOT}/test/epub_pipeline
  # Real stdio-backed FsFile/Storage — must precede test/shims and the
  # fb2_pipeline stub, which also ships a (device-shaped) HalStorage.h.
  ${REPO_ROOT}/test/zip_entry_reader
  # freertos/*.h shims: lib/Fb2/Fb2.cpp is compiled for Fb2::transcodeSourceToUtf8.
  ${REPO_ROOT}/test/fb2_pipeline
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Shelf
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/lib/Fb2
  ${REPO_ROOT}/lib/Fb2/native
  ${REPO_ROOT}/lib/Xtc
  ${REPO_ROOT}/lib/Xtc/Xtc
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/CooperativeAbort
  ${REPO_ROOT}/lib/BmpToBmpConverter
  ${REPO_ROOT}/lib/JpegToBmpConverter
  ${REPO_ROOT}/lib/PngToBmpConverter
  ${REPO_ROOT}/lib/PngDecoder
  ${REPO_ROOT}/lib/ProgressiveJpegDc
  ${REPO_ROOT}/lib/TJpgDec/src
)

add_executable(ShelfIndexTest ${SHELF_TEST_SOURCES})
target_include_directories(ShelfIndexTest PRIVATE ${SHELF_TEST_INCLUDES})
target_compile_definitions(ShelfIndexTest PRIVATE
  SHELF_FIXTURES="${REPO_ROOT}/test/fixtures/shelf"
  TEST_FIXTURES="${REPO_ROOT}/test/fixtures"
  ENABLE_SERIAL_LOG=1
  LOG_LEVEL=2
)
target_link_libraries(ShelfIndexTest PRIVATE
  crosspoint_test_common
  SaxParser
  GTest::gtest_main
)
gtest_discover_tests(ShelfIndexTest PROPERTIES TIMEOUT 120)
