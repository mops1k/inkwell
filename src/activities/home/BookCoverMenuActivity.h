#pragma once

#include "../MenuListActivity.h"

class BookCoverMenuActivity final : public MenuListActivity {
 public:
  enum class Action { Set = 1, Reset = 2, Read = 3 };

  BookCoverMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool hasCustomCover,
                        bool allowRead = false);
  void render(RenderLock&&) override;

 private:
  void onActionSelected(int index) override;
};
