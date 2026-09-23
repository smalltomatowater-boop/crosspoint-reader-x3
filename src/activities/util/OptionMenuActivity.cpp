#include "OptionMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"

OptionMenuActivity::OptionMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                       std::vector<Option> options, int initialSelectedAction)
    : Activity("OptionMenu", renderer, mappedInput), title_(std::move(title)), options_(std::move(options)) {
  if (initialSelectedAction >= 0) {
    for (size_t i = 0; i < options_.size(); ++i) {
      if (options_[i].action == initialSelectedAction) {
        selectedIndex_ = static_cast<int>(i);
        break;
      }
    }
  }
}

void OptionMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void OptionMenuActivity::loop() {
  const int total = static_cast<int>(options_.size());
  buttonNavigator_.onNext([this, total] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, total);
    requestUpdate();
  });
  buttonNavigator_.onPrevious([this, total] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, total);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (options_.empty()) return;
    setResult(MenuResult{options_[selectedIndex_].action});
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{-1};
    setResult(std::move(result));
    finish();
    return;
  }
}

void OptionMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title_.c_str());

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  GUI.drawList(renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, static_cast<int>(options_.size()),
               selectedIndex_, [this](int index) { return options_[index].label; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
