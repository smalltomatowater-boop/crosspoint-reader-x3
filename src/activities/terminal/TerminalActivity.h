#pragma once

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <SdCardFont.h>
#include <WebServer.h>
#include <builtinFonts/migu1m_term_08.h>

#include <memory>
#include <string>
#include <vector>

#include "BleKeyboard.h"
#include "activities/Activity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "fontIds.h"

class TerminalActivity final : public Activity {
 public:
  explicit TerminalActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Terminal", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return true; }
  bool preventAutoSleep() override { return true; }

 private:
  GfxRenderer::Orientation savedOrientation = GfxRenderer::Orientation::Portrait;
  uint16_t displayWidth = 0;
  uint16_t displayHeight = 0;

  // Font metrics — computed in onEnter from UI_12_FONT_ID (Migu1M)
  static constexpr uint8_t LEFT_MARGIN = 4;
  static constexpr uint8_t TOP_MARGIN = 4;
  uint8_t charW_ = 14;  // ASCII cell width (monospace half-width)
  uint8_t charH_ = 28;  // line height
  uint8_t maxCols = 0;
  uint8_t maxRows = 0;

  // Frame state
  std::vector<std::string> rows;
  uint16_t cursorX = 0;
  uint16_t cursorY = 0;
  bool frameReceived = false;
  bool frameDirty = false;
  bool fullRefreshNeeded = true;
  unsigned long lastDisplayUpdate = 0;

  static constexpr unsigned long MIN_FULL_REFRESH_MS = 1800;
  static constexpr unsigned long MIN_FAST_REFRESH_MS = 450;

  static constexpr int TERM_8_FONT_ID = 0x54524D38;  // "TRM8"
  SdCardFont termFont8_;
  int activeFontId_ = UI_10_FONT_ID;

  // HTTP server
  std::unique_ptr<WebServer> server;

  // BLE keyboard
  BleKeyboard bleKeyboard_;
  std::string macMiniUrl_;
  bool pendingBleInit_ = false;
  unsigned long bleInitAfter_ = 0;

  void sendKeyToMacMini(const std::string& key);
  static std::string loadMacMiniUrl();

  void startServer();
  void stopServer();
  void handleRoot();
  void handleStatus();
  void handleFrame();
  void handleFontSize();
  void handleDisplayClear();
  void handleExitTerminal();

  void applyFontMetrics();

  bool applyFrame(JsonDocument& doc);
  void drawFrame();
  void drawCursor(uint16_t col, uint16_t row);

  static std::string limitLine(const std::string& s, uint8_t maxLen);
};
