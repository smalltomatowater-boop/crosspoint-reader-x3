#include "BleHidClient.h"

#include <Logging.h>
#include <NimBLERemoteCharacteristic.h>
#include <NimBLERemoteService.h>
#include <string.h>

static const char* HID_SERVICE_UUID = "1812";
static const char* HID_REPORT_UUID = "2a4d";
static constexpr int KEY_QUEUE_LEN = 32;

BleHidClient* BleHidClient::instance_ = nullptr;

// ============================================================================
// Notify → queue  (NimBLE task — no SD/HTTP allowed here)
// ============================================================================

void BleHidClient::enqueueKey(uint8_t usage, uint8_t mods) {
  if (!keyQueue_) return;
  HidKeyEvent ev{usage, mods, {0, 0}};
  xQueueSend(keyQueue_, &ev, 0);  // queue full → drop (overflow policy)
}

void BleHidClient::notifyCallback(NimBLERemoteCharacteristic* ch, uint8_t* data, size_t len, bool isNotify) {
  (void)ch;
  (void)isNotify;
  if (!instance_ || !instance_->ready_ || len < 8) return;

  // Boot keyboard reports are exactly 8 bytes (mods, reserved, keys[6]).
  // Longer notifications carry a leading report-ID byte — re-parse at
  // offset 1. TODO(v2): parse the Report Map instead of guessing.
  const size_t off = (len >= 9) ? 1 : 0;
  const uint8_t mods = data[off];

  // Press-edge detection: only report keys newly down vs. the previous
  // report, so holding a key does not auto-repeat. lastReportKeys_ is
  // touched only from this (NimBLE) task — no lock needed.
  uint8_t prev[6];
  memcpy(prev, instance_->lastReportKeys_, sizeof(prev));
  memcpy(instance_->lastReportKeys_, data + off + 2, sizeof(prev));

  for (int i = 0; i < 6; i++) {
    const uint8_t kc = data[off + 2 + i];
    if (kc == 0x00 || kc == 0x01) continue;  // empty / rollover error
    if (memchr(prev, kc, sizeof(prev))) continue;
    instance_->enqueueKey(kc, mods);
  }
}

// ============================================================================
// NimBLE callbacks (called from NimBLE task)
// ============================================================================

void BleHidClient::onConnect(NimBLEClient* client) {
  (void)client;
  connected_ = true;
  LOG_INF("BLEH", "Keyboard connected");
}

void BleHidClient::onDisconnect(NimBLEClient* client, int reason) {
  (void)client;
  ready_ = false;
  connected_ = false;
  memset(lastReportKeys_, 0, sizeof(lastReportKeys_));
  LOG_INF("BLEH", "Keyboard disconnected (reason %d)", reason);
}

void BleHidClient::onResult(const NimBLEAdvertisedDevice* device) {
  if (!device->haveServiceUUID()) return;
  if (!device->isAdvertisingService(NimBLEUUID(HID_SERVICE_UUID))) return;
  LOG_INF("BLEH", "HID device found: %s", device->getAddress().toString().c_str());
  NimBLEDevice::getScan()->stop();
  delete device_;
  device_ = new (std::nothrow) NimBLEAdvertisedDevice(*device);
  connectPending_ = device_ != nullptr;
}

void BleHidClient::onScanEnd(const NimBLEScanResults& results, int reason) {
  scanning_ = false;
  LOG_DBG("BLEH", "Scan ended (%d devices, reason %d)", results.getCount(), reason);
}

// ============================================================================
// Connection (runs inside BLE task)
// ============================================================================

bool BleHidClient::connectToDevice() {
  if (!device_) return false;
  subscribeFailed_ = false;

  if (client_) {
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
  }

  client_ = NimBLEDevice::createClient(device_->getAddress());
  client_->setClientCallbacks(this, false);

  LOG_INF("BLEH", "Connecting...");
  if (!client_->connect()) {
    LOG_ERR("BLEH", "Connection failed");
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
    return false;
  }

  NimBLERemoteService* svc = client_->getService(NimBLEUUID(HID_SERVICE_UUID));
  if (!svc) {
    LOG_ERR("BLEH", "HID service not found");
    client_->disconnect();
    return false;
  }

  bool subscribed = false;
  for (auto* ch : svc->getCharacteristics(false)) {
    if (ch->getUUID() == NimBLEUUID(HID_REPORT_UUID) && ch->canNotify()) {
      if (ch->subscribe(true, notifyCallback)) {
        subscribed = true;
        LOG_INF("BLEH", "Subscribed to HID report");
      }
    }
  }

  if (!subscribed) {
    // Most likely an encryption-requiring keyboard: v1 has no
    // pairing/bonding, so the subscribe is refused. Surface it to the UI
    // instead of silently retrying forever (see bleTaskRun()'s
    // subscribeFailed_ latch). Because this now stops the retry loop that
    // used to delete the stale client_ at the top of the *next*
    // connectToDevice() call, this is the last chance to free it — NimBLE's
    // per-connection GATT cache is large (measured ~60KB on device) and is
    // not released by disconnect() alone.
    subscribeFailed_ = true;
    LOG_ERR("BLEH", "No notifiable HID report (pairing required?)");
    client_->disconnect();
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
    return false;
  }

  ready_ = true;
  LOG_INF("BLEH", "Keyboard ready");
  return true;
}

void BleHidClient::startScan() {
  scanning_ = true;
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(this);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->setActiveScan(true);
  scan->start(10000);  // 10 s in ms
  LOG_INF("BLEH", "Scanning for HID keyboards...");
}

// ============================================================================
// BLE task — all blocking BLE work runs here, never in main task
// ============================================================================

void BleHidClient::bleTaskEntry(void* arg) {
  static_cast<BleHidClient*>(arg)->bleTaskRun();
  vTaskDelete(nullptr);
}

void BleHidClient::bleTaskRun() {
  NimBLEDevice::init(deviceName_.c_str());
  bleRunning_ = true;
  startScan();

  while (!stopRequested_) {
    if (connectPending_) {
      connectPending_ = false;
      scanning_ = false;
      if (!connectToDevice() && !subscribeFailed_) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!stopRequested_) startScan();
      }
    }
    // subscribeFailed_ latches: once a keyboard has refused the HID report
    // subscribe (most likely because it requires encryption, which v1 has no
    // pairing/bonding for), stop scanning/reconnecting rather than retrying
    // forever against the same keyboard — surfaced to the UI instead
    // (isSubscribeFailed()). A fresh begin() (re-entering the editor) is what
    // resets it, via connectToDevice()'s own `subscribeFailed_ = false`.
    if (!connected_ && !scanning_ && !connectPending_ && !subscribeFailed_) {
      vTaskDelay(pdMS_TO_TICKS(500));
      if (!stopRequested_) startScan();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  // Cleanup
  ready_ = false;
  connected_ = false;
  bleRunning_ = false;
  if (client_) {
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
  }
  delete device_;
  device_ = nullptr;
  NimBLEDevice::deinit(false);
  LOG_INF("BLEH", "BLE task done");
}

// ============================================================================
// Public API (main task)
// ============================================================================

void BleHidClient::begin(const char* deviceName, HidKeyCallback cb) {
  callback_ = cb;
  instance_ = this;
  stopRequested_ = false;
  deviceName_ = deviceName ? deviceName : "CrossPoint";
  keyQueue_ = xQueueCreate(KEY_QUEUE_LEN, sizeof(HidKeyEvent));
  xTaskCreate(bleTaskEntry, "BLEHID", 4096, this, 1, &bleTask_);
}

void BleHidClient::stop() {
  stopRequested_ = true;
  // Signal scan/connect to abort (only valid while NimBLE is initialized —
  // calling getScan() before init/deinit crashes).
  if (bleRunning_ || bleTask_) NimBLEDevice::getScan()->stop();
  if (bleTask_) {
    // Wait up to 3s for the task to exit
    for (int i = 0; i < 60 && bleTask_ && eTaskGetState(bleTask_) != eDeleted; i++) {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (bleTask_ && eTaskGetState(bleTask_) != eDeleted) {
      vTaskDelete(bleTask_);
    }
    bleTask_ = nullptr;
  }
  if (keyQueue_) {
    vQueueDelete(keyQueue_);
    keyQueue_ = nullptr;
  }
  bleRunning_ = false;
  instance_ = nullptr;
}

void BleHidClient::loop() {
  if (!keyQueue_ || !callback_.fn) return;
  HidKeyEvent ev;
  while (xQueueReceive(keyQueue_, &ev, 0) == pdTRUE) {
    callback_.fn(callback_.ctx, ev);
  }
}

void BleHidClient::discardPending() {
  if (keyQueue_) xQueueReset(keyQueue_);
}
