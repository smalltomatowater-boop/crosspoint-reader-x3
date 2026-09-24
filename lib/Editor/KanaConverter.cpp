#include "KanaConverter.h"

#include <algorithm>

#include "SkkDictionary.h"

namespace {

// Okuri letters for U+3041..U+3093, '_' = none (small kana, ゐ, ゑ).
constexpr char kOkuriLetters[] =
    "_a_i_u_e_o"       // ぁあぃいぅうぇえぉお
    "kgkgkgkgkg"       // かがきぎくぐけげこご
    "szszszszsz"       // さざしじすずせぜそぞ
    "tdtdttdtdtd"      // ただちぢっつづてでとど
    "nnnnn"            // なにぬねの
    "hbphbphbphbphbp"  // はばぱひびぴふぶぷへべぺほぼぽ
    "mmmmm"            // まみむめも
    "_y_y_y"           // ゃやゅゆょよ
    "rrrrr"            // らりるれろ
    "_w__wn";          // ゎわゐゑをん
static_assert(sizeof(kOkuriLetters) - 1 == 0x3093 - 0x3041 + 1, "okuri table must cover U+3041..U+3093");

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Decodes the codepoint at s[i] (BMP only — all this module ever sees) and
// returns its byte length, or 1 for anything malformed.
uint32_t decodeAt(std::string_view s, size_t i, uint32_t& cp) {
  const auto b0 = static_cast<uint8_t>(s[i]);
  if (b0 < 0x80) {
    cp = b0;
    return 1;
  }
  if ((b0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
    cp = ((b0 & 0x1F) << 6) | (static_cast<uint8_t>(s[i + 1]) & 0x3F);
    return 2;
  }
  if ((b0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
    cp = ((b0 & 0x0F) << 12) | ((static_cast<uint8_t>(s[i + 1]) & 0x3F) << 6) | (static_cast<uint8_t>(s[i + 2]) & 0x3F);
    return 3;
  }
  cp = b0;
  return 1;
}

void addUnique(std::vector<std::string>& out, std::string s) {
  if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(std::move(s));
}

}  // namespace

char skkOkuriLetter(uint32_t codepoint) {
  if (codepoint < 0x3041 || codepoint > 0x3093) return 0;
  const char c = kOkuriLetters[codepoint - 0x3041];
  return c == '_' ? 0 : c;
}

std::string hiraganaToKatakana(std::string_view hiragana) {
  std::string out;
  out.reserve(hiragana.size());
  for (size_t i = 0; i < hiragana.size();) {
    uint32_t cp;
    const uint32_t len = decodeAt(hiragana, i, cp);
    if (len == 3 && cp >= 0x3041 && cp <= 0x3096) {
      appendUtf8(out, cp + 0x60);
    } else {
      out.append(hiragana.substr(i, len));
    }
    i += len;
  }
  return out;
}

std::vector<std::string> buildConversionCandidates(const SkkDictionary* dict, std::string_view reading) {
  std::vector<std::string> out;
  if (reading.empty()) return out;

  if (dict) {
    dict->lookup(reading, out);

    // Try the last 1..MAX_OKURI_KANA kana as okurigana. SKK keys an okuri-ari
    // word by its stem plus the first okurigana kana's letter, so 感じる is
    // "かんz" (+ じる) and 書く is "かk" (+ く).
    constexpr int MAX_OKURI_KANA = 3;
    size_t okuriStart = reading.size();
    for (int n = 0; n < MAX_OKURI_KANA && okuriStart > 0; ++n) {
      do {
        --okuriStart;
      } while (okuriStart > 0 && (static_cast<uint8_t>(reading[okuriStart]) & 0xC0) == 0x80);
      if (okuriStart == 0) break;  // no stem left

      uint32_t firstCp;
      decodeAt(reading, okuriStart, firstCp);
      const char letter = skkOkuriLetter(firstCp);
      if (!letter) continue;

      std::string key(reading.substr(0, okuriStart));
      key += letter;
      std::vector<std::string> stems;
      dict->lookup(key, stems);
      const std::string okurigana(reading.substr(okuriStart));
      for (auto& stem : stems) {
        addUnique(out, stem + okurigana);
      }
    }
  }

  addUnique(out, hiraganaToKatakana(reading));
  addUnique(out, std::string(reading));
  return out;
}
