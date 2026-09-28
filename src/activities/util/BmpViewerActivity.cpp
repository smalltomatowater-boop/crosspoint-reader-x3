#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/PixelCacheRenderer.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* IMAGE_CACHE_DIR = BmpViewerActivity::IMAGE_CACHE_DIR;

// One cache file per image, size and screen: a changed file (new size) or a
// different orientation (different fit) gets a fresh decode.
std::string imageCachePath(const std::string& imagePath, int screenW, int screenH) {
  size_t fileSize = 0;
  HalFile f;
  if (Storage.openFileForRead("IMGV", imagePath, f)) fileSize = f.fileSize();
  char key[32];
  snprintf(key, sizeof(key), ":%u:%dx%d", static_cast<unsigned>(fileSize), screenW, screenH);
  char path[64];
  snprintf(path, sizeof(path), "%s/%08x.pxc", IMAGE_CACHE_DIR,
           static_cast<unsigned>(std::hash<std::string>{}(imagePath + key)));
  return path;
}
}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

void BmpViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) return;

  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  size_t lastSlash = filePath.find_last_of('/');
  std::string fileName = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;

  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.') {
        std::string fname(name);
        if (FsHelpers::hasImageExtension(fname)) {
          siblingImages.push_back(fname);
        }
      }
    }
    file.close();
  }
  dir.close();

  FsHelpers::sortFileList(siblingImages);

  for (size_t i = 0; i < siblingImages.size(); ++i) {
    if (siblingImages[i] == fileName) {
      currentImageIndex = static_cast<int>(i);
      break;
    }
  }
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();

  if (siblingImages.empty() && !filePath.empty()) {
    loadSiblingImages();
  }

  HalFile file;

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  if (!FsHelpers::hasBmpExtension(filePath)) {
    // PNG/JPEG: no progress bar; each popup update is a ~0.6s refresh and the
    // BW preview follows within ~1.5s.
    const bool hasPrevious = (siblingImages.size() > 1 && currentImageIndex > 0);
    const bool hasNext = (siblingImages.size() > 1 && currentImageIndex != -1 &&
                          currentImageIndex < static_cast<int>(siblingImages.size()) - 1);
    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), tr(STR_SET_SLEEP_COVER), (hasPrevious ? "<" : ""), (hasNext ? ">" : ""));
    if (!renderDecodedImage(labels.btn1, labels.btn2, labels.btn3, labels.btn4)) {
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_MEMORY_ERROR));
      const auto backOnly = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, backOnly.btn1, backOnly.btn2, backOnly.btn3, backOnly.btn4);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    return;
  }
  GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress
  // 1. Open the file
  if (Storage.openFileForRead("BMP", filePath, file)) {
    Bitmap bitmap(file, true);

    // 2. Parse headers to get dimensions
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      int x, y;

      if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
        float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
        const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

        if (ratio > screenRatio) {
          // Wider than screen
          x = 0;
          y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
        } else {
          // Taller than screen
          x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
          y = 0;
        }
      } else {
        // Center small images
        x = (pageWidth - bitmap.getWidth()) / 2;
        y = (pageHeight - bitmap.getHeight()) / 2;
      }

      // 4. Prepare Rendering
      bool hasPrevious = (siblingImages.size() > 1 && currentImageIndex > 0);
      bool hasNext = (siblingImages.size() > 1 && currentImageIndex != -1 &&
                      currentImageIndex < static_cast<int>(siblingImages.size()) - 1);

      const auto labels =
          mappedInput.mapLabels(tr(STR_BACK), tr(STR_SET_SLEEP_COVER), (hasPrevious ? "<" : ""), (hasNext ? ">" : ""));

      GUI.fillPopupProgress(renderer, popupRect, 50);

      renderer.clearScreen();
      // Assuming drawBitmap defaults to 0,0 crop if omitted, or pass explicitly: drawBitmap(bitmap, x, y, pageWidth,
      // pageHeight, 0, 0)
      renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);

      // Draw UI hints on the base layer
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      // Single pass for non-grayscale images

      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

    } else {
      // Handle file parsing error
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Invalid BMP File");
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }

    file.close();
  } else {
    // Handle file open error
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Could not open file");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}

bool BmpViewerActivity::renderDecodedImage(const char* btn1, const char* btn2, const char* btn3, const char* btn4) {
  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(filePath);
  ImageDimensions dims{};
  if (!decoder || !decoder->getDimensions(filePath, dims) || dims.width <= 0 || dims.height <= 0) {
    LOG_ERR("IMGV", "Cannot decode %s (free %u, largest %u)", filePath.c_str(), ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }

  // Fit inside the screen, centred, never upscaled (the decoder applies the
  // same rule to maxWidth/maxHeight; this just works out the offset).
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  float scale = std::min(static_cast<float>(pageWidth) / dims.width, static_cast<float>(pageHeight) / dims.height);
  if (scale > 1.0f) scale = 1.0f;
  RenderConfig config;
  config.x = (pageWidth - static_cast<int>(dims.width * scale)) / 2;
  config.y = (pageHeight - static_cast<int>(dims.height * scale)) / 2;
  config.maxWidth = pageWidth;
  config.maxHeight = pageHeight;
  config.useGrayscale = true;
  config.useDithering = true;

  // Decode once; the decoder also writes a 2-bit pixel cache (streamed to
  // the SD card for large PNGs), and the grayscale planes and the final BW
  // frame are drawn from that. Opening the image again skips the decode.
  const std::string cachePath = imageCachePath(filePath, pageWidth, pageHeight);
  int cacheW = 0, cacheH = 0;
  bool cached = readPixelCacheSize(cachePath, cacheW, cacheH);
  int x = config.x, y = config.y;

  renderer.clearScreen();
  if (cached) {
    x = (pageWidth - cacheW) / 2;
    y = (pageHeight - cacheH) / 2;
    cached = renderPixelCache(renderer, cachePath, x, y);
  }
  if (!cached) {
    Storage.mkdir(IMAGE_CACHE_DIR);
    config.cachePath = cachePath;
    renderer.clearScreen();
    if (!decoder->decodeToFramebuffer(filePath, renderer, config)) {
      LOG_ERR("IMGV", "%s decode failed: %s", decoder->getFormatName(), filePath.c_str());
      return false;
    }
    cached = readPixelCacheSize(cachePath, cacheW, cacheH);
    if (!cached) LOG_INF("IMGV", "No pixel cache; grayscale will re-decode");
  }
  GUI.drawButtonHints(renderer, btn1, btn2, btn3, btn4);
  // Always FAST for the BW preview: the grayscale pass right after redraws
  // the whole image, and the X3 driver forces a full sync on the first
  // refresh after any grayscale frame anyway. On the X3 a HALF_REFRESH
  // becomes FULL plus a settle pass (HalDisplay::displayBuffer), ~2.5s.
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);

  // One pass in the current render mode, from the cache or (no cache) by
  // decoding again.
  config.cachePath.clear();
  auto drawPass = [&]() {
    if (cached) return renderPixelCache(renderer, cachePath, x, y);
    return decoder->decodeToFramebuffer(filePath, renderer, config);
  };

  // Grayscale: draw the LSB and MSB planes and show them, then redraw the BW
  // frame and re-sync the controller with it for the next differential
  // refresh (redrawing instead of stashing the BW frame saves 52KB).
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  const bool lsb = drawPass();
  if (lsb) renderer.copyGrayscaleLsbBuffers();
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  const bool msb = lsb && drawPass();
  if (msb) {
    renderer.copyGrayscaleMsbBuffers();
    renderer.displayGrayBuffer();
  }
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();
  if (drawPass()) {
    GUI.drawButtonHints(renderer, btn1, btn2, btn3, btn4);
    if (msb) renderer.cleanupGrayscaleWithFrameBuffer();
  } else {
    LOG_ERR("IMGV", "BW redraw failed");
  }
  return true;
}

void BmpViewerActivity::clearImageCache() {
  if (!Storage.exists(IMAGE_CACHE_DIR)) return;
  if (Storage.removeDir(IMAGE_CACHE_DIR)) {
    LOG_DBG("IMGV", "Image cache cleared");
  } else {
    LOG_ERR("IMGV", "Failed to clear %s", IMAGE_CACHE_DIR);
  }
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

namespace {
// Separate (not inlined) so its 2KB buffer is only on the stack while
// copying a BMP, not under the PNG/JPEG converters' own frames.
__attribute__((noinline)) bool copyFile(HalFile& in, HalFile& out) {
  char buffer[2048];
  int bytesRead;
  while ((bytesRead = in.read(buffer, sizeof(buffer))) > 0) {
    if (out.write(buffer, bytesRead) != static_cast<size_t>(bytesRead)) return false;
  }
  return true;
}
}  // namespace

void BmpViewerActivity::doSetSleepCover() {
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  bool success = false;
  HalFile inFile, outFile;
  if (Storage.openFileForRead("BMP", filePath, inFile)) {
    if (Storage.openFileForWrite("BMP", "/sleep.bmp", outFile)) {
      if (FsHelpers::hasPngExtension(filePath)) {
        // The sleep screen reads BMP only: convert this one image, fitted
        // to the screen (not cropped; the cover-mode setting still applies).
        success = PngToBmpConverter::pngFileToBmpStream(inFile, outFile, /*crop=*/false);
      } else if (FsHelpers::hasJpgExtension(filePath)) {
        success = JpegToBmpConverter::jpegFileToBmpStream(inFile, outFile, /*crop=*/false);
      } else {
        success = copyFile(inFile, outFile);
      }
      outFile.close();
      if (!success) Storage.remove("/sleep.bmp");  // don't leave a half-written cover
    }
    inFile.close();
  }

  if (success) {
    SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    SETTINGS.saveToFile();
    GUI.drawPopup(renderer, tr(STR_DONE));
  } else {
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
  }

  delay(1000);
  onEnter();
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    doSetSleepCover();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (siblingImages.size() > 1 && currentImageIndex > 0) {
      currentImageIndex--;
      std::string dirPath = FsHelpers::extractFolderPath(filePath);
      if (dirPath.back() != '/') dirPath += "/";
      filePath = dirPath + siblingImages[currentImageIndex];
      onEnter();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (siblingImages.size() > 1 && currentImageIndex != -1 &&
        currentImageIndex < static_cast<int>(siblingImages.size()) - 1) {
      currentImageIndex++;
      std::string dirPath = FsHelpers::extractFolderPath(filePath);
      if (dirPath.back() != '/') dirPath += "/";
      filePath = dirPath + siblingImages[currentImageIndex];
      onEnter();
    }
    return;
  }
}