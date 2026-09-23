#pragma once

#include <Arduino.h>

#include <functional>
#include <string>

#include "ble/BleHidClient.h"

// Terminal-specific adapter over BleHidClient: translates raw HID key
// events into tmux send-keys key names for the mac-bridge HTTP endpoint.
// Public API is unchanged from the original standalone implementation, so
// TerminalActivity needs no changes.
class BleKeyboard {
 public:
  using KeyCallback = std::function<void(const std::string& key)>;

  BleKeyboard() = default;
  ~BleKeyboard() { stop(); }

  void begin(KeyCallback cb);
  void stop();
  void loop();  // main task: dispatch queued keys only (non-blocking)

  bool isConnected() const { return hid_.isConnected(); }
  bool isScanning() const { return hid_.isScanning(); }

 private:
  BleHidClient hid_;
  KeyCallback callback_;

  static void onHidEvent(void* ctx, const HidKeyEvent& ev);
  static const char* hidKeyToTmux(uint8_t modifier, uint8_t keycode);
};
