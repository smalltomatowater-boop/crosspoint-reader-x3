#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class SkkDictionary;

// Conversion candidates for a hiragana reading, in display order:
//   1. dictionary words for the whole reading ("かく" -> 核, 格, 各, ...)
//   2. okuri-ari words: the last 1-3 kana treated as okurigana (shortest
//      first), looked up as stem + the SKK okuri letter of the first
//      okurigana kana, with the okurigana re-appended ("かく" -> かk -> 書く;
//      "かんじる" -> かんz -> 感じる)
//   3. the reading in katakana, then the reading itself
// Duplicates are dropped. `dict` may be null (no dictionary on the SD card),
// in which case only (3) is returned. Pure logic, host-tested
// (test/kana_converter).
std::vector<std::string> buildConversionCandidates(const SkkDictionary* dict, std::string_view reading);

// SKK okuri letter for a hiragana codepoint ('k' for く, 'u' for う, 'z' for
// じ, 't' for っ, ...), or 0 for small kana and non-hiragana.
char skkOkuriLetter(uint32_t codepoint);

// Hiragana (U+3041-3096) to katakana; other codepoints are copied as-is.
std::string hiraganaToKatakana(std::string_view hiragana);
