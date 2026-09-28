#pragma once

#include <HalStorage.h>
#include <Logging.h>
#include <stdint.h>

#include <algorithm>
#include <cstring>
#include <string>

// Cache buffer for storing 2-bit pixels (4 levels) during decode.
// Packs 4 pixels per byte, MSB first.
struct PixelCache {
  uint8_t* buffer;
  int width;
  int height;
  int bytesPerRow;
  int originX;  // config.x - to convert screen coords to cache coords
  int originY;  // config.y

  PixelCache() : buffer(nullptr), width(0), height(0), bytesPerRow(0), originX(0), originY(0) {}
  PixelCache(const PixelCache&) = delete;
  PixelCache& operator=(const PixelCache&) = delete;

  static constexpr size_t MAX_CACHE_BYTES = 256 * 1024;  // 256KB limit for embedded targets

  bool allocate(int w, int h, int ox, int oy) {
    width = w;
    height = h;
    originX = ox;
    originY = oy;
    bytesPerRow = (w + 3) / 4;  // 2 bits per pixel, 4 pixels per byte
    size_t bufferSize = (size_t)bytesPerRow * h;
    if (bufferSize > MAX_CACHE_BYTES) {
      LOG_ERR("IMG", "Cache buffer too large: %d bytes for %dx%d (limit %d)", bufferSize, w, h, MAX_CACHE_BYTES);
      return false;
    }
    buffer = (uint8_t*)malloc(bufferSize);
    if (buffer) {
      memset(buffer, 0, bufferSize);
      LOG_DBG("IMG", "Allocated cache buffer: %d bytes for %dx%d", bufferSize, w, h);
    }
    return buffer != nullptr;
  }

  void setPixel(int screenX, int screenY, uint8_t value) {
    if (!buffer) return;
    int localX = screenX - originX;
    int localY = screenY - originY;
    if (localX < 0 || localX >= width || localY < 0 || localY >= height) return;

    int byteIdx = localY * bytesPerRow + localX / 4;
    int bitShift = 6 - (localX % 4) * 2;  // MSB first: pixel 0 at bits 6-7
    buffer[byteIdx] = (buffer[byteIdx] & ~(0x03 << bitShift)) | ((value & 0x03) << bitShift);
  }

  bool writeToFile(const std::string& cachePath) {
    if (!buffer) return false;

    HalFile cacheFile;
    if (!Storage.openFileForWrite("IMG", cachePath, cacheFile)) {
      LOG_ERR("IMG", "Failed to open cache file for writing: %s", cachePath.c_str());
      return false;
    }

    uint16_t w = width;
    uint16_t h = height;
    cacheFile.write(&w, 2);
    cacheFile.write(&h, 2);
    cacheFile.write(buffer, bytesPerRow * height);
    cacheFile.close();

    LOG_DBG("IMG", "Cache written: %s (%dx%d, %d bytes)", cachePath.c_str(), width, height, 4 + bytesPerRow * height);
    return true;
  }

  // ---- Streaming mode ------------------------------------------------------
  // For images whose cache would not fit in RAM (a full-screen image is ~78KB):
  // rows are written to `<path>.tmp` as the decoder moves past them, holding
  // one band of STREAM_BAND_ROWS rows in RAM (~1.6KB for a 396-pixel-wide
  // image), and the file is renamed into place by finishStream(). Writing a
  // band at a time instead of a row at a time cuts a full-screen image from
  // 792 small SD writes to 50 (one row at a time added ~0.8s to the decode).
  // Rows must arrive in increasing order (true for the PNG decoder, which
  // draws each destination row once, top to bottom). Anything else marks the
  // stream failed and no cache file is left behind. Same file format as
  // writeToFile().
  static constexpr int STREAM_BAND_ROWS = 16;
  bool streaming = false;
  bool streamFailed = false;
  int streamRowIdx = -1;  // last local row handed out
  int bandStart = 0;      // local row held in the first line of `buffer`
  HalFile streamFile;
  std::string streamPath;

  // Writes `rows` lines of the band and starts the next band, all white.
  void flushBand(int rows) {
    const size_t bytes = static_cast<size_t>(rows) * bytesPerRow;
    if (!streamFailed && streamFile.write(buffer, bytes) != bytes) streamFailed = true;
    memset(buffer, 0xFF, static_cast<size_t>(STREAM_BAND_ROWS) * bytesPerRow);
    bandStart += rows;
  }

  bool beginStream(int w, int h, int ox, int oy, const std::string& cachePath) {
    width = w;
    height = h;
    originX = ox;
    originY = oy;
    bytesPerRow = (w + 3) / 4;
    buffer = static_cast<uint8_t*>(malloc(static_cast<size_t>(STREAM_BAND_ROWS) * bytesPerRow));
    if (!buffer) return false;
    streamPath = cachePath;
    const std::string tmp = cachePath + ".tmp";
    if (!Storage.openFileForWrite("IMG", tmp, streamFile)) {
      free(buffer);
      buffer = nullptr;
      return false;
    }
    const uint16_t hw = w;
    const uint16_t hh = h;
    if (streamFile.write(&hw, 2) != 2 || streamFile.write(&hh, 2) != 2) streamFailed = true;
    streaming = true;
    streamRowIdx = 0;
    bandStart = 0;
    // Unwritten pixels stay white (value 3).
    memset(buffer, 0xFF, static_cast<size_t>(STREAM_BAND_ROWS) * bytesPerRow);
    return true;
  }

  // Row buffer for screen row `screenY`, flushing the bands before it.
  uint8_t* streamRow(int screenY) {
    const int localY = screenY - originY;
    if (localY < streamRowIdx || localY >= height) {
      streamFailed = true;  // out of order: the file can't be trusted
      return buffer;
    }
    while (localY >= bandStart + STREAM_BAND_ROWS) flushBand(STREAM_BAND_ROWS);
    streamRowIdx = localY;
    return buffer + static_cast<size_t>(localY - bandStart) * bytesPerRow;
  }

  bool finishStream() {
    if (!streaming) return false;
    while (bandStart < height && !streamFailed) flushBand(std::min(STREAM_BAND_ROWS, height - bandStart));
    streamFile.close();
    streaming = false;
    const std::string tmp = streamPath + ".tmp";
    if (streamFailed) {
      Storage.remove(tmp.c_str());
      LOG_ERR("IMG", "Streamed cache discarded: %s", streamPath.c_str());
      return false;
    }
    Storage.remove(streamPath.c_str());  // rename refuses to overwrite
    if (!Storage.rename(tmp.c_str(), streamPath.c_str())) {
      Storage.remove(tmp.c_str());
      return false;
    }
    LOG_DBG("IMG", "Cache streamed: %s (%dx%d)", streamPath.c_str(), width, height);
    return true;
  }

  ~PixelCache() {
    if (streaming) {  // decode aborted: drop the partial file
      streamFile.close();
      Storage.remove((streamPath + ".tmp").c_str());
    }
    if (buffer) {
      free(buffer);
      buffer = nullptr;
    }
  }
};
