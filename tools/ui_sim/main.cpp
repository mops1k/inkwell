// Host UI simulator entry point (plan p_46c273a4a2e1, phase E1).
//
// Renders the shelf demo scene through the REAL GfxRenderer and real bitmap
// fonts, then writes every frame as a 1-bit PNG in reading orientation. The
// device is never involved; only HalDisplay/HalGPIO are host implementations.
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <UiFonts.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "Png.h"
#include "Scenario.h"
#include "ShelfDemoScene.h"

namespace {

struct Options {
  std::string outDir = "build/ui_sim/out";
  std::string scenario;
};

Options parseArgs(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
      options.outDir = argv[++i];
    } else if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
      options.scenario = argv[++i];
    } else if (std::strcmp(argv[i], "--help") == 0) {
      std::printf("usage: ui_sim [--out DIR] [--scenario FILE]\n");
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

std::string framePath(const std::string& outDir, int index) {
  char name[32];
  std::snprintf(name, sizeof(name), "frame_%03d.png", index);
  return outDir + "/" + name;
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

  HalDisplay display;
  display.begin();
  GfxRenderer renderer(display);
  renderer.begin();  // binds the framebuffer, as on the device
  registerUiFonts(renderer);

  uisim::ShelfDemoScene scene;
  std::vector<std::string> keys;
  if (!options.scenario.empty()) {
    keys = uisim::readScenarioKeys(options.scenario);
    LOG_INF("UISIM", "Scenario %s: %zu key(s)", options.scenario.c_str(), keys.size());
  }

  const int frameCount = static_cast<int>(keys.size()) + 1;
  size_t firstFrameInk = 0;
  for (int frame = 0; frame < frameCount; ++frame) {
    if (frame > 0) scene.onKey(keys[static_cast<size_t>(frame - 1)]);
    scene.render(renderer);
    renderer.displayBuffer();

    const std::vector<uint8_t>& captured = display.lastFrame();
    const size_t ink = countBlackPixels(captured);
    if (frame == 0) firstFrameInk = ink;

    const std::string path = framePath(options.outDir, frame);
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
