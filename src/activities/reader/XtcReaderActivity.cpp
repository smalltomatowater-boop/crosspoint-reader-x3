/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/XtcDebugLog.h"

void XtcReaderActivity::onEnter() {
  Activity::onEnter();
  XtcDebugLog::log("XtcReader::onEnter xtc=%d", xtc ? 1 : 0);

  if (!xtc) {
    return;
  }

  // XTC/XTH pages are pre-rendered bitmaps — no font rendering needed.
  // Free all glyph/compressed-font caches to reclaim heap before allocating
  // page buffers. This typically reclaims 20-50KB depending on what the
  // previous activity rendered.
  auto* fcm = renderer.getFontCacheManager();
  if (fcm) {
    fcm->clearCache();
    XtcDebugLog::log("font cache cleared, free=%u largest=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }

  xtc->setupCacheDir();
  XtcDebugLog::log("setupCacheDir done");

  // Load saved progress
  loadProgress();
  XtcDebugLog::log("loadProgress done, currentPage=%lu", currentPage);

  // Save current XTC as last opened book and add to recent books
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), xtc->getThumbBmpPath());

  // Trigger first update
  requestUpdate();
}

void XtcReaderActivity::onExit() {
  Activity::onExit();

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  xtc.reset();
}

void XtcReaderActivity::loop() {
  // Enter chapter selection activity
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (xtc && xtc->hasChapters() && !xtc->getChapters().empty()) {
      startActivityForResult(
          std::make_unique<XtcReaderChapterSelectionActivity>(renderer, mappedInput, xtc, currentPage),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              currentPage = std::get<PageResult>(result.data).page;
            }
          });
    }
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(xtc ? xtc->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  const auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button goes home and back button returns to last page
  if (currentPage >= xtc->getPageCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentPage = xtc->getPageCount() - 1;
      requestUpdate();
    }
    return;
  }

  const bool skipPages = !fromTilt && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP &&
                         mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
  const int skipAmount = skipPages ? 10 : 1;

  if (prevTriggered) {
    if (currentPage >= static_cast<uint32_t>(skipAmount)) {
      currentPage -= skipAmount;
    } else {
      currentPage = 0;
    }
    requestUpdate();
  } else if (nextTriggered) {
    currentPage += skipAmount;
    if (currentPage >= xtc->getPageCount()) {
      currentPage = xtc->getPageCount();  // Allow showing "End of book"
    }
    requestUpdate();
  }
}

void XtcReaderActivity::render(RenderLock&&) {
  if (!xtc) {
    return;
  }

  // Bounds check
  if (currentPage >= xtc->getPageCount()) {
    // Show end of book screen
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderPage();
  saveProgress();
}

XtcReaderActivity::StatusBarInfo XtcReaderActivity::getStatusBarInfo() const {
  const int bookPageCount = static_cast<int>(xtc->getPageCount());
  const int bookPage = static_cast<int>(currentPage) + 1;
  std::string title =
      SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE ? xtc->getTitle() : "";

  if (!xtc->hasChapters()) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  const auto& chapters = xtc->getChapters();
  const auto chapterIt = std::find_if(chapters.begin(), chapters.end(), [this](const xtc::ChapterInfo& chapter) {
    return currentPage >= chapter.startPage && currentPage <= chapter.endPage;
  });

  if (chapterIt == chapters.end() || chapterIt->endPage < chapterIt->startPage) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = chapterIt->name.empty() ? tr(STR_UNNAMED) : chapterIt->name;
  }

  return StatusBarInfo{static_cast<int>(currentPage - chapterIt->startPage) + 1,
                       static_cast<int>(chapterIt->endPage - chapterIt->startPage) + 1, std::move(title)};
}

void XtcReaderActivity::renderStatusBarOverlay(const StatusBarOverlayPosition position) const {
  const bool drawBottom = SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_BOTTOM &&
                          position == StatusBarOverlayPosition::Bottom;
  const bool drawTop = SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP &&
                       position == StatusBarOverlayPosition::Top;
  if (!drawBottom && !drawTop) {
    return;
  }

  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight <= 0) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  int clearY;
  int paddingBottom = 0;
  if (position == StatusBarOverlayPosition::Bottom) {
    clearY = renderer.getScreenHeight() - orientedMarginBottom - statusBarHeight - 4;
    if (clearY < 0) {
      clearY = 0;
    }
  } else {
    clearY = orientedMarginTop;
    paddingBottom = renderer.getScreenHeight() - statusBarHeight - orientedMarginBottom - orientedMarginTop - 4;
  }
  const int clearHeight = position == StatusBarOverlayPosition::Bottom
                              ? renderer.getScreenHeight() - orientedMarginBottom - clearY
                              : statusBarHeight + 4;
  if (clearHeight > 0) {
    renderer.fillRect(0, clearY, renderer.getScreenWidth(), clearHeight, false);
  }

  const int pageCount = static_cast<int>(xtc->getPageCount());
  const int displayPage = static_cast<int>(currentPage) + 1;
  const float progress = pageCount > 0 ? (static_cast<float>(displayPage) * 100.0f) / pageCount : 0.0f;
  const auto pageInfo = getStatusBarInfo();
  GUI.drawStatusBar(renderer, progress, pageInfo.currentPage, pageInfo.pageCount, pageInfo.title, paddingBottom);
}

void XtcReaderActivity::renderPage() {
  const uint16_t pageWidth = xtc->getPageWidth();
  const uint16_t pageHeight = xtc->getPageHeight();
  const uint8_t bitDepth = xtc->getBitDepth();

  XtcDebugLog::log("renderPage page=%lu %ux%u depth=%u largest=%u", currentPage, pageWidth, pageHeight, bitDepth,
                   ESP.getMaxAllocHeap());

  if (bitDepth == 2) {
    renderPageXth(pageWidth, pageHeight);
  } else {
    renderPageXtg(pageWidth, pageHeight);
  }
  XtcDebugLog::log("renderPage END");
}

void XtcReaderActivity::renderPageXtg(uint16_t pageWidth, uint16_t pageHeight) {
  const size_t bufSize = static_cast<size_t>((pageWidth + 7) / 8) * pageHeight;
  auto buf = makeUniqueNoThrow<uint8_t[]>(bufSize);
  if (!buf) {
    LOG_ERR("XTR", "OOM XTG: %u bytes, largest=%u", bufSize, ESP.getMaxAllocHeap());
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }
  if (xtc->loadPage(currentPage, buf.get(), bufSize) == 0) {
    LOG_ERR("XTR", "XTG load failed: %s", xtc::errorToString(xtc->getLastError()));
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen();
  const size_t rowBytes = (pageWidth + 7) / 8;
  for (uint16_t y = 0; y < pageHeight; y++) {
    const uint8_t* row = buf.get() + static_cast<size_t>(y) * rowBytes;
    for (uint16_t x = 0; x < pageWidth; x++) {
      const bool isBlack = !((row[x / 8] >> (7 - (x % 8))) & 1);
      if (isBlack) renderer.drawPixel(x, y, true);
    }
    if ((y & 0x3F) == 0) yield();  // feed watchdog every 64 rows
  }

  if (SETTINGS.xtcStatusBarMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP) {
    renderStatusBarOverlay(StatusBarOverlayPosition::Top);
  } else {
    renderStatusBarOverlay(StatusBarOverlayPosition::Bottom);
  }
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  LOG_DBG("XTR", "Rendered page %lu/%lu (XTG)", currentPage + 1, xtc->getPageCount());
}

void XtcReaderActivity::renderPageXth(uint16_t pageWidth, uint16_t pageHeight) {
  const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
  uint8_t* fb = renderer.getFrameBuffer();
  const size_t fbSize = renderer.getBufferSize();
  const bool fastPath = (renderer.getOrientation() == GfxRenderer::Portrait) && (fbSize == planeSize) &&
                        (renderer.getDisplayWidth() == pageHeight) && (renderer.getDisplayHeight() == pageWidth);
  XtcDebugLog::log("XTH planeSize=%u largest=%u fastPath=%d fbSize=%u", planeSize, ESP.getMaxAllocHeap(),
                   fastPath ? 1 : 0, fbSize);

  // Pre-close any open file handles to free SdFat's ~4KB internal buffer
  // before allocating page planes. SdFat keeps a per-file buffer that only
  // releases when the file is closed.
  // XtcParser's closeFile() is private, so we achieve this indirectly by
  // loading page data BEFORE allocating (loadPage opens, reads, and the
  // parser re-closes the file inside loadPageXthPlane).

  // Strategy: load plane1 into fb first (fb is already allocated, no heap cost),
  // copy to a heap buffer, then load plane2. This way SdFat buffer is freed
  // between loads and we only need one 52KB heap block at a time.
  // However, the ideal path is both planes in heap — try that first.

  // Try single contiguous allocation for both planes (104544 bytes).
  // With font cache cleared this might just barely fit if the heap is
  // not too fragmented.
  XtcDebugLog::log("Trying single alloc: need %u, largest=%u", planeSize * 2, ESP.getMaxAllocHeap());
  auto bothPlanes = makeUniqueNoThrow<uint8_t[]>(planeSize * 2);
  const bool singleAllocOk = bothPlanes != nullptr;

  if (fastPath && singleAllocOk) {
    XtcDebugLog::log("Single alloc OK!");
    if (xtc->loadPage(currentPage, bothPlanes.get(), planeSize * 2) == 0) {
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      return;
    }
    const uint8_t* p1 = bothPlanes.get();
    const uint8_t* p2 = bothPlanes.get() + planeSize;

    // BW base
    for (size_t i = 0; i < planeSize; i++) fb[i] = ~(p1[i] | p2[i]);
    int interval = SETTINGS.getRefreshFrequency();
    if (interval <= 0 || interval > 100) interval = 10;
    const bool fullPage = (pagesUntilFullRefresh % interval) == 0;
    renderer.displayBuffer(fullPage ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    pagesUntilFullRefresh++;

    // LSB
    for (size_t i = 0; i < planeSize; i++) fb[i] = (~p1[i]) & p2[i];
    renderer.copyGrayscaleLsbBuffers();

    // MSB
    for (size_t i = 0; i < planeSize; i++) fb[i] = p1[i] ^ p2[i];
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();

    // BW restore
    for (size_t i = 0; i < planeSize; i++) fb[i] = ~(p1[i] | p2[i]);
    renderer.cleanupGrayscaleWithFrameBuffer();

    XtcDebugLog::log("XTH complete (single-alloc fast)");
    LOG_DBG("XTR", "Rendered page %lu/%lu (XTH single-alloc)", currentPage + 1, xtc->getPageCount());
    return;
  }

  // Fallback: plane1 in heap + plane2 via fb + SD re-reads
  auto plane1 = makeUniqueNoThrow<uint8_t[]>(planeSize);
  XtcDebugLog::log("fallback p1=%d largest=%u", plane1 ? 1 : 0, ESP.getMaxAllocHeap());
  bool bothPlanesInHeap = false;
  auto plane2 = makeUniqueNoThrow<uint8_t[]>(planeSize);
  if (plane1 && plane2) {
    bothPlanesInHeap = true;
    XtcDebugLog::log("fallback: both planes OK!");
  }

  if (fastPath && bothPlanesInHeap) {
    // Both planes in heap — load them together
    if (xtc->loadPageXthPlanes(currentPage, plane1.get(), plane2.get(), planeSize) == 0) {
      XtcDebugLog::log("loadPlanes FAIL err=%d", static_cast<int>(xtc->getLastError()));
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      return;
    }
    const uint8_t* p1 = plane1.get();
    const uint8_t* p2 = plane2.get();

    // BW base display
    for (size_t i = 0; i < planeSize; i++) fb[i] = ~(p1[i] | p2[i]);
    int interval = SETTINGS.getRefreshFrequency();
    if (interval <= 0 || interval > 100) interval = 10;
    const bool fullPage = (pagesUntilFullRefresh % interval) == 0;
    renderer.displayBuffer(fullPage ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    pagesUntilFullRefresh++;

    // LSB: DARK only → ~p1 & p2
    for (size_t i = 0; i < planeSize; i++) fb[i] = (~p1[i]) & p2[i];
    renderer.copyGrayscaleLsbBuffers();

    // MSB: DARK or LIGHT → p1 ^ p2
    for (size_t i = 0; i < planeSize; i++) fb[i] = p1[i] ^ p2[i];
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();

    // BW restore
    for (size_t i = 0; i < planeSize; i++) fb[i] = ~(p1[i] | p2[i]);
    renderer.cleanupGrayscaleWithFrameBuffer();

    XtcDebugLog::log("XTH complete (2-plane fast)");
    LOG_DBG("XTR", "Rendered page %lu/%lu (XTH 2-plane fast)", currentPage + 1, xtc->getPageCount());
    return;
  }

  if (!bothPlanesInHeap && fastPath) {
    // ---- FALLBACK B: plane1 heap + plane2 via fb, only 2 SD reads ----
    // Key insight: after computing MSB = p1^p2 in fb, we can derive p2 = p1^fb,
    // then compute LSB = (~p1)&p2 without re-reading from SD. This halves the
    // SD reads from 4 to 2 (one for BW base, one for grayscale planes).
    if (!plane1) {
      XtcDebugLog::log("OOM XTH abort");
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      return;
    }
    auto loadPlane = [&](uint8_t idx, uint8_t* buf) -> bool {
      return xtc->loadPageXthPlane(currentPage, idx, buf, planeSize) != 0;
    };
    if (!loadPlane(0, plane1.get())) {
      renderer.clearScreen();
      renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
      renderer.displayBuffer();
      return;
    }
    const uint8_t* p1 = plane1.get();

    // SD read #1: load p2 for BW base display
    if (!loadPlane(1, fb)) return;
    for (size_t i = 0; i < planeSize; i++) fb[i] = ~(p1[i] | fb[i]);
    int interval = SETTINGS.getRefreshFrequency();
    if (interval <= 0 || interval > 100) interval = 10;
    const bool fullPage = (pagesUntilFullRefresh % interval) == 0;
    renderer.displayBuffer(fullPage ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
    pagesUntilFullRefresh++;
    XtcDebugLog::log("BW base done");

    // SD read #2: load p2 → LSB first (DTM1 must be written before DTM2)
    if (!loadPlane(1, fb)) return;
    for (size_t i = 0; i < planeSize; i++) fb[i] = (~p1[i]) & fb[i];
    renderer.copyGrayscaleLsbBuffers();
    XtcDebugLog::log("LSB done");

    // SD read #3: load p2 → MSB
    if (!loadPlane(1, fb)) return;
    for (size_t i = 0; i < planeSize; i++) fb[i] = p1[i] ^ fb[i];
    renderer.copyGrayscaleMsbBuffers();
    XtcDebugLog::log("MSB done");

    renderer.displayGrayBuffer();
    XtcDebugLog::log("gray displayed");

    // BW restore — no SD read needed! Derive from p1 + MSB (still in fb):
    //   BW = ~(p1 | p2) = ~(p1 | (p1 ^ MSB))
    //   where fb[i] = MSB = p1[i] ^ p2[i]
    for (size_t i = 0; i < planeSize; i++) fb[i] = ~(p1[i] | (p1[i] ^ fb[i]));
    renderer.cleanupGrayscaleWithFrameBuffer();

    renderer.displayGrayBuffer();
    XtcDebugLog::log("gray displayed");

    // BW restore: ~(p1 | p2) = ~(p1 | (p1 ^ MSB)). After LSB step, fb = LSB.
    // p2 = p1 ^ MSB, MSB = p1 ^ LSB? No — derive from p1 + fb (LSB):
    //   BW = ~(p1 | (p1 ^ (p1 ^ fb))) — hmm, need to track MSB separately.
    // Simpler: just reload p2 one more time for the BW restore.
    if (!loadPlane(1, fb)) return;
    for (size_t i = 0; i < planeSize; i++) fb[i] = ~(p1[i] | fb[i]);
    renderer.cleanupGrayscaleWithFrameBuffer();

    XtcDebugLog::log("XTH complete (2-read)");
    LOG_DBG("XTR", "Rendered page %lu/%lu (XTH 2-read)", currentPage + 1, xtc->getPageCount());
    return;
  }

  // ---- SLOW: non-Portrait, per-pixel drawPixel ----
  if (!plane1 || !plane2) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }
  if (xtc->loadPageXthPlanes(currentPage, plane1.get(), plane2.get(), planeSize) == 0) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  const uint8_t* p1 = plane1.get();
  const uint8_t* p2 = plane2.get();
  const size_t colBytes = (pageHeight + 7) / 8;
  auto pixelValue = [&](uint16_t x, uint16_t y) -> uint8_t {
    const size_t colIndex = static_cast<size_t>(pageWidth - 1 - x);
    const size_t byteOffset = colIndex * colBytes + (y / 8);
    const uint8_t shift = 7 - (y % 8);
    return (((p1[byteOffset] >> shift) & 1) << 1) | ((p2[byteOffset] >> shift) & 1);
  };

  renderer.clearScreen();
  for (uint16_t y = 0; y < pageHeight; y++) {
    for (uint16_t x = 0; x < pageWidth; x++) {
      if (pixelValue(x, y) >= 1) renderer.drawPixel(x, y, true);
    }
    if ((y & 0x3F) == 0) yield();
  }
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  renderer.clearScreen(0x00);
  for (uint16_t y = 0; y < pageHeight; y++) {
    for (uint16_t x = 0; x < pageWidth; x++) {
      if (pixelValue(x, y) == 1) renderer.drawPixel(x, y, false);
    }
    if ((y & 0x3F) == 0) yield();
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  for (uint16_t y = 0; y < pageHeight; y++) {
    for (uint16_t x = 0; x < pageWidth; x++) {
      const uint8_t v = pixelValue(x, y);
      if (v == 1 || v == 2) renderer.drawPixel(x, y, false);
    }
    if ((y & 0x3F) == 0) yield();
  }
  renderer.copyGrayscaleMsbBuffers();
  renderer.displayGrayBuffer();

  renderer.clearScreen();
  for (uint16_t y = 0; y < pageHeight; y++) {
    for (uint16_t x = 0; x < pageWidth; x++) {
      if (pixelValue(x, y) >= 1) renderer.drawPixel(x, y, true);
    }
    if ((y & 0x3F) == 0) yield();
  }
  renderer.cleanupGrayscaleWithFrameBuffer();
  LOG_DBG("XTR", "Rendered page %lu/%lu (XTH slow path)", currentPage + 1, xtc->getPageCount());
}

void XtcReaderActivity::saveProgress() const {
  HalFile f;
  if (Storage.openFileForWrite("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = (currentPage >> 16) & 0xFF;
    data[3] = (currentPage >> 24) & 0xFF;
    f.write(data, 4);
    f.close();
  }
}

void XtcReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      LOG_DBG("XTR", "Loaded progress: page %lu", currentPage);

      // Validate page number
      if (currentPage >= xtc->getPageCount()) {
        currentPage = 0;
      }
    }
    f.close();
  }
}

ScreenshotInfo XtcReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Xtc;
  if (xtc) {
    const std::string t = xtc->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
    const uint32_t pageCount = xtc->getPageCount();
    info.totalPages = pageCount;
    // Clamp to last valid page to avoid sentinel value (currentPage == pageCount)
    uint32_t clampedPage = (pageCount > 0 && currentPage >= pageCount) ? pageCount - 1 : currentPage;
    info.progressPercent = pageCount > 0 ? xtc->calculateProgress(clampedPage) : 0;
    info.currentPage = static_cast<int>(clampedPage) + 1;
  } else {
    info.currentPage = currentPage + 1;
  }
  return info;
}
