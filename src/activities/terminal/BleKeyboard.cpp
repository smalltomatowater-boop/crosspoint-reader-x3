#include "BleKeyboard.h"

#include <Logging.h>

// TODO: Implement using NimBLE-Arduino library.
// Bluedroid (default ESP32 BLE) crashes with WiFi on ESP32-C3.
// NimBLE uses ~40KB heap vs Bluedroid's ~80KB and coexists with WiFi.

void BleKeyboard::begin(KeyCallback cb) {
  LOG_INF("BLE", "BLE keyboard pending NimBLE implementation");
}
void BleKeyboard::stop() {}
void BleKeyboard::loop() {}
