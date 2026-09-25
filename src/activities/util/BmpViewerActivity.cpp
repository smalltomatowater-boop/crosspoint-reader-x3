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
#include "components/UITheme.h"
#include "fontIds.h"

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
  GUI.fillPopupProgress(renderer, popupRect, 20);  // Initial 20% progress

  if (!FsHelpers::hasBmpExtension(filePath)) {
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

  renderer.clearScreen();
  if (!decoder->decodeToFramebuffer(filePath, renderer, config)) {
    LOG_ERR("IMGV", "%s decode failed: %s", decoder->getFormatName(), filePath.c_str());
    return false;
  }
  GUI.drawButtonHints(renderer, btn1, btn2, btn3, btn4);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);

  // Grayscale: decode into the LSB and MSB planes and show them, then decode
  // the BW frame once more and re-sync the controller with it for the next
  // differential refresh. Re-decoding (~1.2s for a screen-sized PNG) instead
  // of stashing the BW frame saves 52KB, which the decoder needs itself.
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  const bool lsb = decoder->decodeToFramebuffer(filePath, renderer, config);
  if (lsb) renderer.copyGrayscaleLsbBuffers();
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  const bool msb = lsb && decoder->decodeToFramebuffer(filePath, renderer, config);
  if (msb) {
    renderer.copyGrayscaleMsbBuffers();
    renderer.displayGrayBuffer();
  }
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();
  if (decoder->decodeToFramebuffer(filePath, renderer, config)) {
    GUI.drawButtonHints(renderer, btn1, btn2, btn3, btn4);
    if (msb) renderer.cleanupGrayscaleWithFrameBuffer();
  } else {
    LOG_ERR("IMGV", "BW re-decode failed");
  }
  return true;
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