#include "SkkDictionary.h"

#include <Memory.h>

#include <algorithm>
#include <cstring>

uint32_t SkkDictionary::lineStartAtOrAfter(uint32_t pos) const {
  if (pos == 0) return 0;
  char chunk[64];
  uint32_t p = pos - 1;  // a line starts at pos iff byte pos-1 is '\n'
  while (p < src_.size) {
    const uint32_t got = src_.read(src_.ctx, p, chunk, sizeof(chunk));
    if (got == 0) break;
    const void* nl = memchr(chunk, '\n', got);
    if (nl) return p + static_cast<uint32_t>(static_cast<const char*>(nl) - chunk) + 1;
    p += got;
  }
  return src_.size;
}

int SkkDictionary::compareReadingAt(uint32_t lineStart, std::string_view key) const {
  char buf[128];  // readings are short; longer ones compare on their first 128 bytes
  const uint32_t got = src_.read(src_.ctx, lineStart, buf, sizeof(buf));
  uint32_t len = 0;
  while (len < got && buf[len] != ' ' && buf[len] != '\n') ++len;

  const size_t n = std::min<size_t>(len, key.size());
  const int c = memcmp(buf, key.data(), n);
  if (c != 0) return c;
  if (len == key.size()) return 0;
  return len < key.size() ? -1 : 1;
}

bool SkkDictionary::lookup(std::string_view reading, std::vector<std::string>& out) const {
  if (!src_.read || src_.size == 0 || reading.empty()) return false;

  // Invariant: if the line exists, its start is in [lo, hi); lo is always a line start.
  uint32_t lo = 0;
  uint32_t hi = src_.size;
  uint32_t found = UINT32_MAX;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const uint32_t ls = lineStartAtOrAfter(mid);
    if (ls >= hi) {
      hi = mid;  // no line starts in [mid, hi)
      continue;
    }
    const int c = compareReadingAt(ls, reading);
    if (c == 0) {
      found = ls;
      break;
    }
    if (c < 0) {
      lo = lineStartAtOrAfter(ls + 1);
    } else {
      hi = ls;
    }
  }
  if (found == UINT32_MAX) return false;

  auto line = makeUniqueNoThrow<char[]>(MAX_LINE);
  if (!line) return false;
  const uint32_t got = src_.read(src_.ctx, found, line.get(), MAX_LINE);
  uint32_t end = 0;
  while (end < got && line[end] != '\n') ++end;

  // Skip "<reading> /", then split on '/'.
  uint32_t p = static_cast<uint32_t>(reading.size());
  while (p < end && line[p] != '/') ++p;
  while (p < end && out.size() < MAX_CANDIDATES) {
    const uint32_t start = p + 1;
    uint32_t q = start;
    while (q < end && line[q] != '/') ++q;
    if (q >= end) break;  // unterminated (line truncated at MAX_LINE): drop the partial candidate
    if (q > start) {
      std::string cand(line.get() + start, q - start);
      if (std::find(out.begin(), out.end(), cand) == out.end()) out.push_back(std::move(cand));
    }
    p = q;
  }
  return true;
}
