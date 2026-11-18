#pragma once

#include <GfxRenderer.h>

#include <string>
#include <vector>

#include "Scene.h"
#include "components/themes/BaseTheme.h"

// Host scene that reproduces the HomeActivity main menu: the theme's header,
// its list with row icons and its button-hint bar (bottom capsules + side
// hints). Nothing here draws a frame itself — the chrome comes from
// BaseTheme/LyraTheme so the frame shows what the device draws.
namespace uisim {

class MenuScene : public Scene {
 public:
  MenuScene();

  void onKey(const std::string& key) override;
  void render(GfxRenderer& renderer) const override;

 private:
  struct Item {
    const char* title;
    UIIcon icon;
  };

  std::vector<Item> items_;
  int selected_ = 0;
};

}  // namespace uisim
