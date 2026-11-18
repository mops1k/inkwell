#pragma once

#include <string>
#include <vector>

class GfxRenderer;

namespace uisim {

// Demo grid that proves the host harness end to end: real GfxRenderer, real
// fonts, the shelf's two cover boxes (180x240 cell) and keyboard-driven selection.
// The production shelf screen (phase Ф3) renders through this same harness and
// replaces this scene; the scene exists so the tool is verifiable before Ф3.
class ShelfDemoScene {
 public:
  ShelfDemoScene();

  // Draws into the renderer's framebuffer; the caller flushes (displayBuffer).
  void render(GfxRenderer& renderer) const;

  // "Left"/"Right" move the selection, "Confirm" is accepted and ignored.
  void onKey(const std::string& key);

  size_t itemCount() const { return titles_.size(); }
  int selected() const { return selected_; }

 private:
  std::vector<std::string> titles_;
  int selected_ = 0;
};

}  // namespace uisim
