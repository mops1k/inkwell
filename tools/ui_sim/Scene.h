#pragma once

#include <GfxRenderer.h>

#include <string>

// A renderable host scenario: main() feeds it scenario keys and asks for one
// frame per step. Every scene draws the REAL theme chrome
// (BaseTheme/LyraTheme through UITheme), so a new scene cannot accidentally
// invent its own header or hint bar.
namespace uisim {

class Scene {
 public:
  virtual ~Scene() = default;

  virtual void onKey(const std::string& key) = 0;
  virtual void render(GfxRenderer& renderer) const = 0;
};

}  // namespace uisim
