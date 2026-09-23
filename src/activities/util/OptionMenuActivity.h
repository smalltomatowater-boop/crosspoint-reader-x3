#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Generic vertical list chooser: give it a title and a list of (label, action id)
// entries, get back the chosen id via a MenuResult (see ActivityResult.h) or
// isCancelled == true on Back. For small button-driven menus where a dedicated
// Activity subclass would just be this with fixed data — e.g. the editor's
// Save/SaveAs/Open/New/Exit menu.
class OptionMenuActivity final : public Activity {
 public:
  struct Option {
    std::string label;
    int action;
  };

  // `initialSelectedAction` pre-selects the option with that action id, if any
  // (defaults to the first item).
  OptionMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                     std::vector<Option> options, int initialSelectedAction = -1);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string title_;
  std::vector<Option> options_;
  int selectedIndex_ = 0;
  ButtonNavigator buttonNavigator_;
};
