#pragma once

#include <Arduino.h>
#include <NimBLEAdvertisedDevice.h>
#include <NimBLEClient.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <functional>
#include <string>

class BleKeyboard : public NimBLEClientCallbacks, public NimBLEScanCallbacks {
 public:
  using KeyCallback = std::function<void(const std::string& key)>;

  BleKeyboard() = default;
  ~BleKeyboard() { stop(); }

  void begin(KeyCallback cb);
  void stop();
  void loop();  // main task: dispatch queued keys only (non-blocking)

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
  QueueHandle_t keyQueue_ = nullptr;
  TaskHandle_t bleTask_ = nullptr;
  volatile bool stopRequested_ = false;
  volatile bool connected_ = false;
  volatile bool scanning_ = false;
  volatile bool connectPending_ = false;
  volatile bool ready_ = false;

  bool connectToDevice();
  void startScan();
  void bleTaskRun();  // BLE task body
  void enqueueKey(const char* key);

  static void bleTaskEntry(void* arg);
  static void notifyCallback(NimBLERemoteCharacteristic* ch, uint8_t* data, size_t len, bool isNotify);
  static const char* hidKeyToTmux(uint8_t modifier, uint8_t keycode);

  static BleKeyboard* instance_;
};
