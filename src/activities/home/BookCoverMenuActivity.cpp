#include "BookCoverMenuActivity.h"

#include <I18n.h>

#include "../ActivityResult.h"
#include "components/UITheme.h"

BookCoverMenuActivity::BookCoverMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const bool hasCustomCover, const bool allowRead)
    : MenuListActivity("BookCoverMenu", renderer, mappedInput) {
  // Reading belongs at the top of the card's action list: it is what the card
  // is usually opened for. Home keeps the cover-only menu (allowRead=false).
  if (allowRead) {
    menuItems.push_back(SettingInfo::Action(StrId::STR_OPEN, SettingAction::None));
  }
  menuItems.push_back(SettingInfo::Action(StrId::STR_SET_BOOK_COVER, SettingAction::None));
  if (hasCustomCover) {
    menuItems.push_back(SettingInfo::Action(StrId::STR_RESET_BOOK_COVER, SettingAction::None));
  }
}

void BookCoverMenuActivity::onActionSelected(const int index) {
  if (index < 0 || index >= static_cast<int>(menuItems.size())) return;
  Action action;
  if (menuItems[index].nameId == StrId::STR_OPEN) {
    action = Action::Read;
  } else if (menuItems[index].nameId == StrId::STR_SET_BOOK_COVER) {
    action = Action::Set;
  } else if (menuItems[index].nameId == StrId::STR_RESET_BOOK_COVER) {
    action = Action::Reset;
  } else {
    return;
  }

  MenuResult menuResult;
  menuResult.action = static_cast<int>(action);
  ActivityResult activityResult{std::move(menuResult)};
  activityResult.isCancelled = false;
  setResult(std::move(activityResult));
  finish();
}

void BookCoverMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, true);
  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_COVER));
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  drawMenuList(
      Rect{contentRect.x, contentTop, contentRect.width, contentRect.height - contentTop - metrics.verticalSpacing});
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
