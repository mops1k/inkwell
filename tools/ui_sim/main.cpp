// Host UI simulator entry point (plan p_46c273a4a2e1, phase E1).
//
// Renders host scenes through the REAL GfxRenderer, real bitmap fonts and the
// REAL theme chrome (BaseTheme/LyraTheme via UITheme), then writes every frame
// as a 1-bit PNG in reading orientation. The device is never involved; only
// HalDisplay/HalGPIO/HalClock/HalPowerManager are host implementations.
//
//   --scene shelf (default) : the shelf grid (shelfui::drawShelfPage)
//   --scene menu            : the HomeActivity-style main menu
//   --theme lyra|classic    : which theme's chrome the scenes draw
//   --prefix NAME           : frame files become NAME_%03d.png
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <UiFonts.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "CoverFixtures.h"
#include "MenuScene.h"
#include "Png.h"
#include "Scenario.h"
#include "Scene.h"
#include "ShelfScene.h"
#include "components/UITheme.h"

namespace {

struct Options {
  std::string outDir = "build/ui_sim/out";
  std::string scenario;
  std::string scene = "shelf";
  std::string theme = "lyra";
  std::string prefix = "frame";
};

Options parseArgs(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    const bool hasValue = i + 1 < argc;
    if (std::strcmp(arg, "--out") == 0 && hasValue) {
      options.outDir = argv[++i];
    } else if (std::strcmp(arg, "--scenario") == 0 && hasValue) {
      options.scenario = argv[++i];
    } else if (std::strcmp(arg, "--scene") == 0 && hasValue) {
      options.scene = argv[++i];
    } else if (std::strcmp(arg, "--theme") == 0 && hasValue) {
      options.theme = argv[++i];
    } else if (std::strcmp(arg, "--prefix") == 0 && hasValue) {
      options.prefix = argv[++i];
    } else if (std::strcmp(arg, "--help") == 0) {
      std::printf(
          "usage: ui_sim [--out DIR] [--scenario FILE] [--scene shelf|menu] [--theme lyra|classic] "
          "[--prefix NAME]\n");
      std::exit(0);
    }
  }
  return options;
}

// Bits cleared in the panel buffer are black ink (1 = white).
size_t countBlackPixels(const std::vector<uint8_t>& frame) {
  size_t black = 0;
  for (const uint8_t byte : frame) black += 8u - static_cast<size_t>(__builtin_popcount(byte));
  return black;
}

std::string framePath(const std::string& outDir, const std::string& prefix, int index) {
  char name[96];
  std::snprintf(name, sizeof(name), "%s_%03d.png", prefix.c_str(), index);
  return outDir + "/" + name;
}

bool applyTheme(const std::string& name) {
  using Theme = CrossPointSettings::UI_THEME;
  if (name == "lyra") {
    UITheme::getInstance().setTheme(Theme::LYRA);
    return true;
  }
  if (name == "classic" || name == "base") {
    UITheme::getInstance().setTheme(Theme::CLASSIC);
    return true;
  }
  return false;
}

// Builds the scene the CLI asked for. Covers are only written for the shelf, so
// a menu run does not litter the output directory with unused artwork.
std::unique_ptr<uisim::Scene> makeScene(const Options& options) {
  if (options.scene == "menu") {
    return std::make_unique<uisim::MenuScene>();
  }
  if (options.scene != "shelf") {
    std::fprintf(stderr, "unknown scene '%s' (expected shelf or menu)\n", options.scene.c_str());
    return nullptr;
  }
  auto shelf = std::make_unique<uisim::ShelfScene>();
  // Real 2-bit BMP artwork, written next to the frames: the grid then exercises
  // the same Bitmap + drawBitmap path the device uses for generated covers.
  const std::vector<std::string> covers = uisim::writeFixtureCovers(options.outDir + "/covers");
  if (covers.empty()) {
    std::fprintf(stderr, "cannot write cover fixtures into %s/covers\n", options.outDir.c_str());
    return nullptr;
  }
  shelf->useCovers(covers);
  return shelf;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parseArgs(argc, argv);
  std::error_code ec;
  std::filesystem::create_directories(options.outDir, ec);
  if (ec) {
    std::fprintf(stderr, "cannot create %s\n", options.outDir.c_str());
    return 1;
  }

  if (!applyTheme(options.theme)) {
    std::fprintf(stderr, "unknown theme '%s' (expected lyra or classic)\n", options.theme.c_str());
    return 1;
  }

  HalDisplay display;
  display.begin();
  GfxRenderer renderer(display);
  renderer.begin();  // binds the framebuffer, as on the device
  registerUiFonts(renderer);

  const std::unique_ptr<uisim::Scene> scene = makeScene(options);
  if (!scene) {
    return 1;
  }

  std::vector<std::string> keys;
  if (!options.scenario.empty()) {
    keys = uisim::readScenarioKeys(options.scenario);
    LOG_INF("UISIM", "Scenario %s: %zu key(s)", options.scenario.c_str(), keys.size());
  }

  const int frameCount = static_cast<int>(keys.size()) + 1;
  size_t firstFrameInk = 0;
  for (int frame = 0; frame < frameCount; ++frame) {
    if (frame > 0) scene->onKey(keys[static_cast<size_t>(frame - 1)]);
    scene->render(renderer);
    renderer.displayBuffer();

    const std::vector<uint8_t>& captured = display.lastFrame();
    const size_t ink = countBlackPixels(captured);
    if (frame == 0) firstFrameInk = ink;

    const std::string path = framePath(options.outDir, options.prefix, frame);
    if (!uisim::Png::write(path, captured, HalDisplay::DISPLAY_WIDTH, HalDisplay::DISPLAY_HEIGHT)) {
      std::fprintf(stderr, "cannot write %s\n", path.c_str());
      return 1;
    }
    LOG_INF("UISIM", "Frame %d -> %s (ink px: %zu)", frame, path.c_str(), ink);
  }

  std::printf("wrote %d frame(s) to %s, first frame ink pixels: %zu\n", frameCount, options.outDir.c_str(),
              firstFrameInk);
  if (firstFrameInk == 0) {
    std::fprintf(stderr, "frame is blank: the harness rendered nothing\n");
    return 2;
  }
  return 0;
}
