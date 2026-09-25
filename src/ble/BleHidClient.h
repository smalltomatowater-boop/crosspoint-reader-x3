#pragma once

#include <NimBLEAdvertisedDevice.h>
#include <NimBLEClient.h>
#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdint.h>

#include <string>

// Raw BLE HID keyboard event, delivered on the main task via loop().
// usage = HID Keyboard/Keypad page usage ID (e.g. 0x04 = 'a', 0x28 = Enter,
// 0x87..0x8A = JIS international keys Kana/Yen/Henkan/Muhenkan).
// mods = modifier bitmap (bit0 LCtrl, bit1 LShift, bit2 LAlt, bit3 LGui,
// bit4 RCtrl, bit5 RShift, bit6 RAlt, bit7 RGui).
// Only key *press edges* are reported (keys newly down vs. the previous
// report) — holding a key does not auto-repeat.
struct HidKeyEvent {
  uint8_t usage;
  uint8_t mods;
  uint8_t _pad[2];  // pad to 4 bytes for the FreeRTOS queue
};

// Plain-function callback (no std::function — hot-path rule).
struct HidKeyCallback {
  void* ctx = nullptr;
  void (*fn)(void* ctx, const HidKeyEvent& ev) = nullptr;
};

// Reusable NimBLE HID-over-GATT keyboard client (central role).
// Owns a dedicated FreeRTOS task ("BLEHID", 4KB stack) that performs all
// blocking NimBLE work (init/scan/connect/subscribe); key events cross to
// the consumer via a queue drained by loop() on the main task.
//
// NOTE: the internal task intentionally deviates from the "no background
// tasks in activities" rule in docs/activity-manager.md — it is owned by
// this component (not the activity) and is always joined in stop() before
// the owning activity's onExit() returns (TerminalActivity precedent).
//
// v1 limitation: no pairing/bonding/encryption. Keyboards that require an
// encrypted connection will fail the HID report subscribe; that surfaces
// as isSubscribeFailed() == true rather than a silent retry loop. The v2
// path is NimBLEDevice::setSecurityAuth + NimBLEClient::secureConnection.
class BleHidClient : public NimBLEClientCallbacks, public NimBLEScanCallbacks {
 public:
  BleHidClient() = default;
  ~BleHidClient() { stop(); }

  // Start scanning/connecting. deviceName is the BLE advertised name of
  // this host. Must be called from the main task; cb fires from loop().
  void begin(const char* deviceName, HidKeyCallback cb);

  // Stop and join the BLE task, deinit NimBLE, free the queue.
  // Safe to call even if begin() was never called.
  void stop();

  // Main task: drain queued key events into the callback (non-blocking).
  void loop();

  // Drop all queued key events (e.g. keystrokes typed while a child
  // activity was on top must not land in the document).
  void discardPending();

  bool isConnected() const { return connected_; }
  bool isScanning() const { return scanning_; }
  bool isReady() const { return ready_; }  // subscribed & streaming reports
  bool isSubscribeFailed() const { return subscribeFailed_; }

  // NimBLEClientCallbacks (NimBLE task)
  void onConnect(NimBLEClient* client) override;
  void onDisconnect(NimBLEClient* client, int reason) override;

  // NimBLEScanCallbacks (NimBLE task)
  void onResult(const NimBLEAdvertisedDevice* device) override;
  void onScanEnd(const NimBLEScanResults& results, int reason) override;

 private:
  HidKeyCallback callback_;
  std::string deviceName_;
  NimBLEClient* client_ = nullptr;
  NimBLEAdvertisedDevice* device_ = nullptr;
  QueueHandle_t keyQueue_ = nullptr;
  TaskHandle_t bleTask_ = nullptr;
  volatile bool stopRequested_ = false;
  volatile bool connected_ = false;
  volatile bool scanning_ = false;
  volatile bool connectPending_ = false;
  volatile bool ready_ = false;
  volatile bool subscribeFailed_ = false;
  volatile bool bleRunning_ = false;  // guards getScan() before init/deinit
  volatile bool connecting_ = false;  // inside client_->connect(); stop() cancels it

  // Previous report's keycode slots (accessed only from the NimBLE task).
  uint8_t lastReportKeys_[6] = {};

  bool connectTo(const NimBLEAddress& address);
  // Connects straight to each bonded keyboard in turn (no scan): a bonded
  // keyboard waking from sleep often advertises without the HID service UUID
  // (or only directed at us), so scanning for HID advertisers misses it.
  bool tryBondedReconnect();
  void startScan(uint32_t durationMs);
  void bleTaskRun();
  void enqueueKey(uint8_t usage, uint8_t mods);

  static void bleTaskEntry(void* arg);
  static void notifyCallback(NimBLERemoteCharacteristic* ch, uint8_t* data, size_t len, bool isNotify);

  static BleHidClient* instance_;
};
