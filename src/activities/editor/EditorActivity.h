#pragma once

#include <GfxRenderer.h>
#include <SdCardFont.h>
#include <builtinFonts/migu1m_term_08.h>

#include <string>

#include "activities/Activity.h"
#include "ble/BleHidClient.h"
#include "fontIds.h"

// Terminal-style text editor driven by a BLE HID keyboard.
// M1 skeleton: landscape grid, Japanese-capable font, BLE connection with
// status surfacing; editing arrives in M2, kana input in M3.
class EditorActivity final : public Activity {
 public:
  explicit EditorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath = "");

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return true; }
  bool preventAutoSleep() override { return true; }

 private:
  std::string filePath_;

  GfxRenderer::Orientation savedOrientation_ = GfxRenderer::Orientation::Portrait;
  uint16_t displayWidth_ = 0;
  uint16_t displayHeight_ = 0;

  // Grid metrics — computed in onEnter from the editor font
  static constexpr uint8_t LEFT_MARGIN = 4;
  static constexpr uint8_t TOP_MARGIN = 4;
  uint8_t charW_ = 10;  // ASCII cell width (monospace half-width)
  uint8_t charH_ = 19;  // line height
  uint8_t maxCols_ = 0;
  uint8_t maxRows_ = 0;  // text rows; +1 status row below

  // Frame state
  bool frameDirty_ = false;
  bool fullRefreshNeeded_ = true;
  unsigned long lastDisplayUpdate_ = 0;
  static constexpr unsigned long MIN_FULL_REFRESH_MS = 1500;
  static constexpr unsigned long MIN_FAST_REFRESH_MS = 150;

  // BLE keyboard
  BleHidClient bleHid_;
  bool lastBleConnected_ = false;
  bool lastBleScanning_ = false;
  void onBleKey(const HidKeyEvent& ev);
  static void bleEventTrampoline(void* ctx, const HidKeyEvent& ev);

  // Editor font: same Migu 1M 8pt bitmaps as the terminal, registered under
  // an editor-private id so the two activities never fight over a slot.
  static constexpr int EDITOR_FONT_ID = 0x45445430;  // "EDT0"
  SdCardFont editorFont_;
  int activeFontId_ = UI_10_FONT_ID;

  void applyFontMetrics();
  void drawStatusRow();
};
