#include "TerminalActivity.h"

#include <ArduinoJson.h>
#include <HalDisplay.h>
#include <HTTPClient.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>

#include "fontIds.h"
#include "activities/RenderLock.h"

#include <algorithm>
#include <cstring>

#include "activities/ActivityManager.h"

static constexpr int HTTP_PORT = 80;

// ============================================================================
// Helpers
// ============================================================================

std::string TerminalActivity::limitLine(const std::string& s, uint8_t maxLen) {
    std::string out = s;
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
    std::string expanded;
    for (char c : out) {
        if (c == '\t') expanded += "    ";
        else expanded += c;
    }
    if (expanded.size() > maxLen) expanded.resize(maxLen);
    return expanded;
}

// ============================================================================
// Frame application
// ============================================================================

bool TerminalActivity::applyFrame(JsonDocument& doc) {
    JsonArray incoming = doc["rows"].as<JsonArray>();
    if (incoming.isNull()) return false;

    std::vector<std::string> next;
    next.reserve(maxRows);
    for (JsonVariant v : incoming) {
        if ((uint8_t)next.size() >= maxRows) break;
        next.push_back(limitLine(v.as<std::string>(), maxCols));
    }

    uint16_t nx = cursorX, ny = cursorY;
    JsonArray cursor = doc["cursor"].as<JsonArray>();
    if (!cursor.isNull() && cursor.size() >= 2) {
        nx = std::min((uint16_t)(cursor[0] | 0), (uint16_t)(maxCols - 1));
        ny = std::min((uint16_t)(cursor[1] | 0), (uint16_t)(maxRows - 1));
    }

    bool changed = !frameReceived || rows.size() != next.size();
    if (!changed) {
        for (size_t i = 0; i < next.size(); i++)
            if (rows[i] != next[i]) { changed = true; break; }
    }
    if (cursorX != nx || cursorY != ny) changed = true;

    if (!frameReceived) fullRefreshNeeded = true;
    rows = std::move(next);
    cursorX = nx; cursorY = ny;
    frameReceived = true;
    if (changed) frameDirty = true;
    return true;
}

// ============================================================================
// Rendering
// ============================================================================

void TerminalActivity::drawCursor(uint16_t col, uint16_t row) {
    int16_t x = LEFT_MARGIN + col * charW_;
    int16_t y = TOP_MARGIN  + row * charH_;
    renderer.drawRect(x, y, charW_, charH_, 1, true);
}

void TerminalActivity::drawFrame() {
    renderer.clearScreen(0xFF);

    if (!frameReceived) {
        char ipLine[48];
        snprintf(ipLine, sizeof(ipLine), "IP: %s", WiFi.localIP().toString().c_str());
        int midX = displayWidth / 2 - 60;
        int midY = displayHeight / 2 - charH_;
        renderer.drawText(UI_12_FONT_ID, midX, midY,            "X3 Terminal",        true);
        renderer.drawText(UI_12_FONT_ID, midX, midY + charH_,   "Waiting for frames...", true);
        renderer.drawText(UI_12_FONT_ID, midX, midY + charH_*2, ipLine,               true);
        return;
    }

    for (size_t row = 0; row < rows.size() && row < maxRows; row++) {
        int16_t y = TOP_MARGIN + row * charH_;
        renderer.drawText(UI_12_FONT_ID, LEFT_MARGIN, y, rows[row].c_str(), true);
    }
    drawCursor(cursorX, cursorY);
}

void TerminalActivity::render(RenderLock&& lock) {
    bool doFull = fullRefreshNeeded;
    drawFrame();
    renderer.displayBuffer(doFull ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
    frameDirty = false;
    fullRefreshNeeded = false;
    lastDisplayUpdate = millis();
}

// ============================================================================
// HTTP handlers
// ============================================================================

void TerminalActivity::startServer() {
    server = makeUniqueNoThrow<WebServer>(HTTP_PORT);
    if (!server) { LOG_ERR("TERM", "OOM: WebServer"); return; }

    server->on("/", HTTP_GET, [this]() { handleRoot(); });
    server->on("/status", HTTP_GET, [this]() { handleStatus(); });
    server->on("/frame", HTTP_POST, [this]() { handleFrame(); });
    server->on("/display/clear", HTTP_POST, [this]() { handleDisplayClear(); });
    server->on("/exit", HTTP_POST, [this]() { handleExitTerminal(); });
    server->onNotFound([this]() { server->send(404, "text/plain", "Not found\n"); });
    server->begin();
    LOG_INF("TERM", "HTTP server started at http://%s/", WiFi.localIP().toString().c_str());
}

void TerminalActivity::stopServer() {
    if (server) { server->stop(); server.reset(); }
}

void TerminalActivity::handleRoot() {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "X3 Terminal\n\nIP: %s\nCols: %d  Rows: %d\n\n"
             "POST /frame   - send frame\n"
             "GET  /status  - status JSON\n"
             "POST /exit    - exit terminal mode\n",
             WiFi.localIP().toString().c_str(), maxCols, maxRows);
    server->send(200, "text/plain", buf);
}

void TerminalActivity::handleStatus() {
    JsonDocument doc;
    doc["ok"]   = true;
    doc["ip"]   = WiFi.localIP().toString().c_str();
    doc["cols"] = maxCols;
    doc["rows"] = maxRows;
    String body;
    serializeJson(doc, body);
    server->send(200, "application/json", body);
}

void TerminalActivity::handleFrame() {
    if (!server->hasArg("plain")) {
        server->send(400, "text/plain", "Missing body\n");
        return;
    }
    JsonDocument doc;
    auto err = deserializeJson(doc, server->arg("plain"));
    if (err) {
        server->send(400, "text/plain", "JSON error\n");
        return;
    }
    if (!applyFrame(doc)) {
        server->send(400, "text/plain", "Expected rows array\n");
        return;
    }
    server->send(204, "text/plain", "");
}

void TerminalActivity::handleDisplayClear() {
    frameReceived = false;
    fullRefreshNeeded = true;
    frameDirty = true;
    server->send(204, "text/plain", "");
}

void TerminalActivity::handleExitTerminal() {
    server->send(200, "text/plain", "Exiting terminal mode\n");
    finish();
}

// ============================================================================
// Lifecycle
// ============================================================================

void TerminalActivity::onEnter() {
    savedOrientation = renderer.getOrientation();
    renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);

    displayWidth  = renderer.getDisplayWidth();
    displayHeight = renderer.getDisplayHeight();

    // Derive cell size from Migu1M (UI_12_FONT_ID) metrics
    charH_ = static_cast<uint8_t>(renderer.getLineHeight(UI_12_FONT_ID));
    // Migu1M is monospace: ASCII advance = half of CJK advance = half of line height
    charW_ = static_cast<uint8_t>(renderer.getTextWidth(UI_12_FONT_ID, "A"));
    if (charW_ == 0) charW_ = charH_ / 2;  // fallback

    maxCols = (displayWidth  - LEFT_MARGIN * 2) / charW_;
    maxRows = (displayHeight - TOP_MARGIN)       / charH_;

    LOG_INF("TERM", "Terminal: %dx%d cells (cell %dx%d px, display %dx%d px)",
            maxCols, maxRows, charW_, charH_, displayWidth, displayHeight);

    if (WiFi.status() != WL_CONNECTED) {
        startActivityForResult(
            makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput),
            [this](ActivityResult result) {
                if (!result.isCancelled) {
                    startServer();
                    macMiniUrl_ = loadMacMiniUrl();
                    pendingBleInit_ = true;
                    bleInitAfter_ = millis() + 1000;
                } else {
                    finish();
                }
            });
        return;
    }

    startServer();
    macMiniUrl_ = loadMacMiniUrl();
    pendingBleInit_ = true;
    bleInitAfter_ = millis() + 1000;

    fullRefreshNeeded = true;
    frameDirty = true;
    requestUpdate();
}

void TerminalActivity::onExit() {
    bleKeyboard_.stop();
    stopServer();
    renderer.setOrientation(savedOrientation);
}

std::string TerminalActivity::loadMacMiniUrl() {
    HalFile file;
    if (Storage.openFileForRead("TERM", "/terminal.cfg", file)) {
        char buf[64] = {};
        int n = file.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf) - 1);
        if (n > 0) {
            std::string s(buf, n);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (!s.empty()) {
                LOG_INF("TERM", "Mac mini URL from config: %s", s.c_str());
                return s;
            }
        }
    }
    IPAddress gw = WiFi.gatewayIP();
    char def[32];
    snprintf(def, sizeof(def), "http://%d.%d.%d.%d:3333", gw[0], gw[1], gw[2], gw[3]);
    LOG_INF("TERM", "Mac mini URL default: %s", def);
    return std::string(def);
}

void TerminalActivity::sendKeyToMacMini(const std::string& key) {
    if (macMiniUrl_.empty() || WiFi.status() != WL_CONNECTED) return;

    std::string url = macMiniUrl_ + "/keypress";
    std::string body = "{\"key\":\"" + key + "\"}";

    HTTPClient http;
    http.begin(url.c_str());
    http.addHeader("Content-Type", "application/json");
    int code = http.POST(body.c_str());
    http.end();

    if (code != 204) {
        LOG_DBG("TERM", "keypress POST -> %d", code);
    }
}

void TerminalActivity::loop() {
    if (pendingBleInit_ && millis() >= bleInitAfter_) {
        pendingBleInit_ = false;
        bleKeyboard_.begin([this](const std::string& key) { sendKeyToMacMini(key); });
    }

    bleKeyboard_.loop();
    if (server) server->handleClient();

    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Power)) {
        finish();
        return;
    }

    if (!frameDirty) return;

    unsigned long now = millis();
    unsigned long minInterval = fullRefreshNeeded ? MIN_FULL_REFRESH_MS : MIN_FAST_REFRESH_MS;
    if (now - lastDisplayUpdate < minInterval) return;

    requestUpdate();
}
