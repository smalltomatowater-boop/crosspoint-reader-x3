#include "BleKeyboard.h"

#include <Logging.h>
#include <NimBLERemoteCharacteristic.h>
#include <NimBLERemoteService.h>

static const char* HID_SERVICE_UUID = "1812";
static const char* HID_REPORT_UUID = "2a4d";
static constexpr int KEY_STR_MAX = 12;
static constexpr int KEY_QUEUE_LEN = 16;

BleKeyboard* BleKeyboard::instance_ = nullptr;

// ============================================================================
// HID keycode → tmux send-keys string
// ============================================================================

const char* BleKeyboard::hidKeyToTmux(uint8_t modifier, uint8_t keycode) {
  const bool shift = (modifier & 0x22) != 0;
  const bool ctrl = (modifier & 0x11) != 0;
  const bool alt = (modifier & 0x44) != 0;
  (void)alt;

  if (ctrl && keycode >= 0x04 && keycode <= 0x1D) {
    static char buf[8];
    snprintf(buf, sizeof(buf), "C-%c", 'a' + (keycode - 0x04));
    return buf;
  }

  if (keycode >= 0x3A && keycode <= 0x45) {
    static const char* fkeys[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
    return fkeys[keycode - 0x3A];
  }

  switch (keycode) {
    case 0x28:
      return "Enter";
    case 0x29:
      return "Escape";
    case 0x2A:
      return "BSpace";
    case 0x2B:
      return "Tab";
    case 0x4F:
      return "Right";
    case 0x50:
      return "Left";
    case 0x51:
      return "Down";
    case 0x52:
      return "Up";
    case 0x4A:
      return "Home";
    case 0x4D:
      return "End";
    case 0x4B:
      return "PgUp";
    case 0x4E:
      return "PgDn";
    case 0x4C:
      return "DC";
    case 0x49:
      return "IC";
    default:
      break;
  }

  static char ch[2] = {0, 0};

  if (keycode >= 0x04 && keycode <= 0x1D) {
    ch[0] = shift ? ('A' + keycode - 0x04) : ('a' + keycode - 0x04);
    return ch;
  }

  static const char digits[] = "1234567890";
  static const char dshift[] = "!@#$%^&*()";
  if (keycode >= 0x1E && keycode <= 0x27) {
    ch[0] = shift ? dshift[keycode - 0x1E] : digits[keycode - 0x1E];
    return ch;
  }

  static const char punc_n[] = " -=[]\\#;'`,./";
  static const char punc_s[] = " _+{}|~:\"~<>?";
  if (keycode >= 0x2C && keycode <= 0x38) {
    ch[0] = shift ? punc_s[keycode - 0x2C] : punc_n[keycode - 0x2C];
    return ch;
  }

  return nullptr;
}

// ============================================================================
// Notify → queue  (NimBLE task — no HTTP allowed here)
// ============================================================================

void BleKeyboard::enqueueKey(const char* key) {
  if (!keyQueue_ || !key) return;
  char buf[KEY_STR_MAX] = {};
  strncpy(buf, key, KEY_STR_MAX - 1);
  xQueueSend(keyQueue_, buf, 0);
}

void BleKeyboard::notifyCallback(NimBLERemoteCharacteristic* ch, uint8_t* data, size_t len, bool isNotify) {
  if (!instance_ || !instance_->ready_ || len < 8) return;
  const uint8_t modifier = data[0];
  for (int i = 2; i < 8; i++) {
    const uint8_t kc = data[i];
    if (kc == 0x00 || kc == 0x01) continue;
    const char* key = hidKeyToTmux(modifier, kc);
    if (key) instance_->enqueueKey(key);
  }
}

// ============================================================================
// NimBLE callbacks (called from NimBLE task)
// ============================================================================

void BleKeyboard::onConnect(NimBLEClient* client) {
  connected_ = true;
  LOG_INF("BLE", "Keyboard connected");
}

void BleKeyboard::onDisconnect(NimBLEClient* client, int reason) {
  ready_ = false;
  connected_ = false;
  LOG_INF("BLE", "Keyboard disconnected (reason %d)", reason);
}

void BleKeyboard::onResult(const NimBLEAdvertisedDevice* device) {
  if (!device->haveServiceUUID()) return;
  if (!device->isAdvertisingService(NimBLEUUID(HID_SERVICE_UUID))) return;
  LOG_INF("BLE", "HID keyboard found: %s", device->getAddress().toString().c_str());
  NimBLEDevice::getScan()->stop();
  delete device_;
  device_ = new NimBLEAdvertisedDevice(*device);
  connectPending_ = true;
}

void BleKeyboard::onScanEnd(const NimBLEScanResults& results, int reason) {
  scanning_ = false;
  LOG_DBG("BLE", "Scan ended (%d devices found)", results.getCount());
}

// ============================================================================
// Connection (runs inside BLE task)
// ============================================================================

bool BleKeyboard::connectToDevice() {
  if (!device_) return false;

  if (client_) {
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
  }

  client_ = NimBLEDevice::createClient(device_->getAddress());
  client_->setClientCallbacks(this, false);

  LOG_INF("BLE", "Connecting...");
  if (!client_->connect()) {
    LOG_ERR("BLE", "Connection failed");
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
    return false;
  }

  NimBLERemoteService* svc = client_->getService(NimBLEUUID(HID_SERVICE_UUID));
  if (!svc) {
    LOG_ERR("BLE", "HID service not found");
    client_->disconnect();
    return false;
  }

  bool subscribed = false;
  for (auto* ch : svc->getCharacteristics(false)) {
    if (ch->getUUID() == NimBLEUUID(HID_REPORT_UUID) && ch->canNotify()) {
      if (ch->subscribe(true, notifyCallback)) {
        subscribed = true;
        LOG_INF("BLE", "Subscribed to HID report");
      }
    }
  }

  if (!subscribed) {
    LOG_ERR("BLE", "No notifiable HID report found");
    client_->disconnect();
    return false;
  }

  ready_ = true;
  LOG_INF("BLE", "Keyboard ready");
  return true;
}

void BleKeyboard::startScan() {
  scanning_ = true;
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(this);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->setActiveScan(true);
  scan->start(10000);  // 10 s in ms
  LOG_INF("BLE", "Scanning for HID keyboards...");
}

// ============================================================================
// BLE task — all blocking BLE work runs here, never in main task
// ============================================================================

void BleKeyboard::bleTaskEntry(void* arg) {
  static_cast<BleKeyboard*>(arg)->bleTaskRun();
  vTaskDelete(nullptr);
}

void BleKeyboard::bleTaskRun() {
  NimBLEDevice::init("X3Terminal");
  startScan();

  while (!stopRequested_) {
    if (connectPending_) {
      connectPending_ = false;
      scanning_ = false;
      if (!connectToDevice()) {
        vTaskDelay(pdMS_TO_TICKS(500));
        startScan();
      }
    }
    if (!connected_ && !scanning_ && !connectPending_) {
      vTaskDelay(pdMS_TO_TICKS(500));
      if (!stopRequested_) startScan();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  // Cleanup
  ready_ = false;
  connected_ = false;
  if (client_) {
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
  }
  delete device_;
  device_ = nullptr;
  NimBLEDevice::deinit(false);
  LOG_INF("BLE", "BLE task done");
}

// ============================================================================
// Public API
// ============================================================================

void BleKeyboard::begin(KeyCallback cb) {
  callback_ = cb;
  instance_ = this;
  stopRequested_ = false;
  keyQueue_ = xQueueCreate(KEY_QUEUE_LEN, KEY_STR_MAX);
  xTaskCreate(bleTaskEntry, "BLE", 4096, this, 1, &bleTask_);
}

void BleKeyboard::stop() {
  stopRequested_ = true;
  // Signal scan/connect to abort
  NimBLEDevice::getScan()->stop();
  if (bleTask_) {
    // Wait up to 3s for task to exit
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
  instance_ = nullptr;
}

void BleKeyboard::loop() {
  // Dispatch queued keys → HTTP  (main task, non-blocking)
  if (keyQueue_ && callback_) {
    char buf[KEY_STR_MAX];
    while (xQueueReceive(keyQueue_, buf, 0) == pdTRUE) {
      callback_(std::string(buf));
    }
  }
}
