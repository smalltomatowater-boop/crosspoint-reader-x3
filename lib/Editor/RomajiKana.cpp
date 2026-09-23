#include "RomajiKana.h"

#include <cstddef>
#include <cstring>

namespace {

struct RomajiRule {
  const char* romaji;
  const char* hiragana;
  const char* katakana;
};

// Flash-resident rule table (~150 entries): standard gojuon, dakuten/handakuten
// rows, youon (palatalized) forms, small-form (x-prefix) kana, common loanword
// extensions (fa/va/she/je/tsa-family), and the long-vowel mark. Deliberately
// omits the rarer th-/tw-/dh-/dw-prefixed foreign-sound families (te/to/de/do
// small-vowel combos) to keep the v1 table small and unambiguous; "ti"/"tu"
// resolve to chi/tsu here, matching the most common convention.
static constexpr RomajiRule kRules[] = {
    // Vowels
    {"a", "あ", "ア"},
    {"i", "い", "イ"},
    {"u", "う", "ウ"},
    {"e", "え", "エ"},
    {"o", "お", "オ"},

    // K
    {"ka", "か", "カ"},
    {"ki", "き", "キ"},
    {"ku", "く", "ク"},
    {"ke", "け", "ケ"},
    {"ko", "こ", "コ"},
    {"kya", "きゃ", "キャ"},
    {"kyu", "きゅ", "キュ"},
    {"kyo", "きょ", "キョ"},

    // S
    {"sa", "さ", "サ"},
    {"shi", "し", "シ"},
    {"si", "し", "シ"},
    {"su", "す", "ス"},
    {"se", "せ", "セ"},
    {"so", "そ", "ソ"},
    {"sha", "しゃ", "シャ"},
    {"sya", "しゃ", "シャ"},
    {"shu", "しゅ", "シュ"},
    {"syu", "しゅ", "シュ"},
    {"sho", "しょ", "ショ"},
    {"syo", "しょ", "ショ"},
    {"she", "しぇ", "シェ"},

    // T
    {"ta", "た", "タ"},
    {"chi", "ち", "チ"},
    {"ti", "ち", "チ"},
    {"tsu", "つ", "ツ"},
    {"tu", "つ", "ツ"},
    {"te", "て", "テ"},
    {"to", "と", "ト"},
    {"cha", "ちゃ", "チャ"},
    {"tya", "ちゃ", "チャ"},
    {"chu", "ちゅ", "チュ"},
    {"tyu", "ちゅ", "チュ"},
    {"cho", "ちょ", "チョ"},
    {"tyo", "ちょ", "チョ"},
    {"che", "ちぇ", "チェ"},
    {"tsa", "つぁ", "ツァ"},
    {"tsi", "つぃ", "ツィ"},
    {"tse", "つぇ", "ツェ"},
    {"tso", "つぉ", "ツォ"},

    // N
    {"na", "な", "ナ"},
    {"ni", "に", "ニ"},
    {"nu", "ぬ", "ヌ"},
    {"ne", "ね", "ネ"},
    {"no", "の", "ノ"},
    {"nya", "にゃ", "ニャ"},
    {"nyu", "にゅ", "ニュ"},
    {"nyo", "にょ", "ニョ"},
    {"nn", "ん", "ン"},
    {"n'", "ん", "ン"},

    // H / F
    {"ha", "は", "ハ"},
    {"hi", "ひ", "ヒ"},
    {"fu", "ふ", "フ"},
    {"hu", "ふ", "フ"},
    {"he", "へ", "ヘ"},
    {"ho", "ほ", "ホ"},
    {"hya", "ひゃ", "ヒャ"},
    {"hyu", "ひゅ", "ヒュ"},
    {"hyo", "ひょ", "ヒョ"},
    {"fa", "ふぁ", "ファ"},
    {"fi", "ふぃ", "フィ"},
    {"fe", "ふぇ", "フェ"},
    {"fo", "ふぉ", "フォ"},
    {"fyu", "ふゅ", "フュ"},

    // M
    {"ma", "ま", "マ"},
    {"mi", "み", "ミ"},
    {"mu", "む", "ム"},
    {"me", "め", "メ"},
    {"mo", "も", "モ"},
    {"mya", "みゃ", "ミャ"},
    {"myu", "みゅ", "ミュ"},
    {"myo", "みょ", "ミョ"},

    // Y
    {"ya", "や", "ヤ"},
    {"yu", "ゆ", "ユ"},
    {"yo", "よ", "ヨ"},

    // R
    {"ra", "ら", "ラ"},
    {"ri", "り", "リ"},
    {"ru", "る", "ル"},
    {"re", "れ", "レ"},
    {"ro", "ろ", "ロ"},
    {"rya", "りゃ", "リャ"},
    {"ryu", "りゅ", "リュ"},
    {"ryo", "りょ", "リョ"},

    // W
    {"wa", "わ", "ワ"},
    {"wi", "うぃ", "ウィ"},
    {"we", "うぇ", "ウェ"},
    {"wo", "を", "ヲ"},

    // G
    {"ga", "が", "ガ"},
    {"gi", "ぎ", "ギ"},
    {"gu", "ぐ", "グ"},
    {"ge", "げ", "ゲ"},
    {"go", "ご", "ゴ"},
    {"gya", "ぎゃ", "ギャ"},
    {"gyu", "ぎゅ", "ギュ"},
    {"gyo", "ぎょ", "ギョ"},

    // Z / J
    {"za", "ざ", "ザ"},
    {"ji", "じ", "ジ"},
    {"zi", "じ", "ジ"},
    {"zu", "ず", "ズ"},
    {"ze", "ぜ", "ゼ"},
    {"zo", "ぞ", "ゾ"},
    {"ja", "じゃ", "ジャ"},
    {"zya", "じゃ", "ジャ"},
    {"ju", "じゅ", "ジュ"},
    {"zyu", "じゅ", "ジュ"},
    {"jo", "じょ", "ジョ"},
    {"zyo", "じょ", "ジョ"},
    {"je", "じぇ", "ジェ"},

    // D
    {"da", "だ", "ダ"},
    {"di", "ぢ", "ヂ"},
    {"du", "づ", "ヅ"},
    {"de", "で", "デ"},
    {"do", "ど", "ド"},

    // B
    {"ba", "ば", "バ"},
    {"bi", "び", "ビ"},
    {"bu", "ぶ", "ブ"},
    {"be", "べ", "ベ"},
    {"bo", "ぼ", "ボ"},
    {"bya", "びゃ", "ビャ"},
    {"byu", "びゅ", "ビュ"},
    {"byo", "びょ", "ビョ"},

    // P
    {"pa", "ぱ", "パ"},
    {"pi", "ぴ", "ピ"},
    {"pu", "ぷ", "プ"},
    {"pe", "ぺ", "ペ"},
    {"po", "ぽ", "ポ"},
    {"pya", "ぴゃ", "ピャ"},
    {"pyu", "ぴゅ", "ピュ"},
    {"pyo", "ぴょ", "ピョ"},

    // V
    {"va", "ゔぁ", "ヴァ"},
    {"vi", "ゔぃ", "ヴィ"},
    {"vu", "ゔ", "ヴ"},
    {"ve", "ゔぇ", "ヴェ"},
    {"vo", "ゔぉ", "ヴォ"},

    // Small forms (x-prefix)
    {"xa", "ぁ", "ァ"},
    {"xi", "ぃ", "ィ"},
    {"xu", "ぅ", "ゥ"},
    {"xe", "ぇ", "ェ"},
    {"xo", "ぉ", "ォ"},
    {"xya", "ゃ", "ャ"},
    {"xyu", "ゅ", "ュ"},
    {"xyo", "ょ", "ョ"},
    {"xtsu", "っ", "ッ"},
    {"xtu", "っ", "ッ"},
    {"xwa", "ゎ", "ヮ"},

    // Long vowel mark
    {"-", "ー", "ー"},
};
static constexpr size_t kRuleCount = sizeof(kRules) / sizeof(kRules[0]);
static_assert(kRuleCount > 100, "romaji rule table looks truncated");

const RomajiRule* matchExact(const std::string& buf) {
  for (const RomajiRule& rule : kRules) {
    if (buf == rule.romaji) return &rule;
  }
  return nullptr;
}

bool isValidPrefix(const std::string& buf) {
  for (const RomajiRule& rule : kRules) {
    const size_t ruleLen = strlen(rule.romaji);
    if (ruleLen >= buf.size() && memcmp(rule.romaji, buf.data(), buf.size()) == 0) return true;
  }
  return false;
}

bool isVowelLetter(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }

// Consonant-doubling candidate for sokuon detection. Excludes 'n' — the
// bare-n-before-consonant case is handled separately since "nn" is an
// explicit table rule, not a doubled-consonant sokuon.
bool isConsonantLetter(char c) { return c >= 'a' && c <= 'z' && !isVowelLetter(c) && c != 'n'; }

}  // namespace

std::string RomajiKana::feed(char c) {
  pending_.push_back(c);
  std::string out;

  while (!pending_.empty()) {
    if (const RomajiRule* rule = matchExact(pending_)) {
      out += (mode_ == Mode::Hiragana) ? rule->hiragana : rule->katakana;
      pending_.clear();
      break;
    }
    if (isValidPrefix(pending_)) {
      break;  // Wait for more input.
    }
    if (pending_.size() >= 2 && pending_[0] == pending_[1] && isConsonantLetter(pending_[0])) {
      out += (mode_ == Mode::Hiragana) ? "っ" : "ッ";
      pending_.erase(0, 1);
      continue;
    }
    if (pending_[0] == 'n' && pending_.size() >= 2) {
      out += (mode_ == Mode::Hiragana) ? "ん" : "ン";
      pending_.erase(0, 1);
      continue;
    }
    // Leading character matches no rule at all — pass it through literally.
    out.push_back(pending_[0]);
    pending_.erase(0, 1);
  }

  return out;
}

std::string RomajiKana::flushPending() {
  std::string out;
  if (pending_ == "n") {
    out = (mode_ == Mode::Hiragana) ? "ん" : "ン";
  } else {
    out = pending_;
  }
  pending_.clear();
  return out;
}
