#include "XtcDebugLog.h"

#include <Arduino.h>
#include <HalStorage.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
constexpr size_t MAX_LOG_BYTES = 8192;
constexpr const char* LOG_PATH = "/xtcdebug.txt";
// Fixed-size buffer (no heap usage) — std::string was suspected of fragmenting
// the heap across many appends.
char g_buf[MAX_LOG_BYTES];
size_t g_used = 0;
}  // namespace

void XtcDebugLog::reset() {
  g_used = 0;
  g_buf[0] = '\0';
  HalFile f;
  if (Storage.openFileForWrite("XTCDBG", LOG_PATH, f)) {
    // O_TRUNC truncates; destructor closes.
  }
}

void XtcDebugLog::log(const char* fmt, ...) {
  char body[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);

  char line[320];
  const int lineLen =
      snprintf(line, sizeof(line), "[%lu free=%u] %s\n", millis(), static_cast<unsigned>(ESP.getFreeHeap()), body);
  if (lineLen <= 0) return;
  const size_t addLen = (static_cast<size_t>(lineLen) < sizeof(line)) ? lineLen : sizeof(line) - 1;

  // Append, ring-trimming the oldest content if needed.
  if (g_used + addLen >= sizeof(g_buf)) {
    const size_t drop = (g_used + addLen) - (sizeof(g_buf) - 1);
    if (drop >= g_used) {
      g_used = 0;
    } else {
      memmove(g_buf, g_buf + drop, g_used - drop);
      g_used -= drop;
    }
  }
  memcpy(g_buf + g_used, line, addLen);
  g_used += addLen;
  g_buf[g_used] = '\0';

  HalFile f;
  if (Storage.openFileForWrite("XTCDBG", LOG_PATH, f)) {
    f.write(reinterpret_cast<const uint8_t*>(g_buf), g_used);
  }

  if (Serial) {
    Serial.write(reinterpret_cast<const uint8_t*>(line), addLen);
  }
}
