add_executable(ReleaseJsonParserTest
  ReleaseJsonParserTest.cpp
  ${REPO_ROOT}/lib/JsonParser/ReleaseJsonParser.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(ReleaseJsonParserTest PRIVATE
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(ReleaseJsonParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReleaseJsonParserTest)

add_executable(FirmwareManifestParserTest
  FirmwareManifestParserTest.cpp
  ${REPO_ROOT}/lib/JsonParser/FirmwareManifestParser.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(FirmwareManifestParserTest PRIVATE
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(FirmwareManifestParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FirmwareManifestParserTest)
