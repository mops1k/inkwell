cmake_minimum_required(VERSION 3.16)
project(ui_sim C CXX)

# Host UI simulator (plan p_46c273a4a2e1, phase E1): compiles the REAL
# GfxRenderer pixel path and real bitmap fonts for the host, replacing only the
# hardware boundary (HalDisplay/HalGPIO) so screens can be rendered to images
# without flashing the device.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

set(UISIM_SOURCES
  main.cpp
  Png.cpp
  Scenario.cpp
  ShelfScene.cpp
  host/HostEsp.cpp
  host/Stubs.cpp
  ${REPO_ROOT}/src/activities/shelf/ShelfGridRenderer.cpp
  ${REPO_ROOT}/lib/GfxRenderer/GfxRenderer.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/src/UiFonts.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
  ${REPO_ROOT}/test/epub_pipeline/LoggingImpl.cpp
)

add_executable(ui_sim ${UISIM_SOURCES})

# Include order matters:
#  - host/ first: our HalDisplay/HalGPIO shadow the device headers;
#  - lib/GfxRenderer before test/epub_pipeline so the REAL GfxRenderer.h wins
#    (the pipeline's stub stays available for its esp_*/I18n headers);
#  - test/zip_entry_reader + test/shims last provide the real-FS and Arduino shims.
target_include_directories(ui_sim PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/host
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/CooperativeAbort
  ${REPO_ROOT}/lib/Shelf
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/test/epub_pipeline
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
)

target_compile_definitions(ui_sim PRIVATE ENABLE_SERIAL_LOG=1 LOG_LEVEL=2)
target_compile_options(ui_sim PRIVATE
  -Wall
  -Wextra
  # The device toolchain's Arduino.h pulls in <cmath>/ESP heap symbols that the
  # host shims do not; the prelude injects both (a single -include, because
  # CMake de-duplicates repeated flag/value pairs).
  -include
  ${CMAKE_CURRENT_SOURCE_DIR}/host/host_prelude.h
)

# Visual smoke test: render the demo scene with a key scenario and fail on a
# blank frame or a failed PNG write. Runs as part of the main ctest suite.
enable_testing()
add_test(
  NAME ui_sim_demo
  COMMAND ui_sim
          --out ${CMAKE_CURRENT_BINARY_DIR}/ctest_frames
          --scenario ${CMAKE_CURRENT_SOURCE_DIR}/scenarios/demo.json
)
set_tests_properties(ui_sim_demo PROPERTIES TIMEOUT 60)
