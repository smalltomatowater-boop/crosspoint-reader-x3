#pragma once

#include <string>

class GfxRenderer;

// Draws a .pxc pixel cache (2 bits per pixel; see PixelCache) with its
// top-left at (x, y), in the renderer's current render mode (BW, or one
// grayscale plane). Reads one row at a time, so the cache size doesn't
// matter for RAM.
//
// If expectedWidth/expectedHeight are > 0, a cache whose size differs by
// more than 1 pixel is rejected (rounding differences between decode runs).
// outWidth/outHeight, if given, receive the cached size.
// Returns false if the file is missing, unreadable, or rejected.
bool renderPixelCache(GfxRenderer& renderer, const std::string& cachePath, int x, int y, int expectedWidth = 0,
                      int expectedHeight = 0, int* outWidth = nullptr, int* outHeight = nullptr);

// Reads only the size header of a .pxc cache.
bool readPixelCacheSize(const std::string& cachePath, int& width, int& height);
