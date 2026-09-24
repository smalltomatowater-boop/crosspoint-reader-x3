#include "HalClock.h"

#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include <cassert>

HalClock halClock;  // Singleton instance

// DS3231 register layout (BCD encoded):
//   0x00: Seconds  (bits 6-4 = tens, bits 3-0 = ones)
//   0x01: Minutes  (bits 6-4 = tens, bits 3-0 = ones)
//   0x02: Hours    (bit 6 = 12/24 mode, bits 5-4 = tens, bits 3-0 = ones)
//   0x03: Day of week (1-7, unused here)
//   0x04: Date     (1-31)
//   0x05: Month    (1-12; bit 7 = century, unused — years are 2000-2099)
//   0x06: Year     (00-99)

static uint8_t bcdToDec(uint8_t bcd) { return ((bcd >> 4) * 10) + (bcd & 0x0F); }
static uint8_t decToBcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

void HalClock::begin() {
  if (!gpio.deviceIsX3()) {
    _available = false;
    return;
  }

  // I2C is already initialised by HalPowerManager::begin() for X3.
  // Probe the DS3231 by reading the seconds register.
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    LOG_INF("CLK", "DS3231 RTC not found");
    _available = false;
    return;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)1);
  if (Wire.available() < 1) {
    _available = false;
    return;
  }
  Wire.read();  // discard — just testing connectivity

  _available = true;
  LOG_INF("CLK", "DS3231 RTC found");

  // Prime the cache with an initial read
  uint8_t h, m;
  getTime(h, m);
}

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  if (!_available) return false;

  const unsigned long now = millis();
  if (_lastPollMs != 0 && (now - _lastPollMs) < CLOCK_POLL_MS) {
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  // Read 3 bytes starting at register 0x00: seconds, minutes, hours
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)3);
  if (Wire.available() < 3) {
    if (!_hasCachedTime) return false;
    _lastPollMs = now;
    hour = _cachedHour;
    minute = _cachedMinute;
    return true;
  }

  Wire.read();  // seconds — not needed
  const uint8_t rawMin = Wire.read();
  const uint8_t rawHour = Wire.read();

  _cachedMinute = bcdToDec(rawMin & 0x7F);
  // Handle 12/24h mode: bit 6 high = 12h mode
  if (rawHour & 0x40) {
    // 12h mode: bit 5 = PM, bits 4-0 = hours (1-12)
    uint8_t h12 = bcdToDec(rawHour & 0x1F);
    bool pm = rawHour & 0x20;
    if (h12 == 12) h12 = 0;
    _cachedHour = pm ? (h12 + 12) : h12;
  } else {
    // 24h mode: bits 5-0 = hours (0-23)
    _cachedHour = bcdToDec(rawHour & 0x3F);
  }
  _lastPollMs = now;
  _hasCachedTime = true;

  hour = _cachedHour;
  minute = _cachedMinute;
  return true;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  if (bufSize < (use12Hour ? 9u : 6u)) return false;
  uint8_t h, m;
  if (!getTime(h, m)) return false;

  // Apply UTC offset: convert biased value to signed quarter-hours.
  // Clamp against corrupted persisted values so display time can't drift outside [-12:00, +14:00].
  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  int offsetQuarterHours = static_cast<int>(utcOffsetQuarterHoursBiased) - 48;
  int totalMinutes = static_cast<int>(h) * 60 + static_cast<int>(m) + offsetQuarterHours * 15;

  // Wrap around 24 hours
  totalMinutes = ((totalMinutes % 1440) + 1440) % 1440;

  const int hour24 = totalMinutes / 60;
  const int min = totalMinutes % 60;
  if (use12Hour) {
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    snprintf(buf, bufSize, "%d:%02d %s", hour12, min, pm ? "PM" : "AM");
  } else {
    snprintf(buf, bufSize, "%02d:%02d", hour24, min);
  }
  return true;
}

bool HalClock::writeTimeToRTC(uint8_t hour, uint8_t minute, uint8_t second) {
  assert(hour < 24);
  assert(minute < 60);
  assert(second < 60);
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);    // Start at register 0x00
  Wire.write(decToBcd(second));  // 0x00: Seconds
  Wire.write(decToBcd(minute));  // 0x01: Minutes
  Wire.write(decToBcd(hour));    // 0x02: Hours (24h mode, bit 6 = 0)
  if (Wire.endTransmission() != 0) {
    LOG_ERR("CLK", "Failed to write time to DS3231");
    return false;
  }

  // Invalidate cache so next read fetches fresh data
  _lastPollMs = 0;
  _cachedHour = hour;
  _cachedMinute = minute;
  _hasCachedTime = true;
  return true;
}

bool HalClock::writeDateToRTC(uint16_t year, uint8_t month, uint8_t day) {
  if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31) return false;
  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(0x04);  // Date register
  Wire.write(decToBcd(day));
  Wire.write(decToBcd(month));
  Wire.write(decToBcd(static_cast<uint8_t>(year - 2000)));
  if (Wire.endTransmission() != 0) {
    LOG_ERR("CLK", "Failed to write date to DS3231");
    return false;
  }
  return true;
}

namespace {
// Days since 1970-01-01 for a proleptic Gregorian date, and back
// (H. Hinnant's days_from_civil / civil_from_days).
int32_t daysFromCivil(int32_t y, uint32_t m, uint32_t d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
  const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

void civilFromDays(int32_t z, int32_t& y, uint32_t& m, uint32_t& d) {
  z += 719468;
  const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const uint32_t doe = static_cast<uint32_t>(z - era * 146097);
  const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint32_t mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int32_t>(yoe) + era * 400 + (m <= 2);
}
}  // namespace

bool HalClock::getLocalDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute,
                                uint8_t utcOffsetQuarterHoursBiased) const {
  if (!_available) return false;

  Wire.beginTransmission(I2C_ADDR_DS3231);
  Wire.write(DS3231_SEC_REG);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(I2C_ADDR_DS3231, (uint8_t)7);
  if (Wire.available() < 7) return false;
  uint8_t r[7];
  for (uint8_t& b : r) b = Wire.read();

  const uint8_t min = bcdToDec(r[1] & 0x7F);
  uint8_t h;
  if (r[2] & 0x40) {
    uint8_t h12 = bcdToDec(r[2] & 0x1F);
    if (h12 == 12) h12 = 0;
    h = (r[2] & 0x20) ? h12 + 12 : h12;
  } else {
    h = bcdToDec(r[2] & 0x3F);
  }
  const uint8_t d = bcdToDec(r[4] & 0x3F);
  const uint8_t mo = bcdToDec(r[5] & 0x1F);
  const uint16_t y = 2000 + bcdToDec(r[6]);
  // Power-on default is 2000-01-01; a date only becomes real after syncFromNTP().
  if (y < 2024 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;

  if (utcOffsetQuarterHoursBiased > 104) utcOffsetQuarterHoursBiased = 104;
  const int32_t offsetMin = (static_cast<int32_t>(utcOffsetQuarterHoursBiased) - 48) * 15;
  const int32_t totalMin = daysFromCivil(y, mo, d) * 1440 + h * 60 + min + offsetMin;
  const int32_t days = totalMin >= 0 ? totalMin / 1440 : (totalMin - 1439) / 1440;
  const int32_t minOfDay = totalMin - days * 1440;

  int32_t ly;
  uint32_t lm, ld;
  civilFromDays(days, ly, lm, ld);
  year = static_cast<uint16_t>(ly);
  month = static_cast<uint8_t>(lm);
  day = static_cast<uint8_t>(ld);
  hour = static_cast<uint8_t>(minOfDay / 60);
  minute = static_cast<uint8_t>(minOfDay % 60);
  return true;
}

bool HalClock::syncFromNTP() {
  if (!_available) return false;

  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("CLK", "WiFi not connected, cannot sync NTP");
    return false;
  }

  LOG_INF("CLK", "Starting NTP sync...");
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");

  // Wait for SNTP sync to complete (up to 5 seconds)
  constexpr int maxAttempts = 50;
  for (int i = 0; i < maxAttempts; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);

      if (writeTimeToRTC(timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec)) {
        // Date too (non-fatal): editor timestamps need it; the status-bar clock doesn't.
        writeDateToRTC(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
        LOG_INF("CLK", "RTC set to %02d:%02d:%02d UTC", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        return true;
      }
      return false;
    }
    delay(100);
  }

  LOG_ERR("CLK", "NTP sync timed out");
  return false;
}
