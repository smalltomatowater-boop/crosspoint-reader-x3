#include "BleKeyboard.h"

#include <Logging.h>

// ============================================================================
// HID keycode → tmux send-keys string
// (US layout; JIS-specific usages are not mapped — the terminal only needs
//  ASCII and control keys for tmux.)
// ============================================================================

const char* BleKeyboard::hidKeyToTmux(uint8_t modifier, uint8_t keycode) {
  const bool shift = (modifier & 0x22) != 0;
  const bool ctrl = (modifier & 0x11) != 0;
  const bool alt = (modifier & 0x44) != 0;
  (void)alt;

  if (ctrl && keycode >= 0x04 && keycode <= 0x1D) {
    static char buf[8];
    snprintf(buf, sizeof(buf), "C-%c", 'a' + (keycode - 0x04));
    return buf;
  }

  if (keycode >= 0x3A && keycode <= 0x45) {
    static const char* fkeys[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
    return fkeys[keycode - 0x3A];
  }

  switch (keycode) {
    case 0x28:
      return "Enter";
    case 0x29:
      return "Escape";
    case 0x2A:
      return "BSpace";
    case 0x2B:
      return "Tab";
    case 0x4F:
      return "Right";
    case 0x50:
      return "Left";
    case 0x51:
      return "Down";
    case 0x52:
      return "Up";
    case 0x4A:
      return "Home";
    case 0x4D:
      return "End";
    case 0x4B:
      return "PgUp";
    case 0x4E:
      return "PgDn";
    case 0x4C:
      return "DC";
    case 0x49:
      return "IC";
    default:
      break;
  }

  static char ch[2] = {0, 0};

  if (keycode >= 0x04 && keycode <= 0x1D) {
    ch[0] = shift ? ('A' + keycode - 0x04) : ('a' + keycode - 0x04);
    return ch;
  }

  static const char digits[] = "1234567890";
  static const char dshift[] = "!@#$%^&*()";
  if (keycode >= 0x1E && keycode <= 0x27) {
    ch[0] = shift ? dshift[keycode - 0x1E] : digits[keycode - 0x1E];
    return ch;
  }

  static const char punc_n[] = " -=[]\\#;'`,./";
  static const char punc_s[] = " _+{}|~:\"~<>?";
  if (keycode >= 0x2C && keycode <= 0x38) {
    ch[0] = shift ? punc_s[keycode - 0x2C] : punc_n[keycode - 0x2C];
    return ch;
  }

  return nullptr;
}

// ============================================================================
// BleHidClient adapter
// ============================================================================

void BleKeyboard::onHidEvent(void* ctx, const HidKeyEvent& ev) {
  auto* self = static_cast<BleKeyboard*>(ctx);
  if (!self || !self->callback_) return;
  const char* key = hidKeyToTmux(ev.mods, ev.usage);
  if (key) self->callback_(std::string(key));
}

void BleKeyboard::begin(KeyCallback cb) {
  callback_ = std::move(cb);
  hid_.begin("X3Terminal", {this, onHidEvent});
}

void BleKeyboard::stop() { hid_.stop(); }

void BleKeyboard::loop() { hid_.loop(); }
