#pragma once

// In-memory ring log flushed to /xtcdebug.log on every write so a crash
// before reset still leaves a trail on the SD card. Strictly a debug aid
// for diagnosing the XTH "won't open" symptom; remove once resolved.
class XtcDebugLog {
 public:
  static void log(const char* fmt, ...);
  static void reset();
};
