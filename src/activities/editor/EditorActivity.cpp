#include "EditorActivity.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <I18n.h>
#include <Logging.h>
#include <Utf8.h>
#include <string.h>

#include <algorithm>
#include <variant>
#include <vector>

#include "activities/home/FileBrowserActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/OptionMenuActivity.h"

namespace {

// Standard USB HID Keyboard/Keypad usage codes this editor cares about. Deliberately
// limited to the universally-standard Boot Keyboard range (letters, digits, the
// common punctuation keys, and the plain navigation/control keys) — the JIS
// international usages (Henkan/Muhenkan/Katakana toggle, ~0x87-0x92) are left
// unmapped because their exact assignments aren't something to guess at without a
// device to verify against (see CLAUDE.md's anti-hallucination rule). Tab instead
// cycles Hiragana -> Katakana -> ASCII input mode, which needs no JIS-specific
// knowledge and works on any keyboard.
constexpr uint8_t HID_ENTER = 0x28;
constexpr uint8_t HID_BACKSPACE = 0x2A;
constexpr uint8_t HID_TAB = 0x2B;
constexpr uint8_t HID_SPACE = 0x2C;
constexpr uint8_t HID_DELETE = 0x4C;
constexpr uint8_t HID_HOME = 0x4A;
constexpr uint8_t HID_PAGE_UP = 0x4B;
constexpr uint8_t HID_END = 0x4D;
constexpr uint8_t HID_PAGE_DOWN = 0x4E;
constexpr uint8_t HID_RIGHT = 0x4F;
constexpr uint8_t HID_LEFT = 0x50;
constexpr uint8_t HID_DOWN = 0x51;
constexpr uint8_t HID_UP = 0x52;

constexpr uint8_t MOD_LSHIFT = 0x02;
constexpr uint8_t MOD_RSHIFT = 0x20;
bool hasShift(uint8_t mods) { return (mods & (MOD_LSHIFT | MOD_RSHIFT)) != 0; }

struct AsciiPair {
  char plain;
  char shifted;
};
// usage 0x04..0x27 (a-z, then 1-9,0) -> kAsciiTable[usage - 0x04]
constexpr AsciiPair kAsciiTable[] = {
    {'a', 'A'}, {'b', 'B'}, {'c', 'C'}, {'d', 'D'}, {'e', 'E'}, {'f', 'F'}, {'g', 'G'}, {'h', 'H'}, {'i', 'I'},
    {'j', 'J'}, {'k', 'K'}, {'l', 'L'}, {'m', 'M'}, {'n', 'N'}, {'o', 'O'}, {'p', 'P'}, {'q', 'Q'}, {'r', 'R'},
    {'s', 'S'}, {'t', 'T'}, {'u', 'U'}, {'v', 'V'}, {'w', 'W'}, {'x', 'X'}, {'y', 'Y'}, {'z', 'Z'}, {'1', '!'},
    {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'}, {'6', '^'}, {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'},
};

struct PunctPair {
  uint8_t usage;
  char plain;
  char shifted;
};
constexpr PunctPair kPunctTable[] = {
    {0x2D, '-', '_'},  {0x2E, '=', '+'}, {0x2F, '[', '{'}, {0x30, ']', '}'}, {0x31, '\\', '|'}, {0x33, ';', ':'},
    {0x34, '\'', '"'}, {0x35, '`', '~'}, {0x36, ',', '<'}, {0x37, '.', '>'}, {0x38, '/', '?'},
};

bool hidUsageToAscii(uint8_t usage, uint8_t mods, char& outChar) {
  if (usage >= 0x04 && usage <= 0x27) {
    const AsciiPair& p = kAsciiTable[usage - 0x04];
    outChar = hasShift(mods) ? p.shifted : p.plain;
    return true;
  }
  if (usage == HID_SPACE) {
    outChar = ' ';
    return true;
  }
  for (const auto& pp : kPunctTable) {
    if (pp.usage == usage) {
      outChar = hasShift(mods) ? pp.shifted : pp.plain;
      return true;
    }
  }
  return false;
}

std::string utf8Encode3(uint32_t cp) {
  const char b[3] = {static_cast<char>(0xE0 | (cp >> 12)), static_cast<char>(0x80 | ((cp >> 6) & 0x3F)),
                     static_cast<char>(0x80 | (cp & 0x3F))};
  return std::string(b, 3);
}

// Kana-mode rendering of a non-romaji key, matching common Japanese IME
// defaults: digits and symbols become full-width (U+FF01-FF5E), with the usual
// Japanese punctuation for , . [ ] /. Letters (Shift+letter) and space stay
// half-width so English words and spacing mix in naturally.
std::string kanaModeSymbol(char ch) {
  switch (ch) {
    case ',':
      return utf8Encode3(0x3001);  // 、
    case '.':
      return utf8Encode3(0x3002);  // 。
    case '[':
      return utf8Encode3(0x300C);  // 「
    case ']':
      return utf8Encode3(0x300D);  // 」
    case '/':
      return utf8Encode3(0x30FB);  // ・
    default:
      break;
  }
  const bool isLetter = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
  if (ch > ' ' && ch <= '~' && !isLetter) {
    return utf8Encode3(static_cast<uint32_t>(ch) + 0xFEE0);
  }
  return std::string(1, ch);
}

constexpr uint32_t ROW_BUF_SIZE = 200;  // 80 cols worst case (all-kana) is ~120 bytes; stays under the 256B stack rule

}  // namespace

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

  applyFontMetrics();

  if (!document_.open(filePath_)) {
    LOG_ERR("EDTR", "Failed to open document: %s — starting blank", filePath_.c_str());
    document_.open("");
    filePath_.clear();
  }
  cursorPos_ = 0;
  viewportStart_ = 0;
  goalCol_ = 0;
  relayout();

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
  document_.close();
  renderer.setOrientation(savedOrientation_);
  Activity::onExit();
}

void EditorActivity::applyFontMetrics() {
  const int availH = displayHeight_ - TOP_MARGIN;
  charH_ = static_cast<uint8_t>(renderer.getLineHeight(EDITOR_FONT_ID));

  // Advance, not getTextWidth(): the latter is the glyph's ink bounding box,
  // narrower than the pen advance drawText actually steps by — using it made
  // the cursor drift further left the further right it went.
  charW_ = static_cast<uint8_t>(renderer.getTextAdvanceX(EDITOR_FONT_ID, "A", EpdFontFamily::REGULAR));
  if (charW_ == 0) charW_ = charH_ / 2;
  LOG_INF("EDTR", "Advance: 'A'=%d px, 'AA'=%d px, wide=%d px", charW_,
          renderer.getTextAdvanceX(EDITOR_FONT_ID, "AA", EpdFontFamily::REGULAR),
          renderer.getTextAdvanceX(EDITOR_FONT_ID, "\xe3\x81\x82", EpdFontFamily::REGULAR));
  // +1 row reserved for the status bar; keep cols at 80 max.
  maxCols_ = static_cast<uint8_t>(std::min<int>((displayWidth_ - LEFT_MARGIN * 2) / charW_, 80));
  const uint8_t rows = static_cast<uint8_t>(availH / charH_ - 1);
  maxRows_ = std::min<uint8_t>(rows, MAX_GRID_ROWS);
  LOG_INF("EDTR", "Grid: %dx%d cells (cell %dx%d px, display %dx%d px)", maxCols_, maxRows_, charW_, charH_,
          displayWidth_, displayHeight_);
}

// ============================================================================
// Input (main task)
// ============================================================================

void EditorActivity::bleEventTrampoline(void* ctx, const HidKeyEvent& ev) {
  static_cast<EditorActivity*>(ctx)->onBleKey(ev);
}

void EditorActivity::cycleInputMode() {
  commitPendingKana();
  switch (inputMode_) {
    case InputMode::Hiragana:
      inputMode_ = InputMode::Katakana;
      romajiKana_.setMode(RomajiKana::Mode::Katakana);
      break;
    case InputMode::Katakana:
      inputMode_ = InputMode::Ascii;
      break;
    case InputMode::Ascii:
      inputMode_ = InputMode::Hiragana;
      romajiKana_.setMode(RomajiKana::Mode::Hiragana);
      break;
  }
}

void EditorActivity::commitPendingKana() {
  if (!romajiKana_.hasPending()) return;
  const std::string text = romajiKana_.flushPending();
  if (!text.empty()) insertText(text);
}

void EditorActivity::insertText(const std::string& text) {
  if (text.empty()) return;
  if (document_.insertAt(cursorPos_, text)) {
    cursorPos_ += static_cast<uint32_t>(text.size());
  } else {
    LOG_ERR("EDTR", "Failed to insert %zu bytes at %u", text.size(), cursorPos_);
  }
}

void EditorActivity::onBleKey(const HidKeyEvent& ev) {
  bool contentChanged = false;
  bool cursorMoved = false;
  const bool isVertical =
      (ev.usage == HID_UP || ev.usage == HID_DOWN || ev.usage == HID_PAGE_UP || ev.usage == HID_PAGE_DOWN);

  switch (ev.usage) {
    case HID_TAB:
      cycleInputMode();
      frameDirty_ = true;
      return;

    case HID_ENTER:
      commitPendingKana();
      insertText("\n");
      contentChanged = true;
      break;

    case HID_BACKSPACE:
      if (romajiKana_.hasPending()) {
        romajiKana_.clear();
      } else if (cursorPos_ > 0) {
        const uint32_t len = prevCodepointLen(cursorPos_);
        document_.deleteAt(cursorPos_ - len, len);
        cursorPos_ -= len;
        contentChanged = true;
      }
      break;

    case HID_DELETE:
      commitPendingKana();
      if (cursorPos_ < document_.length()) {
        const uint32_t len = nextCodepointLen(cursorPos_);
        document_.deleteAt(cursorPos_, len);
        contentChanged = true;
      }
      break;

    case HID_LEFT:
      commitPendingKana();
      if (cursorPos_ > 0) cursorPos_ -= prevCodepointLen(cursorPos_);
      cursorMoved = true;
      break;

    case HID_RIGHT:
      commitPendingKana();
      if (cursorPos_ < document_.length()) cursorPos_ += nextCodepointLen(cursorPos_);
      cursorMoved = true;
      break;

    case HID_UP:
      commitPendingKana();
      moveCursorVertically(-1);
      cursorMoved = true;
      break;

    case HID_DOWN:
      commitPendingKana();
      moveCursorVertically(1);
      cursorMoved = true;
      break;

    case HID_PAGE_UP:
      commitPendingKana();
      moveCursorVertically(-static_cast<int>(maxRows_));
      cursorMoved = true;
      break;

    case HID_PAGE_DOWN:
      commitPendingKana();
      moveCursorVertically(static_cast<int>(maxRows_));
      cursorMoved = true;
      break;

    case HID_HOME:
      commitPendingKana();
      moveCursorToRowEdge(/*toStart=*/true);
      cursorMoved = true;
      break;

    case HID_END:
      commitPendingKana();
      moveCursorToRowEdge(/*toStart=*/false);
      cursorMoved = true;
      break;

    default: {
      char ch;
      if (hidUsageToAscii(ev.usage, ev.mods, ch)) {
        const bool romajiEligible =
            inputMode_ != InputMode::Ascii && ((ch >= 'a' && ch <= 'z') || ch == '\'' || ch == '-');
        if (romajiEligible) {
          const std::string kana = romajiKana_.feed(ch);
          if (!kana.empty()) {
            insertText(kana);
            contentChanged = true;
          }
        } else {
          commitPendingKana();
          insertText(inputMode_ != InputMode::Ascii ? kanaModeSymbol(ch) : std::string(1, ch));
          contentChanged = true;
        }
      }
      break;
    }
  }

  if (contentChanged) {
    lastEditTime_ = millis();
    idleMaintenanceDone_ = false;
  }

  if (contentChanged || cursorMoved) {
    relayout();
    ensureCursorVisible();
    if (!isVertical) goalCol_ = cursorCol_;
    frameDirty_ = true;
  }
}

void EditorActivity::loop() {
  // Note: gpio.update() runs once per firmware loop in main.cpp — activities
  // only poll wasReleased/isPressed.
  bleHid_.loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    showEditorMenu();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    requestExit();
    return;
  }

  // Repaint when the BLE connection state changes (status row).
  if (bleHid_.isConnected() != lastBleConnected_ || bleHid_.isScanning() != lastBleScanning_) {
    lastBleConnected_ = bleHid_.isConnected();
    lastBleScanning_ = bleHid_.isScanning();
    frameDirty_ = true;
  }

  const unsigned long now = millis();
  if (!idleMaintenanceDone_ && now - lastEditTime_ >= IDLE_MAINTENANCE_MS) {
    document_.flushEditHead();
    if (document_.needsFlatten()) {
      document_.flatten();
    }
    idleMaintenanceDone_ = true;
  }

  if (frameDirty_ && now - lastDisplayUpdate_ >= MIN_FAST_REFRESH_MS) {
    requestUpdate();
  }
}

// ============================================================================
// Document, cursor and viewport
// ============================================================================

uint32_t EditorActivity::decodeUtf8At(const char* buf, uint32_t avail, uint32_t& outLen) {
  const uint8_t b0 = static_cast<uint8_t>(buf[0]);
  uint32_t len;
  uint32_t cp;
  if (b0 < 0x80) {
    len = 1;
    cp = b0;
  } else if ((b0 & 0xE0) == 0xC0) {
    len = 2;
    cp = b0 & 0x1F;
  } else if ((b0 & 0xF0) == 0xE0) {
    len = 3;
    cp = b0 & 0x0F;
  } else if ((b0 & 0xF8) == 0xF0) {
    len = 4;
    cp = b0 & 0x07;
  } else {
    outLen = 1;
    return REPLACEMENT_GLYPH;
  }

  if (len > avail) {
    outLen = 1;  // truncated at the edge of what's buffered; skip just the lead byte
    return REPLACEMENT_GLYPH;
  }

  for (uint32_t i = 1; i < len; ++i) {
    const uint8_t b = static_cast<uint8_t>(buf[i]);
    if ((b & 0xC0) != 0x80) {
      outLen = 1;
      return REPLACEMENT_GLYPH;
    }
    cp = (cp << 6) | (b & 0x3F);
  }
  outLen = len;
  return cp;
}

uint32_t EditorActivity::prevCodepointLen(uint32_t pos) {
  if (pos == 0) return 0;
  const uint32_t peekStart = (pos > 4) ? pos - 4 : 0;
  char buf[4];
  const uint32_t got = document_.readAt(peekStart, buf, pos - peekStart);
  for (uint32_t i = 1; i <= got; ++i) {
    const uint8_t b = static_cast<uint8_t>(buf[got - i]);
    if ((b & 0xC0) != 0x80) return i;  // found the lead byte of the last codepoint
  }
  return 1;  // fallback; shouldn't happen for valid UTF-8
}

uint32_t EditorActivity::nextCodepointLen(uint32_t pos) {
  char buf[4];
  const uint32_t got = document_.readAt(pos, buf, sizeof(buf));
  if (got == 0) return 0;
  uint32_t len;
  decodeUtf8At(buf, got, len);
  return len;
}

void EditorActivity::relayout() {
  const uint32_t got = document_.readAt(viewportStart_, viewportBuf_, VIEWPORT_BUF_SIZE);

  rowCount_ = 0;
  uint32_t bufPos = 0;
  int col = 0;
  cursorRow_ = -1;
  cursorCol_ = 0;
  bool cursorFound = false;

  rowStart_[0] = viewportStart_;

  while (rowCount_ < maxRows_ && bufPos < got) {
    const uint32_t docPos = viewportStart_ + bufPos;
    if (!cursorFound && docPos == cursorPos_) {
      cursorRow_ = rowCount_;
      cursorCol_ = col;
      cursorFound = true;
    }

    uint32_t cpLen;
    const uint32_t cp = decodeUtf8At(viewportBuf_ + bufPos, got - bufPos, cpLen);

    if (cp == '\n') {
      rowEndsWithNewline_[rowCount_] = true;
      bufPos += cpLen;
      rowCount_++;
      rowStart_[rowCount_] = viewportStart_ + bufPos;
      col = 0;
      continue;
    }

    const int w = utf8IsCjkBreakable(cp) ? 2 : 1;
    if (col + w > maxCols_) {
      rowEndsWithNewline_[rowCount_] = false;
      rowCount_++;
      rowStart_[rowCount_] = viewportStart_ + bufPos;
      col = 0;
      continue;  // re-examine this same character against the fresh row
    }

    col += w;
    bufPos += cpLen;
  }

  if (!cursorFound && rowCount_ < maxRows_ && viewportStart_ + bufPos == cursorPos_) {
    cursorRow_ = rowCount_;
    cursorCol_ = col;
  }

  // Close out whatever row was in progress when we ran out of buffered bytes (true
  // EOF, or — for a pathologically long line — the layout window itself). Not
  // reached if the loop stopped because rowCount_ == maxRows_.
  if (rowCount_ < maxRows_) {
    rowEndsWithNewline_[rowCount_] = false;
    rowStart_[rowCount_ + 1] = viewportStart_ + bufPos;
    rowCount_++;
  }
}

void EditorActivity::ensureCursorVisible() {
  int guard = 0;
  while (cursorRow_ < 0 && guard++ < static_cast<int>(maxRows_) + 4) {
    if (cursorPos_ < viewportStart_) {
      viewportStart_ = findPreviousRowStart(viewportStart_);
    } else {
      if (rowCount_ <= 1) break;  // nothing further to scroll to
      viewportStart_ = rowStart_[1];
    }
    relayout();
  }
}

uint32_t EditorActivity::clampToRowColumn(uint32_t rowStart, uint32_t rowEnd, int targetCol) {
  char buf[ROW_BUF_SIZE];
  const uint32_t avail = (rowEnd > rowStart) ? std::min<uint32_t>(rowEnd - rowStart, ROW_BUF_SIZE) : 0;
  const uint32_t got = (avail > 0) ? document_.readAt(rowStart, buf, avail) : 0;

  uint32_t pos = 0;
  int col = 0;
  while (pos < got && col < targetCol) {
    uint32_t len;
    const uint32_t cp = decodeUtf8At(buf + pos, got - pos, len);
    const int w = utf8IsCjkBreakable(cp) ? 2 : 1;
    if (col + w > targetCol) break;  // landing between the two cells of a wide char: stop before it
    col += w;
    pos += len;
  }
  return rowStart + pos;
}

uint32_t EditorActivity::findPreviousRowStart(uint32_t beforePos) {
  if (beforePos == 0) return 0;
  const uint32_t scanCap = std::min<uint32_t>(beforePos, VIEWPORT_BUF_SIZE - 1);
  const uint32_t scanStart = beforePos - scanCap;

  // Borrow viewportBuf_ as scratch — relayout() will overwrite it right after this
  // call anyway, so nothing else depends on its contents surviving this scan.
  const uint32_t got = document_.readAt(scanStart, viewportBuf_, scanCap);

  // The byte right before beforePos may be the newline that *ends* the previous
  // row — it belongs to that row, not to a line before it, so it must not count
  // as a line start. Including it made lineStart == beforePos and the viewport
  // could never scroll up past a line break.
  uint32_t lineStart = scanStart;
  for (uint32_t i = 0; i + 1 < got; ++i) {
    if (viewportBuf_[i] == '\n') lineStart = scanStart + i + 1;
  }

  uint32_t rowStart = lineStart;
  uint32_t pos = lineStart;
  int col = 0;
  while (pos < beforePos) {
    const uint32_t bufOff = pos - scanStart;
    if (bufOff >= got) break;
    uint32_t len;
    const uint32_t cp = decodeUtf8At(viewportBuf_ + bufOff, got - bufOff, len);
    if (cp == '\n') {
      pos += len;
      if (pos >= beforePos) break;
      rowStart = pos;
      col = 0;
      continue;
    }
    const int w = utf8IsCjkBreakable(cp) ? 2 : 1;
    if (col + w > maxCols_) {
      rowStart = pos;
      col = 0;
      continue;
    }
    col += w;
    pos += len;
  }
  return rowStart;
}

void EditorActivity::moveCursorOneRow(int dir) {
  if (dir < 0) {
    if (cursorRow_ > 0) {
      const uint32_t targetRowStart = rowStart_[cursorRow_ - 1];
      uint32_t targetRowEnd = rowStart_[cursorRow_];
      if (rowEndsWithNewline_[cursorRow_ - 1] && targetRowEnd > targetRowStart) targetRowEnd -= 1;
      cursorPos_ = clampToRowColumn(targetRowStart, targetRowEnd, goalCol_);
      relayout();
    } else if (viewportStart_ > 0) {
      viewportStart_ = findPreviousRowStart(viewportStart_);
      relayout();
      uint32_t targetRowEnd = rowStart_[1];
      if (rowEndsWithNewline_[0] && targetRowEnd > rowStart_[0]) targetRowEnd -= 1;
      cursorPos_ = clampToRowColumn(rowStart_[0], targetRowEnd, goalCol_);
      relayout();
    }
  } else {
    if (cursorRow_ >= 0 && cursorRow_ + 1 < rowCount_) {
      const uint32_t targetRowStart = rowStart_[cursorRow_ + 1];
      uint32_t targetRowEnd = rowStart_[cursorRow_ + 2];
      if (rowEndsWithNewline_[cursorRow_ + 1] && targetRowEnd > targetRowStart) targetRowEnd -= 1;
      cursorPos_ = clampToRowColumn(targetRowStart, targetRowEnd, goalCol_);
      relayout();
    } else if (rowCount_ > 1 && cursorPos_ < document_.length()) {
      viewportStart_ = rowStart_[1];
      relayout();
      const int targetRow = static_cast<int>(rowCount_) - 1;
      if (targetRow >= 0) {
        const uint32_t targetRowStart = rowStart_[targetRow];
        uint32_t targetRowEnd = rowStart_[targetRow + 1];
        if (rowEndsWithNewline_[targetRow] && targetRowEnd > targetRowStart) targetRowEnd -= 1;
        cursorPos_ = clampToRowColumn(targetRowStart, targetRowEnd, goalCol_);
        relayout();
      }
    }
  }
}

void EditorActivity::moveCursorVertically(int rows) {
  const int steps = (rows < 0) ? -rows : rows;
  const int dir = (rows < 0) ? -1 : 1;
  for (int i = 0; i < steps; ++i) {
    moveCursorOneRow(dir);
  }
}

void EditorActivity::moveCursorToRowEdge(bool toStart) {
  if (cursorRow_ < 0) return;
  const uint32_t rowBegin = rowStart_[cursorRow_];
  uint32_t rowEnd = rowStart_[cursorRow_ + 1];
  if (rowEndsWithNewline_[cursorRow_] && rowEnd > rowBegin) rowEnd -= 1;
  cursorPos_ = toStart ? rowBegin : rowEnd;
}

// ============================================================================
// Menu / file actions
// ============================================================================

void EditorActivity::showEditorMenu() {
  bleHid_.discardPending();

  enum class Action { Save, SaveAs, Open, New, Exit };
  std::vector<OptionMenuActivity::Option> options = {
      {tr(STR_SAVE), static_cast<int>(Action::Save)}, {tr(STR_SAVE_AS), static_cast<int>(Action::SaveAs)},
      {tr(STR_OPEN), static_cast<int>(Action::Open)}, {tr(STR_NEW_FILE), static_cast<int>(Action::New)},
      {tr(STR_EXIT), static_cast<int>(Action::Exit)},
  };

  auto handler = [this](const ActivityResult& res) {
    bleHid_.discardPending();
    fullRefreshNeeded_ = true;
    frameDirty_ = true;
    if (res.isCancelled) return;
    const auto* menuResult = std::get_if<MenuResult>(&res.data);
    if (!menuResult) return;

    switch (static_cast<Action>(menuResult->action)) {
      case Action::Save:
        doSave();
        break;
      case Action::SaveAs:
        promptSaveAs();
        break;
      case Action::Open:
        promptOpen();
        break;
      case Action::New:
        promptNew();
        break;
      case Action::Exit:
        requestExit();
        break;
    }
  };

  startActivityForResult(
      std::make_unique<OptionMenuActivity>(renderer, mappedInput, tr(STR_EDITOR_MENU), std::move(options)), handler);
}

void EditorActivity::doSave() {
  if (document_.currentPath().empty()) {
    promptSaveAs();
    return;
  }
  document_.flushEditHead();
  if (document_.save(document_.currentPath())) {
    filePath_ = document_.currentPath();
    LOG_INF("EDTR", "Saved: %s", filePath_.c_str());
  } else {
    LOG_ERR("EDTR", "Save failed: %s", document_.currentPath().c_str());
  }
  frameDirty_ = true;
}

void EditorActivity::promptSaveAs() {
  bleHid_.discardPending();

  const std::string initial =
      filePath_.empty() ? (std::string(tr(STR_UNTITLED)) + ".txt") : filePath_.substr(filePath_.find_last_of('/') + 1);

  auto handler = [this](const ActivityResult& res) {
    bleHid_.discardPending();
    fullRefreshNeeded_ = true;
    frameDirty_ = true;
    if (res.isCancelled) return;
    const auto* kb = std::get_if<KeyboardResult>(&res.data);
    if (!kb || kb->text.empty()) return;

    std::string name = kb->text;
    if (!FsHelpers::hasTxtExtension(name) && !FsHelpers::hasMarkdownExtension(name)) {
      name += ".txt";
    }
    std::string dir = filePath_.empty() ? "/" : FsHelpers::extractFolderPath(filePath_);
    if (dir.empty()) dir = "/";
    if (dir.back() != '/') dir += "/";
    const std::string targetPath = dir + name;

    document_.flushEditHead();
    if (document_.save(targetPath)) {
      filePath_ = targetPath;
      LOG_INF("EDTR", "Saved as: %s", filePath_.c_str());
    } else {
      LOG_ERR("EDTR", "Save As failed: %s", targetPath.c_str());
    }
  };

  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ENTER_FILENAME), initial), handler);
}

void EditorActivity::switchToDocument(const std::string& newPath) {
  document_.close();
  if (!document_.open(newPath)) {
    LOG_ERR("EDTR", "Failed to open: %s — starting blank", newPath.c_str());
    document_.open("");
    filePath_.clear();
  } else {
    filePath_ = newPath;
  }
  cursorPos_ = 0;
  viewportStart_ = 0;
  goalCol_ = 0;
  romajiKana_.clear();
  relayout();
  fullRefreshNeeded_ = true;
  frameDirty_ = true;
}

void EditorActivity::confirmDiscardThen(std::function<void()> action) {
  bleHid_.discardPending();
  auto handler = [this, action](const ActivityResult& res) {
    bleHid_.discardPending();
    fullRefreshNeeded_ = true;
    frameDirty_ = true;
    if (!res.isCancelled) action();
  };
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_UNSAVED_CHANGES),
                                                                tr(STR_DISCARD_CHANGES_BODY)),
                         handler);
}

void EditorActivity::promptOpen() {
  bleHid_.discardPending();

  auto handler = [this](const ActivityResult& res) {
    bleHid_.discardPending();
    fullRefreshNeeded_ = true;
    frameDirty_ = true;
    if (res.isCancelled) return;
    const auto* fp = std::get_if<FilePathResult>(&res.data);
    if (!fp) return;

    const std::string newPath = fp->path;
    if (document_.isDirty()) {
      confirmDiscardThen([this, newPath] { switchToDocument(newPath); });
    } else {
      switchToDocument(newPath);
    }
  };

  startActivityForResult(
      std::make_unique<FileBrowserActivity>(renderer, mappedInput, "/", FileBrowserActivity::Mode::PickTextFile),
      handler);
}

void EditorActivity::promptNew() {
  if (document_.isDirty()) {
    confirmDiscardThen([this] { switchToDocument(""); });
  } else {
    switchToDocument("");
  }
}

void EditorActivity::requestExit() {
  if (document_.isDirty()) {
    confirmDiscardThen([this] { finish(); });
  } else {
    finish();
  }
}

// ============================================================================
// Rendering (render task)
// ============================================================================

void EditorActivity::drawStatusRow() {
  const int y = TOP_MARGIN + static_cast<int>(maxRows_) * charH_;

  char left[64];
  const std::string name =
      filePath_.empty() ? std::string(tr(STR_UNTITLED)) : filePath_.substr(filePath_.find_last_of('/') + 1);
  snprintf(left, sizeof(left), "%s%s", document_.isDirty() ? "*" : "", name.c_str());

  const char* modeStr = (inputMode_ == InputMode::Hiragana)   ? "\xe3\x81\x82"  // あ
                        : (inputMode_ == InputMode::Katakana) ? "\xe3\x82\xa2"  // ア
                                                              : "AA";

  const char* bleState;
  if (bleHid_.isSubscribeFailed()) {
    bleState = tr(STR_EDITOR_BLE_PAIR_UNSUP);
  } else if (bleHid_.isConnected()) {
    bleState = tr(STR_EDITOR_BLE_CONNECTED);
  } else if (bleHid_.isScanning()) {
    bleState = tr(STR_EDITOR_BLE_SCANNING);
  } else {
    bleState = tr(STR_EDITOR_BLE_OFF);
  }

  char right[48];
  snprintf(right, sizeof(right), "%s BLE:%s %dKB", modeStr, bleState, ESP.getFreeHeap() / 1024);

  renderer.drawText(EDITOR_FONT_ID, LEFT_MARGIN, y, left, true);

  const int rightW = renderer.getTextWidth(EDITOR_FONT_ID, right);
  const int rightX = displayWidth_ - LEFT_MARGIN - rightW;
  if (rightX > LEFT_MARGIN) {
    renderer.drawText(EDITOR_FONT_ID, rightX, y, right, true);
  }
}

void EditorActivity::render(RenderLock&& lock) {
  renderer.clearScreen(0xFF);

  const bool showCursor = cursorRow_ >= 0 && bleHid_.isConnected();
  char rowBuf[ROW_BUF_SIZE];
  for (uint8_t r = 0; r < rowCount_ && r < maxRows_; ++r) {
    const uint32_t start = rowStart_[r];
    uint32_t end = rowStart_[r + 1];
    if (rowEndsWithNewline_[r] && end > start) end -= 1;
    uint32_t len = end - start;
    if (len >= sizeof(rowBuf)) len = sizeof(rowBuf) - 1;
    const uint32_t got = (len > 0) ? document_.readAt(start, rowBuf, len) : 0;
    rowBuf[got] = '\0';

    const int y = TOP_MARGIN + static_cast<int>(r) * charH_;
    renderer.drawText(EDITOR_FONT_ID, LEFT_MARGIN, y, rowBuf, true);

    if (showCursor && r == cursorRow_) {
      // Position the cursor by measuring the actual rendered prefix with the
      // same advance math drawText uses, rather than col * charW_ — immune to
      // any mismatch between the cell model and real glyph advances.
      const uint32_t off = std::min<uint32_t>(cursorPos_ - start, got);
      const char saved = rowBuf[off];
      rowBuf[off] = '\0';
      const int cx = LEFT_MARGIN + renderer.getTextAdvanceX(EDITOR_FONT_ID, rowBuf, EpdFontFamily::REGULAR);
      rowBuf[off] = saved;

      int cw = charW_;
      if (off < got) {
        uint32_t cpLen;
        decodeUtf8At(rowBuf + off, got - off, cpLen);
        char ch[5] = {};
        memcpy(ch, rowBuf + off, std::min<uint32_t>(cpLen, 4));
        cw = renderer.getTextAdvanceX(EDITOR_FONT_ID, ch, EpdFontFamily::REGULAR);
      }
      renderer.drawRect(cx, y, cw, charH_, 1, true);
    }
  }

  drawStatusRow();

  renderer.displayBuffer(fullRefreshNeeded_ ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  frameDirty_ = false;
  fullRefreshNeeded_ = false;
  lastDisplayUpdate_ = millis();
}
