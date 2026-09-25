#include "EditorActivity.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <string.h>

#include <algorithm>
#include <cctype>
#include <string_view>
#include <variant>
#include <vector>

#include "CrossPointSettings.h"
#include "KanaConverter.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/OptionMenuActivity.h"
#include "components/UITheme.h"

namespace {

// Standard USB HID Keyboard/Keypad usage codes this editor cares about. Deliberately
// limited to the universally-standard Boot Keyboard range (letters, digits, the
// common punctuation keys, and the plain navigation/control keys) — the JIS
// international usages (Henkan/Muhenkan/Katakana toggle, ~0x87-0x92) are left
// unmapped because their exact assignments aren't something to guess at without a
// device to verify against (see CLAUDE.md's anti-hallucination rule). Caps Lock
// (standard usage 0x39, on every keyboard) instead cycles Hiragana -> Katakana ->
// ASCII input mode; the host never sets the Caps Lock LED/state, so letters are
// unaffected. Tab inserts four spaces (the font has no tab glyph).
constexpr uint8_t HID_ENTER = 0x28;
constexpr uint8_t HID_ESCAPE = 0x29;
constexpr uint8_t HID_BACKSPACE = 0x2A;
constexpr uint8_t HID_TAB = 0x2B;
constexpr uint8_t HID_CAPS_LOCK = 0x39;
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

constexpr uint8_t HID_A = 0x04;
constexpr uint8_t HID_B = 0x05;
constexpr uint8_t HID_D = 0x07;
constexpr uint8_t HID_F = 0x09;
constexpr uint8_t HID_S = 0x16;
constexpr uint8_t HID_U = 0x18;
constexpr uint8_t HID_Z_KEY = 0x1D;
constexpr uint8_t HID_R = 0x15;
constexpr uint8_t HID_Y = 0x1C;
constexpr uint8_t HID_V = 0x19;
constexpr uint8_t HID_Z = 0x1D;

constexpr uint8_t MOD_LCTRL = 0x01;
constexpr uint8_t MOD_LSHIFT = 0x02;
constexpr uint8_t MOD_RCTRL = 0x10;
constexpr uint8_t MOD_RSHIFT = 0x20;
bool hasShift(uint8_t mods) { return (mods & (MOD_LSHIFT | MOD_RSHIFT)) != 0; }
bool hasCtrl(uint8_t mods) { return (mods & (MOD_LCTRL | MOD_RCTRL)) != 0; }

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
  const auto* end = std::end(kPunctTable);
  const auto* it = std::find_if(std::begin(kPunctTable), end, [usage](const PunctPair& p) { return p.usage == usage; });
  if (it == end) return false;
  outChar = hasShift(mods) ? it->shifted : it->plain;
  return true;
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

  viEnabled_ = SETTINGS.editorViMode != 0;
  viMode_ = viEnabled_ ? ViMode::Normal : ViMode::Insert;
  resetUndo();
  viYankLen_ = 0;
  if (viEnabled_ && Storage.exists(VI_YANK_PATH)) {
    // The register survives power-off: pick up the last yank from the SD card.
    HalFile yank;
    char type = 0;
    if (Storage.openFileForRead("EDTR", VI_YANK_PATH, yank) && yank.fileSize() > 1 && yank.read(&type, 1) == 1 &&
        (type == YANK_LINES || type == YANK_CHARS || type == YANK_BLOCK)) {
      viYankType_ = type;
      viYankLen_ = static_cast<uint32_t>(yank.fileSize()) - 1;
    }
  }

  if (!document_.open(filePath_)) {
    LOG_ERR("EDTR", "Failed to open document: %s — starting blank", filePath_.c_str());
    document_.open("");
    filePath_.clear();
  }
  cursorPos_ = 0;
  viewportStart_ = 0;
  goalCol_ = 0;
  relayout();

  const bool dictExists = Storage.exists(DICT_PATH);
  if (dictExists) {
    dictFile_ = Storage.open(DICT_PATH, O_READ);
    if (dictFile_) {
      dict_ =
          makeUniqueNoThrow<SkkDictionary>(ByteSource{this, static_cast<uint32_t>(dictFile_.fileSize()), readDictFile});
    }
  }
  LOG_INF("EDTR", "Kanji dictionary %s (%s: exists=%d open=%d size=%u)", dict_ ? "loaded" : "not available", DICT_PATH,
          dictExists, static_cast<bool>(dictFile_), dictFile_ ? static_cast<unsigned>(dictFile_.fileSize()) : 0u);

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
  dict_.reset();
  if (dictFile_) dictFile_.close();
  if (undoFile_) undoFile_.close();
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
  // Keep cols at 80 max.
  hintStripW_ = UITheme::getInstance().getMetrics().buttonHintsHeight;
  maxCols_ = static_cast<uint8_t>(std::min<int>((displayWidth_ - LEFT_MARGIN * 2 - hintStripW_) / charW_, 80));
  textTop_ = TOP_MARGIN + charH_;
  const uint8_t rows = static_cast<uint8_t>(availH / charH_ - 2);  // - candidate row - status row
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
  commitComposition();
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
  if (!text.empty()) insertKana(text);
}

void EditorActivity::insertKana(const std::string& kana) {
  if (kana.empty()) return;
  if (inputMode_ == InputMode::Hiragana && compose_ == ComposeState::None) {
    compStart_ = cursorPos_;
    compose_ = ComposeState::Composing;
  }
  insertText(kana);
}

void EditorActivity::commitComposition() {
  commitPendingKana();
  compose_ = ComposeState::None;
  compReading_.clear();
  candidates_.clear();
  candIndex_ = 0;
}

void EditorActivity::replaceComposition(const std::string& text) {
  editDelete(compStart_, cursorPos_ - compStart_);
  cursorPos_ = compStart_;
  insertText(text);
}

void EditorActivity::revertConversion() {
  replaceComposition(compReading_);
  compose_ = ComposeState::Composing;
  candidates_.clear();
  candIndex_ = 0;
}

void EditorActivity::convertStep(int dir) {
  if (compose_ != ComposeState::Converting) {
    commitPendingKana();  // a trailing "n" becomes ん and joins the composition
    if (compose_ != ComposeState::Composing || cursorPos_ <= compStart_) {
      compose_ = ComposeState::None;
      return;
    }
    const uint32_t len = cursorPos_ - compStart_;
    if (len > MAX_READING_BYTES) return;  // too long to be one word; leave it as typed
    compReading_.assign(len, '\0');
    compReading_.resize(document_.readAt(compStart_, compReading_.data(), len));
    candidates_ = buildConversionCandidates(dict_.get(), compReading_);
    // SKK-JISYO.L lists rare kanji (JIS level 2 and beyond) that the editor
    // font lacks; they would draw as blank gaps, so they are not offered.
    candidates_.erase(
        std::remove_if(candidates_.begin(), candidates_.end(),
                       [this](const std::string& c) { return !renderer.canRenderText(EDITOR_FONT_ID, c.c_str()); }),
        candidates_.end());
    if (candidates_.empty()) return;
    candIndex_ = 0;
    compose_ = ComposeState::Converting;
  } else {
    const size_t n = candidates_.size();
    candIndex_ = dir > 0 ? (candIndex_ + 1) % n : (candIndex_ + n - 1) % n;
  }
  replaceComposition(candidates_[candIndex_]);
}

uint32_t EditorActivity::readDictFile(void* ctx, uint32_t pos, char* buf, uint32_t n) {
  auto* self = static_cast<EditorActivity*>(ctx);
  if (!self->dictFile_.seek(pos)) return 0;
  const int got = self->dictFile_.read(buf, n);
  return got > 0 ? static_cast<uint32_t>(got) : 0;
}

void EditorActivity::insertText(const std::string& text) {
  if (text.empty()) return;
  if (editInsert(cursorPos_, text)) {
    cursorPos_ += static_cast<uint32_t>(text.size());
  } else {
    LOG_ERR("EDTR", "Failed to insert %zu bytes at %u", text.size(), cursorPos_);
  }
}

// ============================================================================
// Undo
// ============================================================================

void EditorActivity::resetUndo() {
  undoCount_ = 0;
  redoCount_ = 0;
  currentGroupStart_ = 0;
  changeOpen_ = false;
  groupDropped_ = false;
  undoFileEnd_ = 0;
  if (undoFile_.isOpen()) return;
  Storage.mkdir("/.crosspoint");  // may run before EditorDocument has created its folders
  Storage.mkdir("/.crosspoint/edit");
  if (!Storage.openFileForWrite("EDTR", UNDO_PATH, undoFile_)) {
    LOG_ERR("EDTR", "Undo disabled: cannot open %s", UNDO_PATH);
  }
}

void EditorActivity::openChange() {
  if (changeOpen_) return;
  changeOpen_ = true;
  groupDropped_ = !undoFile_.isOpen();
  redoCount_ = 0;                         // a new edit forgets what was undone
  if (undoCount_ == 0) undoFileEnd_ = 0;  // nothing references the file any more
  currentGroupStart_ = undoCount_;
}

void EditorActivity::pushUndoOp(const UndoOp& op) {
  if (groupDropped_) return;
  if (undoCount_ == MAX_UNDO_OPS) {
    if (currentGroupStart_ == 0) {
      // This one change fills the whole history: it can't be undone.
      undoCount_ = 0;
      groupDropped_ = true;
      LOG_INF("EDTR", "Change too large to undo");
      return;
    }
    // Forget the oldest change.
    uint16_t next = 1;
    while (next < undoCount_ && !(undoOps_[next].fileOff & UNDO_GROUP_START)) ++next;
    memmove(undoOps_, undoOps_ + next, (undoCount_ - next) * sizeof(UndoOp));
    undoCount_ -= next;
    currentGroupStart_ -= next;
  }
  UndoOp stored = op;
  if (undoCount_ == currentGroupStart_) stored.fileOff |= UNDO_GROUP_START;
  undoOps_[undoCount_++] = stored;
}

bool EditorActivity::copyToUndoFile(uint32_t pos, uint32_t len, uint32_t& outOff) {
  if (undoFileEnd_ + len > UNDO_OFF_MASK || !undoFile_.seekSet(undoFileEnd_)) return false;
  outOff = undoFileEnd_;
  char buf[128];
  for (uint32_t done = 0; done < len;) {
    const uint32_t got = document_.readAt(pos + done, buf, std::min<uint32_t>(sizeof(buf), len - done));
    if (got == 0 || undoFile_.write(buf, got) != got) return false;
    done += got;
  }
  undoFileEnd_ += len;
  return true;
}

bool EditorActivity::insertFromUndoFile(uint32_t pos, uint32_t len, uint32_t fileOff) {
  if (!undoFile_.seekSet(fileOff)) return false;
  char buf[128];
  for (uint32_t done = 0; done < len;) {
    const int got = undoFile_.read(buf, std::min<uint32_t>(sizeof(buf), len - done));
    if (got <= 0) return false;
    if (!document_.insertAt(pos + done, std::string_view(buf, static_cast<size_t>(got)))) return false;
    done += static_cast<uint32_t>(got);
  }
  return true;
}

bool EditorActivity::editInsert(uint32_t pos, std::string_view text) {
  if (!document_.insertAt(pos, text)) return false;
  openChange();
  const auto len = static_cast<uint32_t>(text.size());
  if (!groupDropped_ && undoCount_ > currentGroupStart_) {
    UndoOp& last = undoOps_[undoCount_ - 1];
    if ((last.fileOff & UNDO_INSERT_FLAG) && pos == last.pos + last.len) {
      last.len += len;  // typing: one op for the whole run
      return true;
    }
  }
  pushUndoOp({pos, len, UNDO_INSERT_FLAG});
  return true;
}

void EditorActivity::editDelete(uint32_t pos, uint32_t len) {
  if (len == 0) return;
  openChange();
  bool recorded = groupDropped_;
  if (!recorded && undoCount_ > currentGroupStart_) {
    UndoOp& last = undoOps_[undoCount_ - 1];
    // Deleting the tail of text this change just inserted (Backspace while
    // typing, a conversion replacing its reading) just un-inserts it.
    if ((last.fileOff & UNDO_INSERT_FLAG) && pos >= last.pos && pos + len == last.pos + last.len) {
      last.len -= len;
      if (last.len == 0) --undoCount_;  // the group-start flag goes with it; the next push re-flags
      recorded = true;
    }
  }
  if (!recorded) {
    uint32_t off = 0;
    if (copyToUndoFile(pos, len, off)) {
      pushUndoOp({pos, len, off});
    } else {
      // Can't keep the text: drop the history rather than undo into garbage.
      LOG_ERR("EDTR", "Undo: SD write failed; history cleared");
      undoCount_ = 0;
      currentGroupStart_ = 0;
      groupDropped_ = true;
    }
  }
  document_.deleteAt(pos, len);
}

uint32_t EditorActivity::revertBlock(uint16_t start, uint16_t end) {
  uint32_t minPos = UINT32_MAX;
  for (int i = end - 1; i >= static_cast<int>(start); --i) {
    UndoOp& op = undoOps_[i];
    if (op.fileOff & UNDO_INSERT_FLAG) {
      uint32_t off = 0;
      if (!copyToUndoFile(op.pos, op.len, off)) return UINT32_MAX;
      document_.deleteAt(op.pos, op.len);
      op.fileOff = off;
    } else {
      if (!insertFromUndoFile(op.pos, op.len, op.fileOff & UNDO_OFF_MASK)) return UINT32_MAX;
      op.fileOff = UNDO_INSERT_FLAG;
    }
    minPos = std::min(minPos, op.pos);
  }
  std::reverse(undoOps_ + start, undoOps_ + end);
  undoOps_[start].fileOff |= UNDO_GROUP_START;
  return minPos;
}

void EditorActivity::undoLastChange() {
  commitComposition();
  romajiKana_.clear();
  closeChange();
  if (undoCount_ == 0) {
    LOG_DBG("EDTR", "Nothing to undo");
    return;
  }
  uint16_t start = undoCount_ - 1;
  while (start > 0 && !(undoOps_[start].fileOff & UNDO_GROUP_START)) --start;
  const uint16_t len = undoCount_ - start;
  const uint32_t minPos = revertBlock(start, undoCount_);
  if (minPos == UINT32_MAX) {
    LOG_ERR("EDTR", "Undo failed (SD I/O); history cleared");
    resetUndo();
    return;
  }
  // Move the reverted group onto the redo stack. undo + redo never exceed
  // the array, so the destination fits; the ranges may overlap (memmove).
  const uint16_t dest = MAX_UNDO_OPS - redoCount_ - len;
  memmove(undoOps_ + dest, undoOps_ + start, len * sizeof(UndoOp));
  undoCount_ = start;
  redoCount_ += len;
  cursorPos_ = std::min(minPos, document_.length());
}

void EditorActivity::redoLastChange() {
  commitComposition();
  romajiKana_.clear();
  closeChange();
  if (redoCount_ == 0) {
    LOG_DBG("EDTR", "Nothing to redo");
    return;
  }
  const uint16_t start = MAX_UNDO_OPS - redoCount_;
  uint16_t end = start + 1;
  while (end < MAX_UNDO_OPS && !(undoOps_[end].fileOff & UNDO_GROUP_START)) ++end;
  const uint16_t len = end - start;
  const uint32_t minPos = revertBlock(start, end);
  if (minPos == UINT32_MAX) {
    LOG_ERR("EDTR", "Redo failed (SD I/O); history cleared");
    resetUndo();
    return;
  }
  memmove(undoOps_ + undoCount_, undoOps_ + start, len * sizeof(UndoOp));
  undoCount_ += len;
  redoCount_ -= len;
  cursorPos_ = std::min(minPos, document_.length());
}

void EditorActivity::onBleKey(const HidKeyEvent& ev) {
  bool contentChanged = false;
  bool cursorMoved = false;
  bool isVertical =
      (ev.usage == HID_UP || ev.usage == HID_DOWN || ev.usage == HID_PAGE_UP || ev.usage == HID_PAGE_DOWN);

  // Ctrl shortcuts. Unbound Ctrl+letter combos are swallowed rather than
  // typed as the bare letter.
  if (hasCtrl(ev.mods) && ev.usage >= HID_A && ev.usage <= HID_Z) {
    if (ev.usage == HID_S) {
      commitComposition();  // save what is on screen, not a half-typed reading
      doSave();
    } else if (ev.usage == HID_Z_KEY) {
      undoLastChange();
      afterKey(true, true, false);
    } else if (ev.usage == HID_Y || (ev.usage == HID_R && viEnabled_ && viMode_ == ViMode::Normal)) {
      redoLastChange();
      afterKey(true, true, false);
    } else if (ev.usage == HID_V && viEnabled_ && viMode_ == ViMode::Normal) {
      // Ctrl-V: block (rectangle) selection; again leaves it.
      if (viVisual_ == ViVisual::Block) {
        viVisual_ = ViVisual::None;
      } else {
        if (viVisual_ == ViVisual::None) viAnchor_ = cursorPos_;
        viVisual_ = ViVisual::Block;
      }
      viPending_ = 0;
      viCount_ = 0;
      afterKey(false, true, false);
    } else if (viEnabled_ && viMode_ == ViMode::Normal) {
      // vi scrolling: Ctrl-F/B a screen, Ctrl-D/U half a screen.
      int rows = 0;
      if (ev.usage == HID_F) rows = maxRows_;
      if (ev.usage == HID_B) rows = -static_cast<int>(maxRows_);
      if (ev.usage == HID_D) rows = maxRows_ / 2;
      if (ev.usage == HID_U) rows = -static_cast<int>(maxRows_ / 2);
      if (rows != 0) {
        moveCursorVertically(rows);
        afterKey(false, true, true);
      }
    }
    return;
  }

  if (viEnabled_) {
    if (viMode_ == ViMode::Command) {
      handleViCommandKey(ev);
      return;
    }
    if (viMode_ == ViMode::Normal) {
      if (handleViNormalKey(ev, contentChanged, cursorMoved, isVertical)) {
        afterKey(contentChanged, cursorMoved, isVertical);
        return;
      }
      // Arrows, Home/End, PgUp/PgDn and Delete fall through to the regular handling.
    } else if (ev.usage == HID_ESCAPE && compose_ != ComposeState::Converting) {
      // One Esc leaves Insert: kana typed so far is committed as shown (a
      // pending "n" becomes ん), not discarded. Only while a candidate is
      // shown does Esc first go back to the reading (below).
      enterViNormal();
      afterKey(false, true, false);
      return;
    }
  }

  switch (ev.usage) {
    case HID_CAPS_LOCK:
      cycleInputMode();
      frameDirty_ = true;
      return;

    case HID_TAB:
      commitComposition();
      insertText("    ");
      contentChanged = true;
      break;

    case HID_ENTER: {
      // In a composition, Enter confirms it (like any IME); otherwise newline.
      const bool confirming = compose_ != ComposeState::None || romajiKana_.hasPending();
      commitComposition();
      if (!confirming || inputMode_ != InputMode::Hiragana) {
        insertText("\n");
        closeChange();  // each line is its own undo step
      }
      contentChanged = true;
      break;
    }

    case HID_ESCAPE:
      romajiKana_.clear();
      if (compose_ == ComposeState::Converting) {
        revertConversion();
      } else if (compose_ == ComposeState::Composing) {
        editDelete(compStart_, cursorPos_ - compStart_);
        cursorPos_ = compStart_;
        compose_ = ComposeState::None;
      }
      contentChanged = true;
      break;

    case HID_SPACE:
      if (inputMode_ == InputMode::Hiragana && (compose_ != ComposeState::None || romajiKana_.hasPending())) {
        convertStep(hasShift(ev.mods) ? -1 : 1);
      } else {
        commitComposition();
        insertText(" ");
      }
      contentChanged = true;
      break;

    case HID_BACKSPACE:
      if (compose_ == ComposeState::Converting) {
        revertConversion();
        contentChanged = true;
      } else if (romajiKana_.hasPending()) {
        romajiKana_.clear();
      } else if (cursorPos_ > 0) {
        const uint32_t len = prevCodepointLen(cursorPos_);
        editDelete(cursorPos_ - len, len);
        cursorPos_ -= len;
        if (compose_ == ComposeState::Composing && cursorPos_ <= compStart_) compose_ = ComposeState::None;
        contentChanged = true;
      }
      break;

    case HID_DELETE:
      commitComposition();
      if (cursorPos_ < document_.length()) {
        const uint32_t len = nextCodepointLen(cursorPos_);
        editDelete(cursorPos_, len);
        contentChanged = true;
      }
      break;

    case HID_LEFT:
      commitComposition();
      closeChange();
      if (cursorPos_ > 0) cursorPos_ -= prevCodepointLen(cursorPos_);
      cursorMoved = true;
      break;

    case HID_RIGHT:
      commitComposition();
      closeChange();
      if (cursorPos_ < document_.length()) cursorPos_ += nextCodepointLen(cursorPos_);
      cursorMoved = true;
      break;

    case HID_UP:
      commitComposition();
      closeChange();
      moveCursorVertically(-1);
      cursorMoved = true;
      break;

    case HID_DOWN:
      commitComposition();
      closeChange();
      moveCursorVertically(1);
      cursorMoved = true;
      break;

    case HID_PAGE_UP:
      commitComposition();
      closeChange();
      moveCursorVertically(-static_cast<int>(maxRows_));
      cursorMoved = true;
      break;

    case HID_PAGE_DOWN:
      commitComposition();
      closeChange();
      moveCursorVertically(static_cast<int>(maxRows_));
      cursorMoved = true;
      break;

    case HID_HOME:
      commitComposition();
      closeChange();
      moveCursorToRowEdge(/*toStart=*/true);
      cursorMoved = true;
      break;

    case HID_END:
      commitComposition();
      closeChange();
      moveCursorToRowEdge(/*toStart=*/false);
      cursorMoved = true;
      break;

    default: {
      char ch;
      if (hidUsageToAscii(ev.usage, ev.mods, ch)) {
        const bool romajiEligible =
            inputMode_ != InputMode::Ascii && ((ch >= 'a' && ch <= 'z') || ch == '\'' || ch == '-');
        if (romajiEligible) {
          // Typing while a candidate is shown confirms it and starts a new word.
          if (compose_ == ComposeState::Converting) commitComposition();
          const std::string kana = romajiKana_.feed(ch);
          if (!kana.empty()) {
            insertKana(kana);
            contentChanged = true;
          }
        } else {
          commitComposition();
          insertText(inputMode_ != InputMode::Ascii ? kanaModeSymbol(ch) : std::string(1, ch));
          contentChanged = true;
        }
      }
      break;
    }
  }

  afterKey(contentChanged, cursorMoved, isVertical);
}

void EditorActivity::afterKey(bool contentChanged, bool cursorMoved, bool isVertical) {
  if (contentChanged) {
    lastEditTime_ = millis();
    idleMaintenanceDone_ = false;
  }

  if (contentChanged || cursorMoved) {
    relayout();
    ensureCursorVisible();
    if (!isVertical) goalCol_ = cursorCol_;
    // Once per key, not in relayout(): motions call relayout() once per row moved.
    computeSelection();
    frameDirty_ = true;
  }
}

// ============================================================================
// Vi mode
// ============================================================================

char EditorActivity::byteAt(uint32_t pos) {
  char c = 0;
  if (pos < document_.length()) document_.readAt(pos, &c, 1);
  return c;
}

uint32_t EditorActivity::lineStartOf(uint32_t pos) {
  char buf[64];
  while (pos > 0) {
    const uint32_t n = std::min<uint32_t>(sizeof(buf), pos);
    const uint32_t got = document_.readAt(pos - n, buf, n);
    if (got != n) return 0;
    for (uint32_t i = n; i > 0; --i) {
      if (buf[i - 1] == '\n') return pos - n + i;
    }
    pos -= n;
  }
  return 0;
}

uint32_t EditorActivity::lineEndOf(uint32_t pos) {
  const uint32_t len = document_.length();
  char buf[64];
  while (pos < len) {
    const uint32_t n = std::min<uint32_t>(sizeof(buf), len - pos);
    const uint32_t got = document_.readAt(pos, buf, n);
    if (got == 0) return len;
    const auto* nl = static_cast<const char*>(memchr(buf, '\n', got));
    if (nl) return pos + static_cast<uint32_t>(nl - buf);
    pos += got;
  }
  return len;
}

int EditorActivity::charClassAt(uint32_t pos) {
  char buf[4];
  const uint32_t got = document_.readAt(pos, buf, sizeof(buf));
  if (got == 0) return 0;
  uint32_t cpLen;
  const uint32_t cp = decodeUtf8At(buf, got, cpLen);
  if (cp == ' ' || cp == '\t' || cp == '\n' || cp == 0x3000) return 0;  // blank (incl. ideographic space)
  if (cp < 0x80) return (isalnum(static_cast<int>(cp)) || cp == '_') ? 1 : 2;
  if (cp >= 0x3040 && cp <= 0x309F) return 3;                                      // hiragana
  if ((cp >= 0x30A0 && cp <= 0x30FF) || (cp >= 0xFF66 && cp <= 0xFF9F)) return 4;  // katakana, ー
  if ((cp >= 0x4E00 && cp <= 0x9FFF) || cp == 0x3005) return 5;                    // kanji, 々
  return 6;                                                                        // other (、。「」 etc.)
}

void EditorActivity::viWordForward() {
  const uint32_t len = document_.length();
  const int cls = charClassAt(cursorPos_);
  if (cls != 0) {
    while (cursorPos_ < len && charClassAt(cursorPos_) == cls) cursorPos_ += nextCodepointLen(cursorPos_);
  }
  while (cursorPos_ < len && charClassAt(cursorPos_) == 0) cursorPos_ += nextCodepointLen(cursorPos_);
}

void EditorActivity::viWordBackward() {
  if (cursorPos_ == 0) return;
  cursorPos_ -= prevCodepointLen(cursorPos_);
  while (cursorPos_ > 0 && charClassAt(cursorPos_) == 0) cursorPos_ -= prevCodepointLen(cursorPos_);
  const int cls = charClassAt(cursorPos_);
  while (cursorPos_ > 0) {
    const uint32_t prev = cursorPos_ - prevCodepointLen(cursorPos_);
    if (charClassAt(prev) != cls) break;
    cursorPos_ = prev;
  }
}

void EditorActivity::viWordEnd() {
  const uint32_t len = document_.length();
  if (cursorPos_ >= len) return;
  cursorPos_ += nextCodepointLen(cursorPos_);
  while (cursorPos_ < len && charClassAt(cursorPos_) == 0) cursorPos_ += nextCodepointLen(cursorPos_);
  if (cursorPos_ >= len) return;
  const int cls = charClassAt(cursorPos_);
  while (true) {
    const uint32_t next = cursorPos_ + nextCodepointLen(cursorPos_);
    if (next >= len || charClassAt(next) != cls) break;
    cursorPos_ = next;
  }
}

bool EditorActivity::yankOpen(HalFile& file, char type) {
  viYankLen_ = 0;
  if (!Storage.openFileForWrite("EDTR", VI_YANK_PATH, file) || file.write(&type, 1) != 1) {
    LOG_ERR("EDTR", "Yank: cannot write %s", VI_YANK_PATH);
    return false;
  }
  viYankType_ = type;
  return true;
}

bool EditorActivity::yankAppend(HalFile& file, uint32_t start, uint32_t end, uint32_t& written, char& last) {
  char buf[128];
  for (uint32_t pos = start; pos < end;) {
    const uint32_t got = document_.readAt(pos, buf, std::min<uint32_t>(sizeof(buf), end - pos));
    if (got == 0 || file.write(buf, got) != got) {
      LOG_ERR("EDTR", "Yank: write failed at %u", pos);
      return false;
    }
    last = buf[got - 1];
    written += got;
    pos += got;
  }
  return true;
}

void EditorActivity::viYankLines(uint32_t count) {
  const uint32_t len = document_.length();
  const uint32_t start = lineStartOf(cursorPos_);
  uint32_t end = start;
  for (uint32_t i = 0; i < count && end < len; ++i) {
    end = lineEndOf(end);
    if (end < len) ++end;  // take the '\n'
  }
  HalFile file;
  if (!yankOpen(file, YANK_LINES)) return;
  uint32_t written = 0;
  char last = 0;
  if (!yankAppend(file, start, end, written, last)) return;
  // Linewise text always ends in '\n', so put never has to guess where a
  // line break goes.
  if (written == 0 || last != '\n') {
    if (file.write("\n", 1) != 1) return;
    ++written;
  }
  viYankLen_ = written;
}

void EditorActivity::viYankRange(uint32_t start, uint32_t end) {
  HalFile file;
  if (!yankOpen(file, YANK_CHARS)) return;
  uint32_t written = 0;
  char last = 0;
  if (yankAppend(file, start, end, written, last)) viYankLen_ = written;
}

void EditorActivity::viYankBlock(const BlockRect& r) {
  HalFile file;
  if (!yankOpen(file, YANK_BLOCK)) return;
  uint32_t written = 0;
  char last = 0;
  uint32_t ls = r.firstLine;
  for (uint32_t i = 0; i < r.lines && ls != UINT32_MAX; ++i) {
    uint32_t a, b;
    blockRangeInLine(ls, r.c1, r.c2, a, b);
    if (!yankAppend(file, a, b, written, last) || file.write("\n", 1) != 1) return;
    ++written;
    ls = nextLineStart(ls);
  }
  viYankLen_ = written;
}

uint32_t EditorActivity::insertYankAt(uint32_t at, uint32_t count) {
  HalFile file;
  if (!Storage.openFileForRead("EDTR", VI_YANK_PATH, file) || !file.seekSet(1)) {
    LOG_ERR("EDTR", "Put: cannot open %s", VI_YANK_PATH);
    return 0;
  }
  char buf[128];
  uint32_t inserted = 0;
  while (inserted < count) {
    const int got = file.read(buf, std::min<uint32_t>(sizeof(buf), count - inserted));
    if (got <= 0) break;
    // Chunks may split a UTF-8 sequence; the piece table stores bytes, and the
    // pieces end up adjacent, so the text is whole again once all are in.
    if (!editInsert(at + inserted, std::string_view(buf, static_cast<size_t>(got)))) {
      LOG_ERR("EDTR", "Put: insert failed at %u", at + inserted);
      break;
    }
    inserted += static_cast<uint32_t>(got);
  }
  return inserted;
}

void EditorActivity::viDeleteLines(uint32_t count) {
  viYankLines(count);
  const uint32_t len = document_.length();
  uint32_t start = lineStartOf(cursorPos_);
  uint32_t end = start;
  for (uint32_t i = 0; i < count && end < len; ++i) {
    end = lineEndOf(end);
    if (end < len) ++end;
  }
  // Deleting the last line: take the '\n' before it instead of after.
  if (end == len && start > 0 && (end == start || byteAt(end - 1) != '\n')) --start;
  editDelete(start, end - start);
  cursorPos_ = lineStartOf(std::min(start, document_.length()));
}

void EditorActivity::viPut(bool below) {
  if (viYankLen_ == 0) return;
  if (viYankType_ == YANK_BLOCK) {
    viPutBlock(below);
    return;
  }
  if (viYankType_ == YANK_CHARS) {
    uint32_t at = cursorPos_;
    if (below && at < document_.length() && byteAt(at) != '\n') at += nextCodepointLen(at);
    const uint32_t n = insertYankAt(at, viYankLen_);
    cursorPos_ = at + n;
    if (n > 0) cursorPos_ -= prevCodepointLen(cursorPos_);  // on the last pasted character, as in vi
    return;
  }
  uint32_t at = lineStartOf(cursorPos_);
  if (below) {
    at = lineEndOf(cursorPos_);
    if (at < document_.length()) {
      ++at;
    } else {
      // Last line has no '\n': add one, then the register minus its own.
      if (!editInsert(at, "\n")) return;
      ++at;
      insertYankAt(at, viYankLen_ - 1);
      cursorPos_ = at;
      return;
    }
  }
  insertYankAt(at, viYankLen_);
  cursorPos_ = at;
}

void EditorActivity::viPutBlock(bool after) {
  // Each register row goes into the next line at the same cell column,
  // padding short lines and adding lines past the end as needed.
  uint32_t vcol = vcolOf(cursorPos_);
  if (after && cursorPos_ < document_.length() && byteAt(cursorPos_) != '\n') vcol += cellWidthAt(cursorPos_);
  uint32_t lineStart = lineStartOf(cursorPos_);
  uint32_t at = posAtVcol(lineStart, vcol, /*pad=*/true);
  const uint32_t first = at;

  HalFile file;
  if (!Storage.openFileForRead("EDTR", VI_YANK_PATH, file) || !file.seekSet(1)) {
    LOG_ERR("EDTR", "Put: cannot open %s", VI_YANK_PATH);
    return;
  }
  char buf[128];
  bool pendingNewline = false;  // a row ended; move down before inserting more
  auto nextRow = [&]() {
    uint32_t next = nextLineStart(lineStart);
    if (next == UINT32_MAX) {
      editInsert(document_.length(), "\n");
      next = document_.length();
    }
    lineStart = next;
    at = posAtVcol(lineStart, vcol, /*pad=*/true);
  };
  for (uint32_t done = 0; done < viYankLen_;) {
    const int got = file.read(buf, std::min<uint32_t>(sizeof(buf), viYankLen_ - done));
    if (got <= 0) break;
    done += static_cast<uint32_t>(got);
    int segStart = 0;
    for (int i = 0; i <= got; ++i) {
      if (i < got && buf[i] != '\n') continue;
      if (i > segStart) {
        if (pendingNewline) {
          nextRow();
          pendingNewline = false;
        }
        const auto n = static_cast<uint32_t>(i - segStart);
        if (!editInsert(at, std::string_view(buf + segStart, n))) return;
        at += n;
      }
      if (i < got) {
        if (pendingNewline) nextRow();  // an empty row still takes a line
        pendingNewline = true;
      }
      segStart = i + 1;
    }
  }
  cursorPos_ = first;
}

// ---- Visual mode -----------------------------------------------------------

int EditorActivity::cellWidthAt(uint32_t pos) {
  char buf[4];
  const uint32_t got = document_.readAt(pos, buf, sizeof(buf));
  if (got == 0) return 1;
  uint32_t cpLen;
  const uint32_t cp = decodeUtf8At(buf, got, cpLen);
  return utf8IsCjkBreakable(cp) ? 2 : 1;
}

uint32_t EditorActivity::vcolOf(uint32_t pos) {
  uint32_t p = lineStartOf(pos);
  uint32_t col = 0;
  while (p < pos) {
    col += cellWidthAt(p);
    p += nextCodepointLen(p);
  }
  return col;
}

uint32_t EditorActivity::nextLineStart(uint32_t lineStart) {
  const uint32_t e = lineEndOf(lineStart);
  return e < document_.length() ? e + 1 : UINT32_MAX;
}

uint32_t EditorActivity::posAtVcol(uint32_t lineStart, uint32_t vcol, bool pad) {
  const uint32_t len = document_.length();
  uint32_t pos = lineStart;
  uint32_t col = 0;
  while (pos < len && byteAt(pos) != '\n') {
    if (col >= vcol) return pos;
    const int w = cellWidthAt(pos);
    pos += nextCodepointLen(pos);
    col += w;
    if (col > vcol) return pos;  // a wide char straddles vcol: go after it
  }
  if (col >= vcol) return pos;
  if (!pad) return UINT32_MAX;
  char spaces[32];
  memset(spaces, ' ', sizeof(spaces));
  for (uint32_t need = vcol - col; need > 0;) {
    const uint32_t n = std::min<uint32_t>(need, sizeof(spaces));
    if (!editInsert(pos, std::string_view(spaces, n))) break;
    pos += n;
    need -= n;
  }
  return pos;
}

void EditorActivity::blockRangeInLine(uint32_t lineStart, uint32_t c1, uint32_t c2, uint32_t& a, uint32_t& b) {
  const uint32_t len = document_.length();
  uint32_t pos = lineStart;
  uint32_t col = 0;
  bool found = false;
  a = b = UINT32_MAX;
  while (pos < len && byteAt(pos) != '\n' && col <= c2) {
    const int w = cellWidthAt(pos);
    const uint32_t next = pos + nextCodepointLen(pos);
    if (col + w - 1 >= c1) {  // overlaps [c1, c2]; a half-covered wide char counts whole
      if (!found) a = pos;
      found = true;
      b = next;
    }
    col += w;
    pos = next;
  }
  if (!found) a = b = pos;  // line too short: empty range at its end
}

EditorActivity::BlockRect EditorActivity::blockRect() {
  const uint32_t lo = std::min(viAnchor_, cursorPos_);
  const uint32_t hi = std::max(viAnchor_, cursorPos_);
  const uint32_t va = vcolOf(viAnchor_);
  const uint32_t vc = vcolOf(cursorPos_);
  BlockRect r{};
  r.firstLine = lineStartOf(lo);
  r.c1 = std::min(va, vc);
  r.c2 = std::max(va + cellWidthAt(viAnchor_) - 1, vc + cellWidthAt(cursorPos_) - 1);
  const uint32_t lastLine = lineStartOf(hi);
  r.lines = 1;
  for (uint32_t ls = r.firstLine; ls < lastLine && ls != UINT32_MAX; ls = nextLineStart(ls)) ++r.lines;
  return r;
}

void EditorActivity::computeSelection() {
  for (uint8_t r = 0; r < MAX_GRID_ROWS; ++r) {
    selStart_[r] = selEnd_[r] = 0;
    selNewline_[r] = false;
  }
  if (viVisual_ == ViVisual::None || rowCount_ == 0) return;

  const uint32_t len = document_.length();
  const uint32_t lo = std::min(viAnchor_, cursorPos_);
  const uint32_t hi = std::max(viAnchor_, cursorPos_);
  uint32_t a = 0, b = 0;  // document range for Char/Line
  BlockRect rect{};
  uint32_t lastLine = 0;
  if (viVisual_ == ViVisual::Char) {
    a = lo;
    b = hi < len ? hi + nextCodepointLen(hi) : len;
  } else if (viVisual_ == ViVisual::Line) {
    a = lineStartOf(lo);
    b = lineEndOf(hi);
    if (b < len) ++b;
  } else {
    rect = blockRect();
    lastLine = lineStartOf(hi);
  }

  uint32_t lineStart = lineStartOf(rowStart_[0]);
  uint32_t cachedLine = UINT32_MAX, ba = 0, bb = 0;
  for (uint8_t r = 0; r < rowCount_ && r < maxRows_; ++r) {
    if (r > 0 && rowEndsWithNewline_[r - 1]) lineStart = rowStart_[r];
    const uint32_t rs = rowStart_[r];
    uint32_t re = rowStart_[r + 1];
    const bool hasNl = rowEndsWithNewline_[r] && re > rs;
    if (hasNl) --re;  // text part of the row, without its '\n'
    uint32_t sa, sb;
    if (viVisual_ == ViVisual::Block) {
      if (lineStart < rect.firstLine || lineStart > lastLine) continue;
      if (cachedLine != lineStart) {
        blockRangeInLine(lineStart, rect.c1, rect.c2, ba, bb);
        cachedLine = lineStart;
      }
      sa = ba;
      sb = bb;
    } else {
      sa = a;
      sb = b;
      selNewline_[r] = hasNl && sa <= re && sb > re;
    }
    selStart_[r] = std::max(sa, rs);
    selEnd_[r] = std::min(sb, re);
  }
}

void EditorActivity::viClampToLine() {
  const uint32_t ls = lineStartOf(cursorPos_);
  if ((cursorPos_ >= document_.length() || byteAt(cursorPos_) == '\n') && cursorPos_ > ls) {
    cursorPos_ -= prevCodepointLen(cursorPos_);
  }
}

bool EditorActivity::viVisualOperator(char op) {
  const ViVisual mode = viVisual_;
  viVisual_ = ViVisual::None;
  const uint32_t len = document_.length();
  const uint32_t lo = std::min(viAnchor_, cursorPos_);
  const uint32_t hi = std::max(viAnchor_, cursorPos_);
  const bool remove = op == 'd' || op == 'x' || op == 'c';

  if (mode == ViVisual::Char) {
    if (op == 'I' || op == 'A') return false;
    const uint32_t end = hi < len ? hi + nextCodepointLen(hi) : len;
    viYankRange(lo, end);
    cursorPos_ = lo;
    if (remove) editDelete(lo, end - lo);
    if (op == 'c') {
      viMode_ = ViMode::Insert;
    } else {
      viClampToLine();
    }
    return remove;
  }

  if (mode == ViVisual::Line) {
    if (op == 'I' || op == 'A') return false;
    const uint32_t first = lineStartOf(lo);
    uint32_t lines = 1;
    for (uint32_t ls = first; lineEndOf(ls) < hi; ls = lineEndOf(ls) + 1) ++lines;
    cursorPos_ = first;
    if (op == 'c') {
      // Replace the lines with one empty line and type into it.
      viYankLines(lines);
      uint32_t end = first;
      for (uint32_t i = 0; i < lines; ++i) end = (i + 1 < lines) ? lineEndOf(end) + 1 : lineEndOf(end);
      editDelete(first, end - first);
      cursorPos_ = first;
      viMode_ = ViMode::Insert;
      return true;
    }
    if (remove) {
      viDeleteLines(lines);
    } else {
      viYankLines(lines);
    }
    return remove;
  }

  // Block
  const BlockRect rect = blockRect();
  if (op == 'y' || remove) viYankBlock(rect);
  if (remove) {
    uint32_t ls = rect.firstLine;
    for (uint32_t i = 0; i < rect.lines && ls != UINT32_MAX; ++i) {
      uint32_t a, b;
      blockRangeInLine(ls, rect.c1, rect.c2, a, b);
      editDelete(a, b - a);
      ls = nextLineStart(ls);
    }
  }
  if (op == 'c' || op == 'I' || op == 'A') {
    viBlockInsert_ = {};
    viBlockInsert_.active = true;
    viBlockInsert_.append = (op == 'A');
    viBlockInsert_.vcol = (op == 'A') ? rect.c2 + 1 : rect.c1;
    viBlockInsert_.firstLine = rect.firstLine;
    viBlockInsert_.extraLines = rect.lines - 1;
    const uint32_t at = posAtVcol(rect.firstLine, viBlockInsert_.vcol, /*pad=*/true);
    cursorPos_ = at;
    viBlockInsert_.startPos = at;
    viMode_ = ViMode::Insert;
    return true;
  }
  const uint32_t top = posAtVcol(rect.firstLine, rect.c1, /*pad=*/false);
  cursorPos_ = top == UINT32_MAX ? rect.firstLine : top;
  viClampToLine();
  return remove;
}

void EditorActivity::finishBlockInsert() {
  const BlockInsert bi = viBlockInsert_;
  viBlockInsert_.active = false;
  if (!bi.active || cursorPos_ <= bi.startPos || lineStartOf(cursorPos_) != lineStartOf(bi.startPos)) return;
  const uint32_t textLen = cursorPos_ - bi.startPos;
  // Copy the typed text into each line below. Those lines come after the
  // source, so inserting there never moves it.
  uint32_t ls = nextLineStart(lineStartOf(bi.startPos));
  char buf[128];
  for (uint32_t i = 0; i < bi.extraLines && ls != UINT32_MAX; ++i) {
    const uint32_t at = posAtVcol(ls, bi.vcol, bi.append);
    if (at != UINT32_MAX) {
      for (uint32_t done = 0; done < textLen;) {
        const uint32_t got = document_.readAt(bi.startPos + done, buf, std::min<uint32_t>(sizeof(buf), textLen - done));
        if (got == 0 || !editInsert(at + done, std::string_view(buf, got))) break;
        done += got;
      }
    }
    ls = nextLineStart(ls);
  }
}

void EditorActivity::enterViNormal() {
  commitComposition();
  romajiKana_.clear();
  viMode_ = ViMode::Normal;
  viPending_ = 0;
  viCount_ = 0;
  viEscCount_ = 1;  // the Esc that got us here
  if (viBlockInsert_.active) finishBlockInsert();
  closeChange();  // the Insert session was one change
  // As in vi, leaving Insert steps back onto the last typed character.
  if (cursorPos_ > lineStartOf(cursorPos_)) cursorPos_ -= prevCodepointLen(cursorPos_);
}

bool EditorActivity::handleViNormalKey(const HidKeyEvent& ev, bool& contentChanged, bool& cursorMoved,
                                       bool& isVertical) {
  switch (ev.usage) {
    case HID_PAGE_UP:
    case HID_PAGE_DOWN:
    case HID_DELETE:
      viPending_ = 0;
      viCount_ = 0;
      viEscCount_ = 0;
      return false;
    case HID_ESCAPE:
      viPending_ = 0;
      viCount_ = 0;
      if (viVisual_ != ViVisual::None) {
        viVisual_ = ViVisual::None;
        viEscCount_ = 0;
        cursorMoved = true;
        return true;
      }
      if (++viEscCount_ >= 2) {
        viEscCount_ = 0;
        inputMode_ = InputMode::Ascii;
        romajiKana_.clear();
        cursorMoved = true;  // repaint the mode indicator
      }
      return true;
    default:
      break;
  }
  viEscCount_ = 0;

  // Arrows and Home/End act as their vi motions, so they also follow the
  // block-mode rules (logical lines, kept column) and extend selections.
  char ch = 0;
  if (ev.usage == HID_DOWN) {
    ch = 'j';
  } else if (ev.usage == HID_UP) {
    ch = 'k';
  } else if (ev.usage == HID_LEFT) {
    ch = 'h';
  } else if (ev.usage == HID_RIGHT) {
    ch = 'l';
  } else if (ev.usage == HID_HOME) {
    ch = '0';
  } else if (ev.usage == HID_END) {
    ch = '$';
  }
  const bool fromNavKey = ch != 0;  // never part of a count ("3" then Home is not "30")
  if (!fromNavKey) {
    if (ev.usage == HID_ENTER) {
      ch = '+';
    } else if (ev.usage == HID_SPACE) {
      ch = 'l';
    } else if (ev.usage == HID_BACKSPACE) {
      ch = 'h';
    } else if (!hidUsageToAscii(ev.usage, ev.mods, ch)) {
      return true;  // Caps Lock, Tab etc.: nothing in Normal mode
    }
  }

  if (!fromNavKey && ((ch >= '1' && ch <= '9') || (ch == '0' && viCount_ > 0))) {
    viCount_ = static_cast<uint16_t>(std::min(999, viCount_ * 10 + (ch - '0')));
    return true;
  }
  const char pending = viPending_;
  viPending_ = 0;
  const bool visual = viVisual_ != ViVisual::None;
  if (!pending && (ch == 'g' || (!visual && (ch == 'd' || ch == 'y')))) {
    viPending_ = ch;  // keeps viCount_ for "3dd"
    return true;
  }
  const uint32_t count = viCount_ ? viCount_ : 1;
  viCount_ = 0;
  if (ch != 'j' && ch != 'k') viGoalValid_ = false;
  cursorMoved = true;  // mode/cursor changes all repaint
  closeChange();       // each Normal command (or Insert session it starts) is one change
  if (viVisual_ == ViVisual::Block && (ch == 'i' || ch == 'a')) ch = static_cast<char>(ch - 'a' + 'A');
  if (visual && pending != 'g') {
    switch (ch) {
      case 'd':
      case 'x':
      case 'y':
      case 'c':
      case 'I':
      case 'A':
        contentChanged = viVisualOperator(ch);
        return true;
      case 'o':
        std::swap(viAnchor_, cursorPos_);
        return true;
      case 'v':
        viVisual_ = viVisual_ == ViVisual::Char ? ViVisual::None : ViVisual::Char;
        return true;
      case 'V':
        viVisual_ = viVisual_ == ViVisual::Line ? ViVisual::None : ViVisual::Line;
        return true;
      default:
        if (!strchr("hjkl+0^$wbeG", ch)) return true;  // only motions extend the selection
        break;
    }
  }
  if (!visual && (ch == 'v' || ch == 'V')) {
    viVisual_ = ch == 'v' ? ViVisual::Char : ViVisual::Line;
    viAnchor_ = cursorPos_;
    return true;
  }
  if (ch == 'u') {
    undoLastChange();
    contentChanged = true;
    return true;
  }
  const uint32_t len = document_.length();

  if (pending == 'd') {
    if (ch == 'd') {
      viDeleteLines(count);
      contentChanged = true;
    }
    return true;
  }
  if (pending == 'y') {
    if (ch == 'y') viYankLines(count);
    return true;
  }
  if (pending == 'g') {
    if (ch == 'g') cursorPos_ = 0;
    return true;
  }

  // Keeps the Normal-mode cursor on a character, not on a line's '\n'.
  auto clampToLine = [this]() {
    const uint32_t ls = lineStartOf(cursorPos_);
    if ((cursorPos_ >= document_.length() || byteAt(cursorPos_) == '\n') && cursorPos_ > ls) {
      cursorPos_ -= prevCodepointLen(cursorPos_);
    }
  };
  auto firstNonBlank = [this](uint32_t pos) {
    const uint32_t end = lineEndOf(pos);
    while (pos < end && (byteAt(pos) == ' ' || byteAt(pos) == '\t')) ++pos;
    return pos;
  };

  switch (ch) {
    case 'h':
      for (uint32_t i = 0; i < count && cursorPos_ > lineStartOf(cursorPos_); ++i) {
        cursorPos_ -= prevCodepointLen(cursorPos_);
      }
      break;
    case 'l':
      for (uint32_t i = 0; i < count && cursorPos_ < len && byteAt(cursorPos_) != '\n'; ++i) {
        const uint32_t next = cursorPos_ + nextCodepointLen(cursorPos_);
        if (next >= len || byteAt(next) == '\n') break;
        cursorPos_ = next;
      }
      break;
    case 'j':
    case 'k':
      if (viVisual_ == ViVisual::Block) {
        // By logical line, keeping the cell column: display rows would put
        // a wrapped line's continuation at column 57+ and stretch the block.
        if (!viGoalValid_) viGoalVcol_ = vcolOf(cursorPos_);
        uint32_t ls = lineStartOf(cursorPos_);
        for (uint32_t i = 0; i < count; ++i) {
          const uint32_t next = (ch == 'j') ? nextLineStart(ls) : (ls > 0 ? lineStartOf(ls - 1) : UINT32_MAX);
          if (next == UINT32_MAX) break;
          ls = next;
        }
        const uint32_t at = posAtVcol(ls, viGoalVcol_, /*pad=*/false);
        cursorPos_ = at == UINT32_MAX ? lineEndOf(ls) : at;
        viClampToLine();
        viGoalValid_ = true;
      } else {
        moveCursorVertically(ch == 'j' ? static_cast<int>(count) : -static_cast<int>(count));
      }
      isVertical = true;
      break;
    case '+':  // Enter: first non-blank of the next line
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t e = lineEndOf(cursorPos_);
        if (e >= len) break;
        cursorPos_ = e + 1;
      }
      cursorPos_ = firstNonBlank(lineStartOf(cursorPos_));
      break;
    case '0':
      cursorPos_ = lineStartOf(cursorPos_);
      break;
    case '^':
      cursorPos_ = firstNonBlank(lineStartOf(cursorPos_));
      break;
    case '$':
      cursorPos_ = lineEndOf(cursorPos_);
      clampToLine();
      break;
    case 'w':
      for (uint32_t i = 0; i < count; ++i) viWordForward();
      clampToLine();
      break;
    case 'b':
      for (uint32_t i = 0; i < count; ++i) viWordBackward();
      break;
    case 'e':
      for (uint32_t i = 0; i < count; ++i) viWordEnd();
      break;
    case 'G':
      cursorPos_ = lineStartOf(len);
      break;
    case 'x':
      for (uint32_t i = 0; i < count && cursorPos_ < document_.length() && byteAt(cursorPos_) != '\n'; ++i) {
        editDelete(cursorPos_, nextCodepointLen(cursorPos_));
        contentChanged = true;
      }
      clampToLine();
      break;
    case 'D': {
      const uint32_t e = lineEndOf(cursorPos_);
      if (e > cursorPos_) {
        editDelete(cursorPos_, e - cursorPos_);
        contentChanged = true;
      }
      clampToLine();
      break;
    }
    case 'p':
    case 'P':
      viPut(ch == 'p');
      contentChanged = true;
      break;
    case 'a':
      if (cursorPos_ < len && byteAt(cursorPos_) != '\n') cursorPos_ += nextCodepointLen(cursorPos_);
      viMode_ = ViMode::Insert;
      break;
    case 'i':
      viMode_ = ViMode::Insert;
      break;
    case 'A':
      cursorPos_ = lineEndOf(cursorPos_);
      viMode_ = ViMode::Insert;
      break;
    case 'I':
      cursorPos_ = firstNonBlank(lineStartOf(cursorPos_));
      viMode_ = ViMode::Insert;
      break;
    case 'o':
      cursorPos_ = lineEndOf(cursorPos_);
      insertText("\n");
      contentChanged = true;
      viMode_ = ViMode::Insert;
      break;
    case 'O': {
      const uint32_t ls = lineStartOf(cursorPos_);
      cursorPos_ = ls;
      insertText("\n");
      cursorPos_ = ls;
      contentChanged = true;
      viMode_ = ViMode::Insert;
      break;
    }
    case ':':
      viMode_ = ViMode::Command;
      viCommandLen_ = 0;
      viCommand_[0] = '\0';
      break;
    default:
      break;
  }
  return true;
}

void EditorActivity::handleViCommandKey(const HidKeyEvent& ev) {
  frameDirty_ = true;
  if (ev.usage == HID_ESCAPE) {
    viMode_ = ViMode::Normal;
    return;
  }
  if (ev.usage == HID_ENTER) {
    viMode_ = ViMode::Normal;
    runViCommand();
    return;
  }
  if (ev.usage == HID_BACKSPACE) {
    if (viCommandLen_ == 0) {
      viMode_ = ViMode::Normal;
    } else {
      viCommand_[--viCommandLen_] = '\0';
    }
    return;
  }
  char ch;
  if (!hidUsageToAscii(ev.usage, ev.mods, ch)) return;
  if (viCommandLen_ < sizeof(viCommand_) - 1) {
    viCommand_[viCommandLen_++] = ch;
    viCommand_[viCommandLen_] = '\0';
  }
}

void EditorActivity::runViCommand() {
  // "verb[!] [argument]", e.g. "w", "w memo", "e! /notes/a.md", "wq".
  std::string_view cmd(viCommand_, viCommandLen_);
  while (!cmd.empty() && cmd.front() == ' ') cmd.remove_prefix(1);
  while (!cmd.empty() && cmd.back() == ' ') cmd.remove_suffix(1);
  const size_t space = cmd.find(' ');
  std::string_view verb = cmd.substr(0, space);
  std::string_view arg = space == std::string_view::npos ? std::string_view() : cmd.substr(space + 1);
  while (!arg.empty() && arg.front() == ' ') arg.remove_prefix(1);
  const std::string name(arg);

  bool leaving = false;  // a confirm dialog or exit is pending
  if (verb == "w") {
    if (name.empty()) {
      doSave();
    } else {
      saveAs(resolveNamedPath(name));
    }
  } else if (verb == "wq" || verb == "x") {
    if (name.empty()) {
      doSave();
    } else {
      saveAs(resolveNamedPath(name));
    }
    if (!document_.isDirty()) finish();
    leaving = true;
  } else if (verb == "q") {
    requestExit();  // asks before discarding unsaved changes
    leaving = true;
  } else if (verb == "q!") {
    finish();
    leaving = true;
  } else if (verb == "e" || verb == "e!") {
    if (name.empty()) {
      promptOpen();  // file picker; asks before discarding unsaved changes
    } else {
      openNamed(resolveNamedPath(name), verb == "e!");
    }
    leaving = true;
  } else {
    LOG_DBG("EDTR", "Unknown vi command: %s", viCommand_);
    return;
  }
  // Leaving, dialogs and pickers are deferred by the activity manager; keys
  // still queued behind this Enter must not edit the document meanwhile.
  if (leaving) bleHid_.discardPending();
}

void EditorActivity::loop() {
  // Note: gpio.update() runs once per firmware loop in main.cpp — activities
  // only poll wasReleased/isPressed.
  bleHid_.loop();

  const bool menuPressed = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  const bool backPressed = mappedInput.wasReleased(MappedInputManager::Button::Back);
  if (menuPressed || backPressed) {
    // Anything mid-composition is confirmed as shown before leaving the text.
    commitComposition();
    relayout();
    ensureCursorVisible();
    if (menuPressed) {
      showEditorMenu();
    } else {
      requestExit();
    }
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

  startSubActivity(std::make_unique<OptionMenuActivity>(renderer, mappedInput, tr(STR_EDITOR_MENU), std::move(options)),
                   handler);
}

void EditorActivity::doSave() {
  // An untitled document saves straight away under a timestamp name —
  // typing a filename with the physical buttons is slow; Save As still
  // offers a custom name.
  std::string path = document_.currentPath();
  if (path.empty()) path = filePath_;  // named by ":e newname" but not written yet
  if (path.empty()) {
    Storage.mkdir(AUTO_SAVE_DIR);
    path = makeAutoSavePath();
  }
  document_.flushEditHead();
  if (document_.save(path)) {
    filePath_ = path;
    LOG_INF("EDTR", "Saved: %s", filePath_.c_str());
  } else {
    LOG_ERR("EDTR", "Save failed: %s", path.c_str());
  }
  frameDirty_ = true;
}

std::string EditorActivity::makeAutoSavePath() {
  uint16_t y;
  uint8_t mo, d, h, mi;
  const bool haveDate = halClock.getLocalDateTime(y, mo, d, h, mi, SETTINGS.clockUtcOffsetQ);
  char stamp[20] = {};
  if (haveDate) snprintf(stamp, sizeof(stamp), "%04u%02u%02u-%02u%02u", y, mo, d, h, mi);

  char path[64];
  for (int n = 1; n < 1000; ++n) {
    if (!haveDate) {
      snprintf(path, sizeof(path), "%s/memo-%03d.txt", AUTO_SAVE_DIR, n);  // RTC date never synced
    } else if (n == 1) {
      snprintf(path, sizeof(path), "%s/%s.txt", AUTO_SAVE_DIR, stamp);
    } else {
      snprintf(path, sizeof(path), "%s/%s-%d.txt", AUTO_SAVE_DIR, stamp, n);
    }
    if (!Storage.exists(path)) return path;
  }
  return std::string(AUTO_SAVE_DIR) + "/memo.txt";
}

void EditorActivity::startSubActivity(std::unique_ptr<Activity> activity, ActivityResultHandler handler) {
  // Menus, file pickers and the filename keyboard are driven by the physical
  // buttons, which only line up with the screen in the normal (portrait) UI
  // orientation — show them there, and switch back to the landscape grid
  // when they return.
  {
    RenderLock lock(*this);
    renderer.setOrientation(savedOrientation_);
  }
  startActivityForResult(std::move(activity), [this, handler = std::move(handler)](const ActivityResult& res) {
    {
      RenderLock lock(*this);
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
    }
    fullRefreshNeeded_ = true;
    frameDirty_ = true;
    handler(res);
  });
}

void EditorActivity::promptSaveAs() {
  bleHid_.discardPending();

  const std::string initial = filePath_.empty() ? makeAutoSavePath().substr(strlen(AUTO_SAVE_DIR) + 1)
                                                : filePath_.substr(filePath_.find_last_of('/') + 1);

  auto handler = [this](const ActivityResult& res) {
    bleHid_.discardPending();
    fullRefreshNeeded_ = true;
    frameDirty_ = true;
    if (res.isCancelled) return;
    const auto* kb = std::get_if<KeyboardResult>(&res.data);
    if (!kb || kb->text.empty()) return;

    saveAs(resolveNamedPath(kb->text));
  };

  startSubActivity(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ENTER_FILENAME), initial),
                   handler);
}

std::string EditorActivity::resolveNamedPath(const std::string& typed) const {
  std::string name = typed;
  if (!FsHelpers::hasTxtExtension(name) && !FsHelpers::hasMarkdownExtension(name)) {
    name += ".txt";
  }
  if (name.front() == '/') return name;
  std::string dir = filePath_.empty() ? AUTO_SAVE_DIR : FsHelpers::extractFolderPath(filePath_);
  if (dir.empty()) dir = "/";
  if (dir.back() != '/') dir += "/";
  return dir + name;
}

void EditorActivity::saveAs(const std::string& path) {
  if (path.rfind(AUTO_SAVE_DIR, 0) == 0) Storage.mkdir(AUTO_SAVE_DIR);
  document_.flushEditHead();
  if (document_.save(path)) {
    filePath_ = path;
    LOG_INF("EDTR", "Saved as: %s", filePath_.c_str());
  } else {
    LOG_ERR("EDTR", "Save As failed: %s", path.c_str());
  }
  frameDirty_ = true;
}

void EditorActivity::openNamed(const std::string& path, bool force) {
  auto open = [this, path] {
    if (Storage.exists(path.c_str())) {
      switchToDocument(path);
    } else {
      // As in vi: a new, empty buffer that the next save creates.
      switchToDocument("");
      filePath_ = path;
    }
  };
  if (document_.isDirty() && !force) {
    confirmDiscardThen(open);
  } else {
    open();
  }
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
  resetUndo();
  romajiKana_.clear();
  compose_ = ComposeState::None;
  candidates_.clear();
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
  startSubActivity(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_UNSAVED_CHANGES),
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

  startSubActivity(
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

void EditorActivity::drawCandidateRow() {
  if (compose_ != ComposeState::Converting || candidates_.empty()) return;

  // "[3/12] 各 角 画 ..." — the current candidate first, then as many of the
  // following ones as fit before the button-hint strip.
  const int availW = displayWidth_ - LEFT_MARGIN * 2 - hintStripW_;
  char head[40];
  snprintf(head, sizeof(head), "%s[%u/%u]", dict_ ? "" : tr(STR_DICT_MISSING), static_cast<unsigned>(candIndex_ + 1),
           static_cast<unsigned>(candidates_.size()));
  std::string line(head);
  for (size_t i = 0; i < candidates_.size(); ++i) {
    const std::string next = line + " " + candidates_[(candIndex_ + i) % candidates_.size()];
    if (renderer.getTextAdvanceX(EDITOR_FONT_ID, next.c_str(), EpdFontFamily::REGULAR) > availW) break;
    line = next;
  }
  renderer.drawText(EDITOR_FONT_ID, LEFT_MARGIN, TOP_MARGIN, line.c_str(), true);
  renderer.drawLine(LEFT_MARGIN, textTop_ - 2, LEFT_MARGIN + availW, textTop_ - 2, true);
}

void EditorActivity::drawStatusRow() {
  const int y = textTop_ + static_cast<int>(maxRows_) * charH_;

  char left[96];
  const std::string name =
      filePath_.empty() ? std::string(tr(STR_UNTITLED)) : filePath_.substr(filePath_.find_last_of('/') + 1);
  if (viEnabled_ && viMode_ == ViMode::Command) {
    snprintf(left, sizeof(left), ":%s_", viCommand_);
  } else if (viEnabled_) {
    const char* modeName = tr(STR_EDITOR_VI_INSERT);
    if (viMode_ == ViMode::Normal) {
      modeName = viVisual_ == ViVisual::Char    ? tr(STR_EDITOR_VI_VISUAL)
                 : viVisual_ == ViVisual::Line  ? tr(STR_EDITOR_VI_VISUAL_LINE)
                 : viVisual_ == ViVisual::Block ? tr(STR_EDITOR_VI_VISUAL_BLOCK)
                                                : tr(STR_EDITOR_VI_NORMAL);
    }
    snprintf(left, sizeof(left), "%s %s%s", modeName, document_.isDirty() ? "*" : "", name.c_str());
  } else {
    snprintf(left, sizeof(left), "%s%s", document_.isDirty() ? "*" : "", name.c_str());
  }

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
  const int rightX = displayWidth_ - LEFT_MARGIN - hintStripW_ - rightW;
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

    const int y = textTop_ + static_cast<int>(r) * charH_;
    renderer.drawText(EDITOR_FONT_ID, LEFT_MARGIN, y, rowBuf, true);

    // x offset of byte `off` within this row, measured with the same advance
    // math drawText uses (immune to any mismatch with the cell model).
    auto prefixX = [&](uint32_t off) {
      off = std::min<uint32_t>(off, got);
      const char saved = rowBuf[off];
      rowBuf[off] = '\0';
      const int x = LEFT_MARGIN + renderer.getTextAdvanceX(EDITOR_FONT_ID, rowBuf, EpdFontFamily::REGULAR);
      rowBuf[off] = saved;
      return x;
    };

    if (compose_ != ComposeState::None) {
      // Underline the composition; thicker while a candidate is shown.
      const uint32_t a = std::max(compStart_, start);
      const uint32_t b = std::min(cursorPos_, start + got);
      if (a < b) {
        const int uy = y + charH_ - 3;
        renderer.drawLine(prefixX(a - start), uy, prefixX(b - start), uy, compose_ == ComposeState::Converting ? 3 : 1,
                          true);
      }
    }

    if (viVisual_ != ViVisual::None && selEnd_[r] > selStart_[r] && selStart_[r] >= start) {
      // Selection: black band with the text redrawn in white.
      const uint32_t sa = std::min<uint32_t>(selStart_[r] - start, got);
      const uint32_t sb = std::min<uint32_t>(selEnd_[r] - start, got);
      const int x1 = prefixX(sa);
      const int x2 = prefixX(sb);
      renderer.fillRect(x1, y, x2 - x1, charH_, true);
      const char saved = rowBuf[sb];
      rowBuf[sb] = '\0';
      renderer.drawText(EDITOR_FONT_ID, x1, y, rowBuf + sa, false);
      rowBuf[sb] = saved;
    }
    if (viVisual_ != ViVisual::None && selNewline_[r]) {
      renderer.fillRect(prefixX(got), y, charW_ / 2, charH_, true);  // selected line break
    }

    if (showCursor && r == cursorRow_) {
      const uint32_t off = std::min<uint32_t>(cursorPos_ - start, got);
      const int cx = prefixX(off);
      int cw = charW_;
      if (off < got) {
        uint32_t cpLen;
        decodeUtf8At(rowBuf + off, got - off, cpLen);
        char ch[5] = {};
        memcpy(ch, rowBuf + off, std::min<uint32_t>(cpLen, 4));
        cw = renderer.getTextAdvanceX(EDITOR_FONT_ID, ch, EpdFontFamily::REGULAR);
      }
      if (viEnabled_ && viMode_ != ViMode::Insert) {
        // vi Normal mode: solid block with the character knocked out in white.
        renderer.fillRect(cx, y, cw, charH_, true);
        if (off < got) {
          uint32_t cpLen;
          decodeUtf8At(rowBuf + off, got - off, cpLen);
          char ch[5] = {};
          memcpy(ch, rowBuf + off, std::min<uint32_t>(cpLen, 4));
          renderer.drawText(EDITOR_FONT_ID, cx, y, ch, false);
        }
      } else {
        renderer.drawRect(cx, y, cw, charH_, 1, true);
      }
    }
  }

  drawCandidateRow();
  drawStatusRow();

  // drawButtonHints switches to Portrait internally, so the hints land next to
  // the physical front buttons whatever this activity's orientation is.
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), tr(STR_EDITOR_MENU), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(fullRefreshNeeded_ ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
  frameDirty_ = false;
  fullRefreshNeeded_ = false;
  lastDisplayUpdate_ = millis();
}
