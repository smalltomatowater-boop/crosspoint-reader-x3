#pragma once

#include <Arduino.h>
#include <NimBLEAdvertisedDevice.h>
#include <NimBLEClient.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>

#include <functional>
#include <string>

class BleKeyboard : public NimBLEClientCallbacks, public NimBLEScanCallbacks {
 public:
  using KeyCallback = std::function<void(const std::string& key)>;

  BleKeyboard() = default;
  ~BleKeyboard() { stop(); }

  void begin(KeyCallback cb);
  void stop();
  void loop();

  bool isConnected() const { return connected_; }
  bool isScanning() const { return scanning_; }

  // NimBLEClientCallbacks
  void onConnect(NimBLEClient* client) override;
  void onDisconnect(NimBLEClient* client, int reason) override;

  // NimBLEScanCallbacks
  void onResult(const NimBLEAdvertisedDevice* device) override;
  void onScanEnd(const NimBLEScanResults& results, int reason) override;

 private:
  KeyCallback callback_;
  NimBLEClient* client_ = nullptr;
  NimBLEAdvertisedDevice* device_ = nullptr;
  bool connected_ = false;
  bool scanning_ = false;
  bool connectPending_ = false;

  bool connectToDevice();
  void startScan();
  void handleReport(const uint8_t* data, size_t len);

  static void notifyCallback(NimBLERemoteCharacteristic* ch, uint8_t* data, size_t len, bool isNotify);
  static const char* hidKeyToTmux(uint8_t modifier, uint8_t keycode);

  static BleKeyboard* instance_;
};
