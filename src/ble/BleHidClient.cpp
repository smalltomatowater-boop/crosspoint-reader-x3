#include "BleHidClient.h"

#include <Arduino.h>
#include <Logging.h>
#include <NimBLERemoteCharacteristic.h>
#include <NimBLERemoteService.h>
#include <string.h>

static const char* HID_SERVICE_UUID = "1812";
static const char* HID_REPORT_UUID = "2a4d";
static constexpr int KEY_QUEUE_LEN = 32;
// Idea from the FreeInk SDK's BleKeyboardHost (MIT): a bounded connect timeout
// (NimBLE's default is 30 s) and an 8 s link supervision timeout, so long
// blocking work on this single core (saving, flattening) can't drop the link.
static constexpr uint32_t CONNECT_TIMEOUT_MS = 8000;
static constexpr uint32_t PAIRING_SCAN_MS = 5000;    // scan slot between bonded attempts
static constexpr uint32_t UNBONDED_SCAN_MS = 10000;  // no bonds yet: just scan

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
  const bool hid = device->haveServiceUUID() && device->isAdvertisingService(NimBLEUUID(HID_SERVICE_UUID));
  if (!hid && !NimBLEDevice::isBonded(device->getAddress())) return;
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

bool BleHidClient::connectTo(const NimBLEAddress& address) {
  subscribeFailed_ = false;

  if (client_) {
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
  }

  client_ = NimBLEDevice::createClient(address);
  if (!client_) {
    LOG_ERR("BLEH", "createClient failed");
    return false;
  }
  client_->setClientCallbacks(this, false);
  client_->setConnectTimeout(CONNECT_TIMEOUT_MS);
  // Units: interval 1.25 ms (15-30 ms), supervision timeout 10 ms (8 s).
  client_->setConnectionParams(12, 24, 0, 800);

  LOG_INF("BLEH", "Connecting to %s...", address.toString().c_str());
  connecting_ = true;
  const bool linked = client_->connect();
  connecting_ = false;
  if (!linked) {
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
  // refresh=true is required: with false, NimBLE returns its cached vector,
  // which is empty until characteristics have been discovered at least once
  // (NimBLERemoteService.cpp:118-124) — the subscribe loop never ran at all.
  for (auto* ch : svc->getCharacteristics(true)) {
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
    // connectTo() call, this is the last chance to free it — NimBLE's
    // per-connection GATT cache is large (measured ~60KB on device) and is
    // not released by disconnect() alone.
    subscribeFailed_ = true;
    LOG_ERR("BLEH", "No notifiable HID report subscribed (%u characteristics, last err %d)",
            static_cast<unsigned>(svc->getCharacteristics(false).size()), client_->getLastError());
    client_->disconnect();
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
    return false;
  }

  ready_ = true;
  LOG_INF("BLEH", "Keyboard ready");
  return true;
}

void BleHidClient::startScan(uint32_t durationMs) {
  scanning_ = true;
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(this);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->setActiveScan(true);
  scan->start(durationMs);
  LOG_INF("BLEH", "Scanning for HID keyboards...");
}

bool BleHidClient::tryBondedReconnect() {
  // One bond per call, round-robin, newest first (NimBLE appends new bonds to
  // the end of its store). Each failed attempt costs a full
  // CONNECT_TIMEOUT_MS, so trying every bond back to back would leave only a
  // small slice of each cycle for the scan that finds a keyboard in pairing
  // mode — keyboards that take a fresh address on every re-pair leave dead
  // bonds behind.
  const int bonds = NimBLEDevice::getNumBonds();
  if (bonds <= 0) return false;
  const int index = bonds - 1 - static_cast<int>(bondCursor_ % static_cast<uint32_t>(bonds));
  ++bondCursor_;
  const NimBLEAddress addr = NimBLEDevice::getBondedAddress(index);
  if (addr.isNull()) return false;
  if (connectTo(addr)) {
    bondCursor_ = 0;
    return true;
  }
  if (subscribeFailed_) {
    // The link came up but the encrypted subscribe failed: the keyboard has
    // most likely dropped our keys (re-paired elsewhere). Forget the stale
    // bond so it can be paired afresh from pairing mode, instead of latching.
    LOG_INF("BLEH", "Dropping stale bond %s", addr.toString().c_str());
    NimBLEDevice::deleteBond(addr);
    subscribeFailed_ = false;
  }
  return false;
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
  // HOGP requires an encrypted link for HID reports. NimBLE pairs on demand
  // when a subscribe hits an insufficient-encryption error
  // (NimBLERemoteValueAttribute.cpp:76-79); bonding keeps the keys in NVS
  // (CONFIG_BT_NIMBLE_NVS_PERSIST) so later reconnects skip pairing.
  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/false, /*sc=*/true);
  bleRunning_ = true;

  while (!stopRequested_) {
    if (connectPending_) {
      connectPending_ = false;
      scanning_ = false;
      if (device_) connectTo(device_->getAddress());
    }
    // subscribeFailed_ latches: once a keyboard has refused the HID report
    // subscribe (most likely because it requires encryption, which v1 has no
    // pairing/bonding for), stop scanning/reconnecting rather than retrying
    // forever against the same keyboard — surfaced to the UI instead
    // (isSubscribeFailed()). A fresh begin() (re-entering the editor) is what
    // resets it, via connectTo()'s own `subscribeFailed_ = false`.
    if (!connected_ && !scanning_ && !connectPending_ && !subscribeFailed_) {
      // Bonded keyboards first (direct connect, no pairing mode needed), then
      // a short scan so a new keyboard in pairing mode can still be found.
      const bool haveBonds = NimBLEDevice::getNumBonds() > 0;
      if (haveBonds && tryBondedReconnect()) continue;
      vTaskDelay(pdMS_TO_TICKS(200));
      if (!stopRequested_ && !subscribeFailed_ && !connected_) {
        startScan(haveBonds ? PAIRING_SCAN_MS : UNBONDED_SCAN_MS);
      }
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
  // clearAll=true: with false NimBLE keeps its scan object (and every
  // advertised device the scan found) on the heap after deinit. Left in the
  // middle of the heap, that split the largest free block from ~114KB to
  // ~51KB, so the XTC reader's 52KB page buffer failed after using the editor.
  NimBLEDevice::deinit(true);
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
  // A direct connect blocks inside client_->connect() for up to
  // CONNECT_TIMEOUT_MS; cancel it rather than deleting the task under NimBLE
  // (which leaves the host/controller inconsistent). client_ is not deleted
  // while connecting_ is set.
  if (connecting_ && client_) client_->cancelConnect();
  if (bleTask_) {
    // Wait up to 10s for the task to exit (covers a connect that ignores the cancel)
    for (int i = 0; i < 200 && bleTask_ && eTaskGetState(bleTask_) != eDeleted; i++) {
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
    LOG_DBG("BLEH", "Stopped: free %u, largest block %u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
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
