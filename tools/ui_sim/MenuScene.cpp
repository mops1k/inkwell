#include "MenuScene.h"

#include <algorithm>
#include <string>

#include "components/UITheme.h"

namespace uisim {

MenuScene::MenuScene() {
  // The same entries HomeActivity puts under the recent-books carousel, in the
  // same order: the library first, then the file browser and the settings menu.
  items_ = {
      {"Библиотека", Library}, {"Недавние", Recent},          {"Файлы", Folder},
      {"Настройки", Settings}, {"Передача файлов", Transfer}, {"Wi-Fi", Wifi},
  };
}

void MenuScene::onKey(const std::string& key) {
  const int last = static_cast<int>(items_.size()) - 1;
  if (key == "Up") selected_ = std::clamp(selected_ - 1, 0, last);
  if (key == "Down") selected_ = std::clamp(selected_ + 1, 0, last);
}

void MenuScene::render(GfxRenderer& renderer) const {
  renderer.clearScreen();

  const BaseTheme& theme = UITheme::getInstance().getTheme();
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  // Same content box the activities use: it already excludes the hint bar and,
  // on X3, both side strips the side hints live in.
  const Rect content = UITheme::getContentRect(renderer, /*hasBottomHints=*/true, /*hasSideHints=*/true);

  theme.drawHeader(renderer, Rect{content.x, content.y, content.width, metrics.headerHeight}, "Главное меню", nullptr);

  const int listTop = content.y + metrics.headerHeight + metrics.verticalSpacing;
  const Rect listRect{content.x, listTop, content.width, content.y + content.height - listTop};
  theme.drawList(
      renderer, listRect, static_cast<int>(items_.size()), selected_,
      [this](int index) { return std::string(items_[index].title); }, nullptr,
      [this](int index) { return items_[index].icon; });

  theme.drawButtonHints(renderer, "", "Выбрать", "Вверх", "Вниз");
  theme.drawSideButtonHints(renderer, "Вверх", "Вниз");
}

}  // namespace uisim
