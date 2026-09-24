#pragma once

#include <GfxRenderer.h>

#include <functional>
#include <string>

#include "EditorDocument.h"
#include "RomajiKana.h"
#include "activities/Activity.h"
#include "ble/BleHidClient.h"
#include "fontIds.h"

// Terminal-style text editor driven by a BLE HID keyboard.
// Grid rendering + BLE connection lifecycle from M1; M2 adds the document (piece
// table via EditorDocument), romaji->kana input (RomajiKana), keyboard-only cursor
// movement, the Save/SaveAs/Open/New/Exit menu, and crash-safe save.
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
  std::string filePath_;  // "" for a never-saved document; kept in sync with document_.currentPath()

  GfxRenderer::Orientation savedOrientation_ = GfxRenderer::Orientation::Portrait;
  uint16_t displayWidth_ = 0;
  uint16_t displayHeight_ = 0;

  // Grid metrics — computed in onEnter from the editor font
  static constexpr uint8_t LEFT_MARGIN = 4;
  static constexpr uint8_t TOP_MARGIN = 4;
  uint8_t charW_ = 10;  // ASCII cell width = glyph advance (not ink width) of the monospace half-width font
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

  // Migu 1M 12pt (owner's choice: 60x17 cells, readability over row count),
  // already loaded at boot as the UI title font (main.cpp tryLoadSdUiFonts) —
  // no extra flash or RAM.
  static constexpr int EDITOR_FONT_ID = UI_12_FONT_ID;

  void applyFontMetrics();
  void drawStatusRow();

  // ==========================================================================
  // Document, cursor and viewport (M2)
  // ==========================================================================

  EditorDocument document_;
  RomajiKana romajiKana_;

  enum class InputMode { Hiragana, Katakana, Ascii };
  InputMode inputMode_ = InputMode::Hiragana;
  void cycleInputMode();

  uint32_t cursorPos_ = 0;  // byte offset into the logical document
  int goalCol_ = 0;         // preserved across consecutive Up/Down; resnapped by any other move

  // Small write-combining/idle-maintenance state — flatten() and flushEditHead() are
  // both "safe to call anytime, expensive-ish, so only do it when nothing has been
  // typed for a bit" (see EditorDocument.h for why).
  unsigned long lastEditTime_ = 0;
  bool idleMaintenanceDone_ = true;
  static constexpr unsigned long IDLE_MAINTENANCE_MS = 2000;

  // Viewport: a small (~4KB) materialized window of the document starting at
  // viewportStart_, re-laid-out into display rows on every cursor/content change.
  // This is the "windows" budget from handoff.md — never the whole document.
  static constexpr uint32_t VIEWPORT_BUF_SIZE = 4096;
  static constexpr uint8_t MAX_GRID_ROWS = 32;  // headroom above any realistic maxRows_
  char viewportBuf_[VIEWPORT_BUF_SIZE] = {};
  uint32_t viewportStart_ = 0;
  uint32_t rowStart_[MAX_GRID_ROWS + 1] = {};  // logical doc offsets; rowStart_[rowCount_] is one-past-the-end
  bool rowEndsWithNewline_[MAX_GRID_ROWS] = {};
  uint8_t rowCount_ = 0;
  int cursorRow_ = -1;  // -1 when the cursor isn't within the currently laid-out viewport
  int cursorCol_ = 0;

  // Re-walks the document from viewportStart_ into rowStart_/rowEndsWithNewline_ (and
  // locates the cursor within that layout, if it falls inside it). Call after any
  // change to the document or to cursorPos_/viewportStart_.
  void relayout();
  // If relayout() didn't find the cursor in the current viewport, scrolls one row at
  // a time (per handoff's cursor semantics) until it does.
  void ensureCursorVisible();

  static uint32_t decodeUtf8At(const char* buf, uint32_t avail, uint32_t& outLen);
  uint32_t prevCodepointLen(uint32_t pos);  // bytes of the codepoint ending at pos
  uint32_t nextCodepointLen(uint32_t pos);  // bytes of the codepoint starting at pos

  void moveCursorVertically(int rows);
  void moveCursorOneRow(int dir);
  void moveCursorToRowEdge(bool toStart);
  // Walks forward from rowStart counting display cells, returning the byte offset of
  // the `targetCol`-th cell (or rowEnd, whichever comes first).
  uint32_t clampToRowColumn(uint32_t rowStart, uint32_t rowEnd, int targetCol);
  // Bounded backward scan (see EditorActivity.cpp) for the display row immediately
  // before `beforePos` — used to scroll the viewport up.
  uint32_t findPreviousRowStart(uint32_t beforePos);

  void commitPendingKana();  // flushes any in-progress romaji composition into the document
  void insertText(const std::string& text);

  // ==========================================================================
  // Menu / file actions (M2)
  // ==========================================================================

  void showEditorMenu();
  void doSave();
  void promptSaveAs();
  void promptOpen();
  void promptNew();
  void requestExit();
  void confirmDiscardThen(std::function<void()> action);
  void switchToDocument(const std::string& newPath);
};
