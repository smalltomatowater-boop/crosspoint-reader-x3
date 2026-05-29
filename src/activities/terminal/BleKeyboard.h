#pragma once

#include <Arduino.h>

#include <functional>
#include <string>

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
