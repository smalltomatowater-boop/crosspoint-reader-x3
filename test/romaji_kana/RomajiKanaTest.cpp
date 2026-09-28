#include <gtest/gtest.h>

#include "RomajiKana.h"

namespace {

// Feeds a full romaji string and returns the concatenated committed kana
// (pending state left in the converter, matching how EditorActivity would
// call feed() once per keystroke).
std::string feedAll(RomajiKana& rk, const std::string& romaji) {
  std::string out;
  for (char c : romaji) out += rk.feed(c);
  return out;
}

}  // namespace

TEST(RomajiKana, PlainVowelsAndGojuon) {
  RomajiKana rk;
  EXPECT_EQ(feedAll(rk, "aiueo"), "あいうえお");
  EXPECT_EQ(feedAll(rk, "kakikukeko"), "かきくけこ");
}

TEST(RomajiKana, PendingPrefixEmitsNothingUntilComplete) {
  RomajiKana rk;
  EXPECT_EQ(rk.feed('k'), "");
  EXPECT_TRUE(rk.hasPending());
  EXPECT_EQ(rk.feed('a'), "か");
  EXPECT_FALSE(rk.hasPending());
}

TEST(RomajiKana, YouonPalatalizedForms) {
  RomajiKana rk;
  EXPECT_EQ(feedAll(rk, "kya"), "きゃ");
  EXPECT_EQ(feedAll(rk, "shu"), "しゅ");
  EXPECT_EQ(feedAll(rk, "cho"), "ちょ");
  EXPECT_EQ(feedAll(rk, "ja"), "じゃ");
}

TEST(RomajiKana, DeadEndPrefixFlushesLeadingCharLiterally) {
  RomajiKana rk;
  // "v" is a valid prefix (va/vi/vu/ve/vo) but "vy" is not a prefix of
  // anything -> "v" flushes literally, "y" carries over as the new pending
  // prefix, then completes "ya" -> や. (Was "jya" until jya became a rule.)
  EXPECT_EQ(feedAll(rk, "vya"), "vや");
}

TEST(RomajiKana, AlternateSpellingsConverge) {
  RomajiKana rk;
  EXPECT_EQ(feedAll(rk, "shi"), "し");
  EXPECT_EQ(feedAll(rk, "si"), "し");
  EXPECT_EQ(feedAll(rk, "chi"), "ち");
  EXPECT_EQ(feedAll(rk, "ti"), "ち");
  EXPECT_EQ(feedAll(rk, "tsu"), "つ");
  EXPECT_EQ(feedAll(rk, "tu"), "つ");
  EXPECT_EQ(feedAll(rk, "fu"), "ふ");
  EXPECT_EQ(feedAll(rk, "hu"), "ふ");
}

TEST(RomajiKana, SokuonDoubledConsonant) {
  RomajiKana rk;
  // "matte" -> ま,っ,て
  EXPECT_EQ(feedAll(rk, "matte"), "まって");
  // "kitte" -> き,っ,て
  EXPECT_EQ(feedAll(rk, "kitte"), "きって");
  // "issho" -> い,っ,しょ
  EXPECT_EQ(feedAll(rk, "issho"), "いっしょ");
}

TEST(RomajiKana, BareNBeforeConsonantCommitsAsN) {
  RomajiKana rk;
  // "kanb..." -> か, then "n" immediately resolves to ん once "b" (a
  // consonant that can't extend "n") arrives, leaving "b" itself pending.
  std::string out = feedAll(rk, "kanb");
  EXPECT_EQ(out, "かん");
  EXPECT_TRUE(rk.hasPending());
  EXPECT_EQ(rk.feed('a'), "ば");
}

TEST(RomajiKana, DoubleNCommitsAsN) {
  RomajiKana rk;
  EXPECT_EQ(feedAll(rk, "konnnichi"), "こんにち");
}

TEST(RomajiKana, ExplicitNApostropheCommitsAsN) {
  RomajiKana rk;
  EXPECT_EQ(feedAll(rk, "kon'ya"), "こんや");
}

TEST(RomajiKana, FlushPendingLoneNResolvesToN) {
  RomajiKana rk;
  EXPECT_EQ(rk.feed('h'), "");
  EXPECT_EQ(rk.feed('o'), "ほ");
  EXPECT_EQ(rk.feed('n'), "");
  EXPECT_TRUE(rk.hasPending());
  EXPECT_EQ(rk.flushPending(), "ん");
  EXPECT_FALSE(rk.hasPending());
}

TEST(RomajiKana, FlushPendingIncompletePrefixIsLiteral) {
  RomajiKana rk;
  EXPECT_EQ(rk.feed('s'), "");
  EXPECT_EQ(rk.feed('h'), "");  // "sh" is a valid pending prefix (sha/shi/shu/she/sho)
  EXPECT_TRUE(rk.hasPending());
  EXPECT_EQ(rk.flushPending(), "sh");
}

TEST(RomajiKana, UnrecognizedLeadingCharFlushesLiterally) {
  RomajiKana rk;
  // Every letter now starts some rule (q became kwa-style), so use a
  // character that starts none.
  EXPECT_EQ(rk.feed('@'), "@");
  EXPECT_FALSE(rk.hasPending());
}

TEST(RomajiKana, KatakanaMode) {
  RomajiKana rk(RomajiKana::Mode::Katakana);
  EXPECT_EQ(feedAll(rk, "kohi-"), "コヒー");
  EXPECT_EQ(feedAll(rk, "fa"), "ファ");
}

TEST(RomajiKana, SmallFormsAndLongVowel) {
  RomajiKana rk;
  EXPECT_EQ(feedAll(rk, "xtsu"), "っ");
  EXPECT_EQ(feedAll(rk, "-"), "ー");
}

TEST(RomajiKana, ClearResetsPendingState) {
  RomajiKana rk;
  rk.feed('k');
  EXPECT_TRUE(rk.hasPending());
  rk.clear();
  EXPECT_FALSE(rk.hasPending());
  // "k" was discarded, not carried over — 'a' alone completes the vowel
  // mora, not "ka".
  EXPECT_EQ(rk.feed('a'), "あ");
}

TEST(RomajiKana, MsImeForeignSoundSpellings) {
  RomajiKana rk;
  EXPECT_EQ(feedAll(rk, "thi"), "てぃ");
  EXPECT_EQ(feedAll(rk, "dhi"), "でぃ");
  EXPECT_EQ(feedAll(rk, "dhu"), "でゅ");
  EXPECT_EQ(feedAll(rk, "twu"), "とぅ");
  EXPECT_EQ(feedAll(rk, "dwu"), "どぅ");
  EXPECT_EQ(feedAll(rk, "whi"), "うぃ");
  EXPECT_EQ(feedAll(rk, "kwa"), "くぁ");
  EXPECT_EQ(feedAll(rk, "qo"), "くぉ");
  EXPECT_EQ(feedAll(rk, "ye"), "いぇ");
  EXPECT_EQ(feedAll(rk, "jya"), "じゃ");
  EXPECT_EQ(feedAll(rk, "paathii"), "ぱあてぃい");
}

TEST(RomajiKana, LPrefixSmallKana) {
  RomajiKana rk;
  EXPECT_EQ(feedAll(rk, "li"), "ぃ");
  EXPECT_EQ(feedAll(rk, "la"), "ぁ");
  EXPECT_EQ(feedAll(rk, "lyu"), "ゅ");
  EXPECT_EQ(feedAll(rk, "ltu"), "っ");
  EXPECT_EQ(feedAll(rk, "ltsu"), "っ");
  EXPECT_EQ(feedAll(rk, "lka"), "ゕ");
  EXPECT_EQ(feedAll(rk, "teli"), "てぃ");
}

TEST(RomajiKana, NewPrefixesDoNotBreakSokuonOrExisting) {
  RomajiKana rk;
  EXPECT_EQ(feedAll(rk, "motto"), "もっと");
  EXPECT_EQ(feedAll(rk, "matthi"), "まってぃ");
  EXPECT_EQ(feedAll(rk, "wa"), "わ");
  EXPECT_EQ(feedAll(rk, "wi"), "うぃ");
  EXPECT_EQ(feedAll(rk, "chi"), "ち");
  EXPECT_EQ(feedAll(rk, "tsu"), "つ");
}

TEST(RomajiKana, KatakanaForNewSpellings) {
  RomajiKana rk;
  rk.setMode(RomajiKana::Mode::Katakana);
  EXPECT_EQ(feedAll(rk, "thi"), "ティ");
  EXPECT_EQ(feedAll(rk, "li"), "ィ");
  EXPECT_EQ(feedAll(rk, "twu"), "トゥ");
}
