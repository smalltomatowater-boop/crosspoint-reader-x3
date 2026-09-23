#include "EditorActivity.h"

#include <Arduino.h>
#include <I18n.h>
#include <Logging.h>
#include <MappedInputManager.h>
#include <string.h>

#include <algorithm>

EditorActivity::EditorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath)
    : Activity("Editor", renderer, mappedInput), filePath_(std::move(filePath)) {}

// ============================================================================
// Lifecycle
// ============================================================================

void EditorActivity::onEnter() {
  Activity::onEnter();

  LOG_DBG("MEM", "Heap before BLE init: %d", ESP.getFreeHeap());

  // Landscape-only (user decision) — save, force, restore on exit
  // (TerminalActivity precedent).
  savedOrientation_ = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
  displayWidth_ = renderer.getDisplayWidth();
  displayHeight_ = renderer.getDisplayHeight();

  // Japanese-capable monospace font, loaded from flash (no DRAM copy of
  // the bitmaps — loadFromMemory points into flash).
  if (editorFont_.loadFromMemory(MIGU1M_TERM_08, MIGU1M_TERM_08_SIZE)) {
    EpdFont* reg = editorFont_.getEpdFont(0);
    if (reg) {
      EpdFontFamily fam(reg, editorFont_.getEpdFont(1), editorFont_.getEpdFont(2), editorFont_.getEpdFont(3));
      renderer.replaceFont(EDITOR_FONT_ID, fam);
      activeFontId_ = EDITOR_FONT_ID;
      LOG_INF("EDTR", "Editor font loaded");
    }
  }

  applyFontMetrics();

  // BLE keyboard: connect at startup. The editor never touches WiFi, so
  // NimBLE's ~40KB heap cost does not collide with the WebServer problem
  // that disabled BLE in TerminalActivity.
  bleHid_.begin("CrossPoint", {this, bleEventTrampoline});

  fullRefreshNeeded_ = true;
  frameDirty_ = true;
  requestUpdate();
  LOG_DBG("MEM", "Heap after BLE init: %d", ESP.getFreeHeap());
}

void EditorActivity::onExit() {
  // Join the BLE task BEFORE activity destruction (component-owned task,
  // joined here per the BleHidClient contract).
  bleHid_.stop();
  renderer.setOrientation(savedOrientation_);
  Activity::onExit();
}

void EditorActivity::applyFontMetrics() {
  charH_ = static_cast<uint8_t>(renderer.getLineHeight(activeFontId_));
  charW_ = static_cast<uint8_t>(renderer.getTextWidth(activeFontId_, "A"));
  if (charW_ == 0) charW_ = charH_ / 2;
  // +1 row reserved for the status bar; keep cols at 80 max.
  maxCols_ = static_cast<uint8_t>(std::min<int>((displayWidth_ - LEFT_MARGIN * 2) / charW_, 80));
  maxRows_ = static_cast<uint8_t>((displayHeight_ - TOP_MARGIN) / charH_ - 1);
  LOG_INF("EDTR", "Grid: %dx%d cells (cell %dx%d px, display %dx%d px)", maxCols_, maxRows_, charW_, charH_,
          displayWidth_, displayHeight_);
}

// ============================================================================
// Input (main task)
// ============================================================================

void EditorActivity::bleEventTrampoline(void* ctx, const HidKeyEvent& ev) {
  static_cast<EditorActivity*>(ctx)->onBleKey(ev);
}

void EditorActivity::onBleKey(const HidKeyEvent& ev) {
  // M1: observe and log raw events; the editing pipeline lands in M2.
  LOG_DBG("EDTR", "HID usage=0x%02X mods=0x%02X", ev.usage, ev.mods);
}

void EditorActivity::loop() {
  // Note: gpio.update() runs once per firmware loop in main.cpp — activities
  // only poll wasReleased/isPressed.
  bleHid_.loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  // Repaint when the BLE connection state changes (status row).
  if (bleHid_.isConnected() != lastBleConnected_ || bleHid_.isScanning() != lastBleScanning_) {
    lastBleConnected_ = bleHid_.isConnected();
    lastBleScanning_ = bleHid_.isScanning();
    frameDirty_ = true;
  }

  const unsigned long now = millis();
  if (frameDirty_ && now - lastDisplayUpdate_ >= MIN_FAST_REFRESH_MS) {
    requestUpdate();
  }
}

// ============================================================================
// Rendering (render task)
// ============================================================================

void EditorActivity::drawStatusRow() {
  const int y = TOP_MARGIN + static_cast<int>(maxRows_) * charH_;

  char left[48];
  if (filePath_.empty()) {
    snprintf(left, sizeof(left), "[%s]", tr(STR_TEXT_EDIT));
  } else {
    // Show just the file name, not the full path.
    const char* base = filePath_.c_str();
    if (const char* slash = strrchr(base, '/')) base = slash + 1;
    snprintf(left, sizeof(left), "%s", base);
  }

  char right[48];
  if (bleHid_.isSubscribeFailed()) {
    snprintf(right, sizeof(right), "BLE:%s", tr(STR_EDITOR_BLE_PAIR_UNSUP));
  } else if (bleHid_.isConnected()) {
    snprintf(right, sizeof(right), "BLE:%s", tr(STR_EDITOR_BLE_CONNECTED));
  } else if (bleHid_.isScanning()) {
    snprintf(right, sizeof(right), "BLE:%s", tr(STR_EDITOR_BLE_SCANNING));
  } else {
    snprintf(right, sizeof(right), "BLE:%s", tr(STR_EDITOR_BLE_OFF));
  }

  renderer.drawText(activeFontId_, LEFT_MARGIN, y, left, true);

  char heap[24];
  snprintf(heap, sizeof(heap), " %dKB", ESP.getFreeHeap() / 1024);
  const int rightW = renderer.getTextWidth(activeFontId_, right) + renderer.getTextWidth(activeFontId_, heap);
  const int rightX = displayWidth_ - LEFT_MARGIN - rightW;
  if (rightX > LEFT_MARGIN) {
    renderer.drawText(activeFontId_, rightX, y, right, true);
    renderer.drawText(activeFontId_, rightX + renderer.getTextWidth(activeFontId_, right), y, heap, true);
  }
}

void EditorActivity::render(RenderLock&& lock) {
  renderer.clearScreen(0xFF);

  const int midY = displayHeight_ / 2 - charH_ * 2;
  renderer.drawText(activeFontId_, 8, midY, tr(STR_TEXT_EDIT), true);
  renderer.drawText(activeFontId_, 8, midY + charH_, "(M1 skeleton)", true);

  drawStatusRow();

  renderer.displayBuffer(fullRefreshNeeded_ ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  frameDirty_ = false;
  fullRefreshNeeded_ = false;
  lastDisplayUpdate_ = millis();
}
