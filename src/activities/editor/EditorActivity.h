#pragma once

#include <GfxRenderer.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "EditorDocument.h"
#include "RomajiKana.h"
#include "SkkDictionary.h"
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
  // "" for an untitled document. Normally equal to document_.currentPath(); after
  // ":e newname" it names a file that doesn't exist yet (created by the next save).
  std::string filePath_;

  GfxRenderer::Orientation savedOrientation_ = GfxRenderer::Orientation::Portrait;
  uint16_t displayWidth_ = 0;
  uint16_t displayHeight_ = 0;

  // Grid metrics — computed in onEnter from the editor font
  static constexpr uint8_t LEFT_MARGIN = 4;
  static constexpr uint8_t TOP_MARGIN = 4;
  // Row 0 is reserved for the kanji candidate list (blank when not
  // converting), so text starts one row down and never reflows mid-conversion.
  int textTop_ = 0;
  // Strip on the physical-button edge (right side in this landscape
  // orientation) kept clear for the Back/Menu button hints.
  int hintStripW_ = 0;
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
  // Shared tail of every key: relayout, scroll, goal column, repaint.
  void afterKey(bool contentChanged, bool cursorMoved, bool isVertical);
  static void bleEventTrampoline(void* ctx, const HidKeyEvent& ev);

  // Migu 1M 12pt (owner's choice: 60x17 cells, readability over row count),
  // already loaded at boot as the UI title font (main.cpp tryLoadSdUiFonts) —
  // no extra flash or RAM.
  static constexpr int EDITOR_FONT_ID = UI_12_FONT_ID;

  void applyFontMetrics();
  void drawCandidateRow();
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
  // Kanji conversion (Hiragana mode)
  // ==========================================================================

  // The composition (kana typed since the last commit) lives in the document
  // itself at [compStart_, cursorPos_), so layout and rendering need no special
  // path: converting replaces that range with a candidate, committing just
  // forgets the range. The cursor is always at the end of the composition —
  // any cursor movement commits first.
  enum class ComposeState { None, Composing, Converting };
  ComposeState compose_ = ComposeState::None;
  uint32_t compStart_ = 0;
  std::string compReading_;  // hiragana reading while Converting
  std::vector<std::string> candidates_;
  size_t candIndex_ = 0;
  static constexpr uint32_t MAX_READING_BYTES = 96;  // 32 kana; longer compositions aren't converted

  static constexpr const char* DICT_PATH = "/dict/skk.txt";  // scripts/build_skk_dict.py output
  HalFile dictFile_;
  std::unique_ptr<SkkDictionary> dict_;  // null when DICT_PATH is missing: conversion offers kana only
  static uint32_t readDictFile(void* ctx, uint32_t pos, char* buf, uint32_t n);

  void insertKana(const std::string& kana);  // romaji output; starts a composition in Hiragana mode
  void commitComposition();                  // the single commit seam for all kana/kanji emission
  void convertStep(int dir);                 // Space (+1) / Shift+Space (-1)
  void revertConversion();                   // back to the hiragana reading
  void replaceComposition(const std::string& text);

  // ==========================================================================
  // Undo / redo (vi "u" / Ctrl-R; Ctrl+Z / Ctrl+Y), multi-level
  // ==========================================================================

  // Every document edit goes through editInsert()/editDelete(), which record
  // the current change as position-based ops. They stay valid across
  // flatten() and save (neither moves text), so saving doesn't clear undo.
  // Deleted text is copied to an SD file, not the heap.
  //
  // One array holds both stacks: undo groups grow up from index 0, redo
  // groups grow down from the top. A group is its ops in application order,
  // the first one flagged UNDO_GROUP_START. Reverting a group applies its
  // ops' inverses newest first; those inverses, in that order, are the group
  // that reverts the revert, so undo and redo are the same operation.
  struct UndoOp {
    uint32_t pos;
    uint32_t len;
    uint32_t fileOff;  // flag bits below | offset of the deleted text in UNDO_PATH
  };
  static constexpr uint32_t UNDO_INSERT_FLAG = 0x80000000u;  // op inserted [pos, pos+len); no text
  static constexpr uint32_t UNDO_GROUP_START = 0x40000000u;
  static constexpr uint32_t UNDO_OFF_MASK = 0x3FFFFFFFu;
  static constexpr uint16_t MAX_UNDO_OPS = 256;  // 3KB; a typed line coalesces into one op
  static constexpr const char* UNDO_PATH = "/.crosspoint/edit/undo.bin";
  UndoOp undoOps_[MAX_UNDO_OPS] = {};
  uint16_t undoCount_ = 0;          // undo ops at [0, undoCount_)
  uint16_t redoCount_ = 0;          // redo ops at [MAX_UNDO_OPS - redoCount_, MAX_UNDO_OPS)
  uint16_t currentGroupStart_ = 0;  // first undo index of the change being recorded
  bool changeOpen_ = false;         // the next edit extends the current change instead of starting one
  bool groupDropped_ = false;       // current change outgrew the whole array: not undoable
  uint32_t undoFileEnd_ = 0;
  HalFile undoFile_;  // member file: closed in onExit()

  bool editInsert(uint32_t pos, std::string_view text);
  void editDelete(uint32_t pos, uint32_t len);
  void closeChange() { changeOpen_ = false; }
  void openChange();
  void pushUndoOp(const UndoOp& op);
  bool copyToUndoFile(uint32_t pos, uint32_t len, uint32_t& outOff);
  bool insertFromUndoFile(uint32_t pos, uint32_t len, uint32_t fileOff);
  // Reverts the ops at [start, end) in place (each becomes its inverse, then
  // the block is reversed). Returns the lowest position touched, or UINT32_MAX on failure.
  uint32_t revertBlock(uint16_t start, uint16_t end);
  void undoLastChange();
  void redoLastChange();
  void resetUndo();

  // ==========================================================================
  // Vi mode (Settings > Controls > Editor Vi Mode)
  // ==========================================================================

  // Insert is the plain editor (romaji-kana, conversion); Esc with nothing
  // being composed switches to Normal. Normal keys are ASCII commands
  // whatever the kana mode is; ":" opens a one-line command (w, q, q!, wq).
  enum class ViMode { Insert, Normal, Command };
  bool viEnabled_ = false;  // read from SETTINGS in onEnter()
  ViMode viMode_ = ViMode::Insert;
  char viPending_ = 0;    // first key of a two-key command: 'd', 'y' or 'g'
  uint16_t viCount_ = 0;  // numeric prefix ("3j", "2dd"); 0 = none
  // Consecutive Esc presses (the one leaving Insert counts). Two in a row
  // switch input to direct ASCII, so the next Insert starts in alphabet mode.
  uint8_t viEscCount_ = 0;
  char viCommand_[64] = {};
  uint8_t viCommandLen_ = 0;
  // Linewise register for dd/yy/p/P, kept on the SD card (not the heap) so
  // yanking any number of lines costs only a 128-byte stack buffer.
  // Always ends in '\n' when non-empty.
  static constexpr const char* VI_YANK_PATH = "/.crosspoint/edit/yank.txt";
  uint32_t viYankLen_ = 0;  // bytes in the register file; 0 = empty. Reloaded in onEnter().

  // Returns false for keys Normal mode leaves to the regular handler
  // (arrows, Home/End, PgUp/PgDn, Delete).
  bool handleViNormalKey(const HidKeyEvent& ev, bool& contentChanged, bool& cursorMoved, bool& isVertical);
  void handleViCommandKey(const HidKeyEvent& ev);
  void runViCommand();
  void enterViNormal();

  char byteAt(uint32_t pos);
  uint32_t lineStartOf(uint32_t pos);
  uint32_t lineEndOf(uint32_t pos);  // offset of the line's '\n', or length()
  int charClassAt(uint32_t pos);     // word-motion class of the codepoint at pos
  void viWordForward();
  void viWordBackward();
  void viWordEnd();
  void viDeleteLines(uint32_t count);
  void viYankLines(uint32_t count);
  void viPut(bool below);
  uint32_t insertYankAt(uint32_t at, uint32_t count);  // first `count` register bytes; returns bytes inserted

  // ==========================================================================
  // Menu / file actions (M2)
  // ==========================================================================

  // Pushes a button-driven sub-activity (menu, picker, dialog) in the normal
  // UI orientation and restores the editor's landscape grid when it returns.
  void startSubActivity(std::unique_ptr<Activity> activity, ActivityResultHandler handler);

  static constexpr const char* AUTO_SAVE_DIR = "/notes";
  // "/notes/YYYYMMDD-HHMM.txt" (local time) if the RTC has a date, else
  // "/notes/memo-NNN.txt"; always a path that doesn't exist yet.
  static std::string makeAutoSavePath();

  void showEditorMenu();
  void doSave();
  void promptSaveAs();
  // Save-As target for a typed name: adds ".txt" when there's no .txt/.md
  // extension; relative names go in the current file's folder (or /notes).
  std::string resolveNamedPath(const std::string& name) const;
  void saveAs(const std::string& path);
  void openNamed(const std::string& path, bool force);  // ":e name" / ":e! name"
  void promptOpen();
  void promptNew();
  void requestExit();
  void confirmDiscardThen(std::function<void()> action);
  void switchToDocument(const std::string& newPath);
};
