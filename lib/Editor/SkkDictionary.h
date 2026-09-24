#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Random-access byte source the dictionary reads through: an SD file on
// device, a std::string in host tests (test/kana_converter).
struct ByteSource {
  void* ctx = nullptr;
  uint32_t size = 0;
  // Reads up to n bytes at pos into buf; returns the number of bytes read.
  uint32_t (*read)(void* ctx, uint32_t pos, char* buf, uint32_t n) = nullptr;
};

// Exact-reading lookup in a dictionary file produced by
// scripts/build_skk_dict.py: one "<reading> /<cand>/<cand>/.../" line per
// reading, sorted by the UTF-8 bytes of <reading>. Lookup binary-searches the
// file in place (~log2(lines) seeks, about 18 for SKK-JISYO.L), so the
// dictionary is never loaded into RAM — the only allocation is one line
// buffer per lookup.
class SkkDictionary {
 public:
  static constexpr uint32_t MAX_LINE = 1024;  // longest line in SKK-JISYO.L output is 970 bytes
  static constexpr size_t MAX_CANDIDATES = 32;

  explicit SkkDictionary(const ByteSource& src) : src_(src) {}

  // Appends the candidates for exactly `reading` to `out`, skipping any
  // already present, up to MAX_CANDIDATES. Returns true if the reading exists.
  bool lookup(std::string_view reading, std::vector<std::string>& out) const;

 private:
  ByteSource src_;

  // First line start >= pos (0, or the byte after a '\n'); src_.size if none.
  uint32_t lineStartAtOrAfter(uint32_t pos) const;
  // Compares the reading of the line starting at lineStart with key
  // (<0, 0, >0 like memcmp over bytes, shorter-prefix-first).
  int compareReadingAt(uint32_t lineStart, std::string_view key) const;
};
