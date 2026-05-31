/**
 * XtcParser.h
 *
 * XTC file parsing and page data extraction
 * XTC ebook support for CrossPoint Reader
 */

#pragma once

#include <HalStorage.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "XtcTypes.h"

namespace xtc {

/**
 * XTC File Parser
 *
 * Reads XTC files from SD card and extracts page data.
 * Designed for ESP32-C3's limited RAM (~380KB) using streaming.
 *
 * The source file is kept closed between reads to free heap for rendering.
 * It is reopened on-demand for page table lookups and bitmap data reads.
 */
class XtcParser {
 public:
  XtcParser();
  ~XtcParser();

  // File open/close
  XtcError open(const char* filepath);
  void close();
  bool isOpen() const { return m_isOpen; }

  // Header information access
  const XtcHeader& getHeader() const { return m_header; }
  uint16_t getPageCount() const { return m_header.pageCount; }
  uint16_t getWidth() const { return m_defaultWidth; }
  uint16_t getHeight() const { return m_defaultHeight; }
  uint8_t getBitDepth() const { return m_bitDepth; }  // 1 = XTC/XTG, 2 = XTCH/XTH

  // Page information
  bool getPageInfo(uint32_t pageIndex, PageInfo& info);

  /**
   * Load page bitmap (raw 1-bit data, skipping XTG header)
   *
   * @param pageIndex Page index (0-based)
   * @param buffer Output buffer (caller allocated)
   * @param bufferSize Buffer size
   * @return Number of bytes read on success, 0 on failure
   */
  size_t loadPage(uint32_t pageIndex, uint8_t* buffer, size_t bufferSize);

  /**
   * Load a single plane of an XTH (2-bit) page.
   * Used as a fallback for severely fragmented heaps where neither one big
   * combined allocation nor two parallel plane allocations fit. The caller
   * holds one plane in heap and uses the framebuffer as scratch for the
   * second, re-reading it each pass.
   *
   * @param planeIndex 0 for bit-1 plane, 1 for bit-2 plane.
   */
  size_t loadPageXthPlane(uint32_t pageIndex, uint8_t planeIndex, uint8_t* buf, size_t bufSize);

  /**
   * Load XTH (2-bit) page bitmap into two separate plane buffers.
   * Splitting the ~100 KB combined bitmap into two ~50 KB allocations survives
   * a fragmented heap that rejects one contiguous large block.
   *
   * @param pageIndex Page index (0-based)
   * @param plane1 Bit-1 plane buffer (at least planeBufferSize bytes)
   * @param plane2 Bit-2 plane buffer (at least planeBufferSize bytes)
   * @param planeBufferSize Size of each plane buffer
   * @return Total bytes read (== 2 * actualPlaneSize) on success, 0 on failure
   */
  size_t loadPageXthPlanes(uint32_t pageIndex, uint8_t* plane1, uint8_t* plane2, size_t planeBufferSize);

  /**
   * Streaming page load
   * Memory-efficient method that reads page data in chunks.
   *
   * @param pageIndex Page index
   * @param callback Callback function to receive data chunks
   * @param chunkSize Chunk size (default: 1024 bytes)
   * @return Error code
   */
  XtcError loadPageStreaming(uint32_t pageIndex,
                             std::function<void(const uint8_t* data, size_t size, size_t offset)> callback,
                             size_t chunkSize = 1024);

  // Get title/author from metadata
  std::string getTitle() const { return m_title; }
  std::string getAuthor() const { return m_author; }

  bool hasChapters() const { return m_hasChapters; }
  const std::vector<ChapterInfo>& getChapters();

  // Validation
  static bool isValidXtcFile(const char* filepath);

  // Error information
  XtcError getLastError() const { return m_lastError; }

 private:
  HalFile m_file;
  std::string m_filepath;
  bool m_isOpen;
  XtcHeader m_header;
  std::vector<ChapterInfo> m_chapters;
  std::string m_title;
  std::string m_author;

  // Cached page table — read once during open() to avoid per-page seek+read
  std::vector<PageTableEntry> m_pageTable;

  uint16_t m_defaultWidth;
  uint16_t m_defaultHeight;
  uint8_t m_bitDepth;  // 1 = XTC/XTG (1-bit), 2 = XTCH/XTH (2-bit)
  bool m_hasChapters;
  bool m_chaptersLoaded;
  XtcError m_lastError;

  // Internal helper functions
  XtcError readHeader();
  XtcError readTitle();
  XtcError readAuthor();
  XtcError readChapters();
  bool readPageTableEntry(uint32_t pageIndex, PageInfo& info);

  // File handle management — reopen on demand, close after use
  bool ensureFileOpen();
  void closeFile();
};

}  // namespace xtc
