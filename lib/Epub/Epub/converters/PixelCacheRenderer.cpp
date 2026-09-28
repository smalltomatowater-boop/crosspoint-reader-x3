#include "PixelCacheRenderer.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdlib>

#include "DirectPixelWriter.h"

// Cache file format:
// - uint16_t width
// - uint16_t height
// - uint8_t pixels[...] - 2 bits per pixel, packed (4 pixels per byte, MSB first), row-major order

bool readPixelCacheSize(const std::string& cachePath, int& width, int& height) {
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) return false;
  uint16_t w = 0, h = 0;
  if (cacheFile.read(&w, 2) != 2 || cacheFile.read(&h, 2) != 2) return false;
  if (w == 0 || h == 0) return false;
  width = w;
  height = h;
  return true;
}

bool renderPixelCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth,
                      int expectedHeight, int* outWidth, int* outHeight) {
  HalFile cacheFile;
  if (!Storage.openFileForRead("IMG", cachePath, cacheFile)) {
    return false;
  }

  uint16_t cachedWidth, cachedHeight;
  if (cacheFile.read(&cachedWidth, 2) != 2 || cacheFile.read(&cachedHeight, 2) != 2) {
    return false;
  }

  if (expectedWidth > 0 && expectedHeight > 0) {
    // Allow 1 pixel of rounding difference between decode runs.
    if (abs(cachedWidth - expectedWidth) > 1 || abs(cachedHeight - expectedHeight) > 1) {
      LOG_ERR("IMG", "Cache dimension mismatch: %dx%d vs %dx%d", cachedWidth, cachedHeight, expectedWidth,
              expectedHeight);
      return false;
    }
  }
  if (outWidth) *outWidth = cachedWidth;
  if (outHeight) *outHeight = cachedHeight;

  LOG_DBG("IMG", "Loading from cache: %s (%dx%d)", cachePath.c_str(), cachedWidth, cachedHeight);

  const int bytesPerRow = (cachedWidth + 3) / 4;
  auto rowBuffer = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
  if (!rowBuffer) {
    LOG_ERR("IMG", "Failed to allocate row buffer (%d bytes)", bytesPerRow);
    return false;
  }

  DirectPixelWriter pw;
  pw.init(renderer);

  for (int row = 0; row < cachedHeight; row++) {
    if (cacheFile.read(rowBuffer.get(), bytesPerRow) != bytesPerRow) {
      LOG_ERR("IMG", "Cache read error at row %d", row);
      return false;
    }
    pw.beginRow(y + row);
    for (int col = 0; col < cachedWidth; col++) {
      const int bitShift = 6 - (col & 3) * 2;  // MSB first within byte
      pw.writePixel(x + col, (rowBuffer[col >> 2] >> bitShift) & 0x03);
    }
  }
  return true;
}
