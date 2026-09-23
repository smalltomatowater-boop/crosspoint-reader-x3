#pragma once

#include <string>

// Converts romaji keystrokes to kana (hiragana/katakana) with a classic
// IME-style FSM: buffer romaji until it matches a complete mora. Sokuon
// (doubled consonant -> small tsu) and the "n" ambiguity (na/ni/... vs a
// bare ん before a consonant) fall out of the generic buffer/prefix logic
// below rather than a hardcoded doubled-letter list, so extending the rule
// table never requires touching feed().
//
// Pure C++, no Arduino/ESP-IDF dependency — host-testable (test/romaji_kana).
// Kanji conversion (v2, SKK-style) consumes the kana this class emits; it
// sits above this class and does not require changes here.
class RomajiKana {
 public:
  enum class Mode { Hiragana, Katakana };

  explicit RomajiKana(Mode mode = Mode::Hiragana) : mode_(mode) {}

  void setMode(Mode mode) { mode_ = mode; }
  Mode mode() const { return mode_; }

  // Feed one lowercase ASCII romaji character ('a'-'z', '\'', '-').
  // Returns UTF-8 kana text to commit, or an empty string if the input is
  // still a pending, incomplete mora prefix (e.g. "k", "sh"). A single call
  // can emit more than one kana (e.g. "tt" completes a sokuon and starts
  // the next mora). Non-romaji characters (digits, punctuation, Enter,
  // Space) should not be passed here — the caller inserts them directly.
  std::string feed(char c);

  // Abandon the pending prefix: cursor move, Enter, Space, Backspace, or
  // losing BLE focus. A lone pending "n" resolves to ん/ン, matching real
  // IME behavior at word/sentence boundaries (e.g. "hon" at end of input).
  // Any other partial prefix never completed a mora, so it flushes as the
  // literal ASCII the user typed.
  std::string flushPending();

  bool hasPending() const { return !pending_.empty(); }
  void clear() { pending_.clear(); }

 private:
  Mode mode_;
  std::string pending_;
};
