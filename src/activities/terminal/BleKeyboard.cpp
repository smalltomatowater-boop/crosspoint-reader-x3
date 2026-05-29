#include "BleKeyboard.h"

#include <Logging.h>

void BleKeyboard::begin(KeyCallback cb) { LOG_INF("BLE", "BLE keyboard disabled (WiFi coexistence pending)"); }
void BleKeyboard::stop() {}
void BleKeyboard::loop() {}
