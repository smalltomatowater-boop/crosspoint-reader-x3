#pragma once

#include <functional>
#include <string>

#include "MappedInputManager.h"
#include "activities/Activity.h"

// Image viewer: BMP (drawBitmap), PNG and JPEG (the EPUB image decoders,
// drawn straight into the framebuffer, then a grayscale pass). Left/Right
// step through the images in the same folder; Confirm sets the image as
// the sleep screen (PNG/JPEG are converted to /sleep.bmp once).
class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  void loadSiblingImages();
  void doSetSleepCover();
  // PNG/JPEG: fit to the screen, draw BW with the button hints, then add the
  // two grayscale planes if the BW frame can be stashed. False if undecodable.
  bool renderDecodedImage(const char* btn1, const char* btn2, const char* btn3, const char* btn4);

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
};