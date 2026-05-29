#pragma once

#include <Arduino.h>

#include <functional>
#include <string>

// BLE HID host for keyboard input.
// TODO: Implement using NimBLE-Arduino for stable WiFi+BLE coexistence.
// Bluedroid (standard BLE library) crashes when used alongside WiFi on ESP32-C3.
class BleKeyboard {
 public:
  using KeyCallback = std::function<void(const std::string& key)>;

  BleKeyboard() = default;
  ~BleKeyboard() { stop(); }

  void begin(KeyCallback cb);
  void stop();
  void loop();

  bool isConnected() const { return false; }
  bool isScanning() const { return false; }
};
