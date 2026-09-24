#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "KanaConverter.h"
#include "SkkDictionary.h"

namespace {

uint32_t readString(void* ctx, uint32_t pos, char* buf, uint32_t n) {
  const auto* s = static_cast<const std::string*>(ctx);
  if (pos >= s->size()) return 0;
  const uint32_t got = std::min<uint32_t>(n, static_cast<uint32_t>(s->size() - pos));
  memcpy(buf, s->data() + pos, got);
  return got;
}

ByteSource sourceFor(const std::string& data) {
  return ByteSource{const_cast<std::string*>(&data), static_cast<uint32_t>(data.size()), readString};
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

// Byte-sorted, as scripts/build_skk_dict.py writes it — note "かk" sorts
// before "かく" (0x6B < 0xE3).
const std::string kSmallDict =
    "あい /愛/藍/\n"
    "かk /書/掛/\n"
    "かく /核/格/書く/\n"
    "かん /感/缶/\n"
    "かんz /感/観/\n"
    "にほん /日本/二本/\n";

}  // namespace

TEST(SkkOkuri, Letters) {
  EXPECT_EQ(skkOkuriLetter(0x304F), 'k');  // く
  EXPECT_EQ(skkOkuriLetter(0x3046), 'u');  // う
  EXPECT_EQ(skkOkuriLetter(0x3058), 'z');  // じ
  EXPECT_EQ(skkOkuriLetter(0x3063), 't');  // っ
  EXPECT_EQ(skkOkuriLetter(0x3093), 'n');  // ん
  EXPECT_EQ(skkOkuriLetter(0x3092), 'w');  // を
  EXPECT_EQ(skkOkuriLetter(0x3071), 'p');  // ぱ
  EXPECT_EQ(skkOkuriLetter(0x308B), 'r');  // る
  EXPECT_EQ(skkOkuriLetter(0x3083), 0);    // ゃ
  EXPECT_EQ(skkOkuriLetter('a'), 0);
}

TEST(Katakana, ConvertsHiraganaOnly) {
  EXPECT_EQ(hiraganaToKatakana("かーど"), "カード");
  EXPECT_EQ(hiraganaToKatakana("にほんabc"), "ニホンabc");
}

TEST(SkkDictionary, FindsEveryLine) {
  const SkkDictionary dict(sourceFor(kSmallDict));
  for (const char* r : {"あい", "かく", "かk", "かん", "かんz", "にほん"}) {
    std::vector<std::string> out;
    EXPECT_TRUE(dict.lookup(r, out)) << r;
    EXPECT_FALSE(out.empty()) << r;
  }
  std::vector<std::string> out;
  ASSERT_TRUE(dict.lookup("にほん", out));
  EXPECT_EQ(out, (std::vector<std::string>{"日本", "二本"}));
}

TEST(SkkDictionary, MissesAbsentReadings) {
  const SkkDictionary dict(sourceFor(kSmallDict));
  for (const char* r : {"あ", "か", "かくし", "ぬ", "ん", "a"}) {
    std::vector<std::string> out;
    EXPECT_FALSE(dict.lookup(r, out)) << r;
    EXPECT_TRUE(out.empty()) << r;
  }
}

TEST(SkkDictionary, EmptySource) {
  const std::string empty;
  const SkkDictionary dict(sourceFor(empty));
  std::vector<std::string> out;
  EXPECT_FALSE(dict.lookup("かく", out));
}

TEST(SkkDictionary, RandomizedAgainstReference) {
  std::mt19937 rng(7);
  std::vector<std::string> keys;
  for (int i = 0; i < 2000; ++i) {
    std::string k;
    const int len = 1 + static_cast<int>(rng() % 4);
    for (int j = 0; j < len; ++j) {
      k += static_cast<char>(0xE3);
      k += static_cast<char>(0x81);
      k += static_cast<char>(0x82 + rng() % 30);  // あ..
    }
    keys.push_back(k);
  }
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  // Keep every other key in the file; the rest must miss.
  std::string data;
  for (size_t i = 0; i < keys.size(); i += 2) data += keys[i] + " /x" + std::to_string(i) + "/\n";
  const SkkDictionary dict(sourceFor(data));
  for (size_t i = 0; i < keys.size(); ++i) {
    std::vector<std::string> out;
    const bool found = dict.lookup(keys[i], out);
    ASSERT_EQ(found, i % 2 == 0) << "key index " << i;
    if (found) ASSERT_EQ(out[0], "x" + std::to_string(i));
  }
}

TEST(Candidates, WholeReadingThenOkuriThenKana) {
  const SkkDictionary dict(sourceFor(kSmallDict));
  const auto c = buildConversionCandidates(&dict, "かく");
  // 書く appears once even though both the whole-reading line and the
  // okuri-ari line produce it.
  EXPECT_EQ(c, (std::vector<std::string>{"核", "格", "書く", "掛く", "カク", "かく"}));
}

TEST(Candidates, MultiKanaOkurigana) {
  const SkkDictionary dict(sourceFor(kSmallDict));
  const auto c = buildConversionCandidates(&dict, "かんじる");
  EXPECT_TRUE(contains(c, "感じる"));
  EXPECT_TRUE(contains(c, "観じる"));
  EXPECT_EQ(c.back(), "かんじる");
}

TEST(Candidates, NoDictionaryGivesKanaOnly) {
  const auto c = buildConversionCandidates(nullptr, "かく");
  EXPECT_EQ(c, (std::vector<std::string>{"カク", "かく"}));
}

TEST(Candidates, UnknownReading) {
  const SkkDictionary dict(sourceFor(kSmallDict));
  const auto c = buildConversionCandidates(&dict, "ぬぬ");
  EXPECT_EQ(c, (std::vector<std::string>{"ヌヌ", "ぬぬ"}));
}

// Optional: SKK_DICT=/path/to/skk.txt ctest — checks the real converted file.
TEST(RealDictionary, CommonWords) {
  const char* path = std::getenv("SKK_DICT");
  if (!path) GTEST_SKIP() << "set SKK_DICT to the build_skk_dict.py output to run";
  std::ifstream f(path, std::ios::binary);
  ASSERT_TRUE(f.good());
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string data = ss.str();
  const SkkDictionary dict(sourceFor(data));

  EXPECT_TRUE(contains(buildConversionCandidates(&dict, "にほん"), "日本"));
  EXPECT_TRUE(contains(buildConversionCandidates(&dict, "かく"), "書く"));
  EXPECT_TRUE(contains(buildConversionCandidates(&dict, "かんじる"), "感じる"));
  EXPECT_TRUE(contains(buildConversionCandidates(&dict, "へんかん"), "変換"));
  EXPECT_TRUE(contains(buildConversionCandidates(&dict, "たべた"), "食べた"));
}
