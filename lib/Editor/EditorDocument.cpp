#include "EditorDocument.h"

#include <Logging.h>
#include <Memory.h>

#include <cstring>
#include <functional>

namespace {
constexpr const char* TAG = "EDITDOC";
}  // namespace

EditorDocument::EditorDocument() = default;

EditorDocument::~EditorDocument() { close(); }

void EditorDocument::scratchPathsFor(const std::string& path, std::string& flatPath, std::string& editPath) {
  const size_t hash = std::hash<std::string>{}(path);
  const std::string base = "/.crosspoint/edit/" + std::to_string(hash);
  flatPath = base + ".flat";
  editPath = base + ".edit";
}

bool EditorDocument::open(const std::string& path) {
  close();

  currentPath_ = path;
  dirty_ = false;
  scratchPathsFor(path, scratchFlatPath_, scratchEditPath_);

  Storage.mkdir("/.crosspoint");
  Storage.mkdir("/.crosspoint/edit");

  if (!editHead_) {
    editHead_ = makeUniqueNoThrow<uint8_t[]>(EDIT_HEAD_CAPACITY);
    if (!editHead_) {
      LOG_ERR(TAG, "OOM: edit head buffer (%u bytes)", EDIT_HEAD_CAPACITY);
      return false;
    }
  }

  // Every editing session starts with a fresh, empty edit store — any leftover from
  // an earlier crashed session is discarded rather than recovered (see handoff.md).
  editFile_ = Storage.open(scratchEditPath_.c_str(), O_RDWR | O_CREAT | O_TRUNC);
  if (!editFile_) {
    LOG_ERR(TAG, "Failed to create edit store: %s", scratchEditPath_.c_str());
    return false;
  }
  editFlushedSize_ = 0;
  editHeadLen_ = 0;

  uint32_t baseLen = 0;
  if (!path.empty() && Storage.exists(path.c_str())) {
    baseFile_ = Storage.open(path.c_str(), O_READ);
    if (!baseFile_) {
      LOG_ERR(TAG, "Failed to open base file: %s", path.c_str());
      return false;
    }
    baseLen = static_cast<uint32_t>(baseFile_.fileSize());
  }

  pieceTable_.reset(PieceTable::Source::Base, baseLen);
  open_ = true;
  return true;
}

void EditorDocument::close() {
  if (baseFile_) baseFile_.close();
  if (editFile_) editFile_.close();

  // Only our own internal scratch files are ours to clean up — save() targets the
  // caller's own path and is deliberately left alone here.
  if (!scratchEditPath_.empty()) {
    Storage.remove(scratchEditPath_.c_str());
  }
  if (!scratchFlatPath_.empty()) {
    Storage.remove(scratchFlatPath_.c_str());
    Storage.remove((scratchFlatPath_ + ".tmp").c_str());
    Storage.remove((scratchFlatPath_ + ".bak").c_str());
  }

  pieceTable_.reset(PieceTable::Source::Base, 0);
  editHeadLen_ = 0;
  editFlushedSize_ = 0;
  currentPath_.clear();
  scratchFlatPath_.clear();
  scratchEditPath_.clear();
  dirty_ = false;
  open_ = false;
}

bool EditorDocument::flushEditHeadInternal() {
  if (editHeadLen_ == 0) return true;
  if (!editFile_ || !editFile_.seek(editFlushedSize_)) {
    LOG_ERR(TAG, "Failed to seek edit store to %u", editFlushedSize_);
    return false;
  }
  const size_t written = editFile_.write(editHead_.get(), editHeadLen_);
  if (written != editHeadLen_) {
    LOG_ERR(TAG, "Short write to edit store: wrote %zu of %u bytes", written, editHeadLen_);
    return false;
  }
  editFlushedSize_ += editHeadLen_;
  editHeadLen_ = 0;
  return true;
}

void EditorDocument::flushEditHead() { flushEditHeadInternal(); }

uint32_t EditorDocument::appendToEditStore(const char* bytes, uint32_t len) {
  const uint32_t startOffset = editFlushedSize_ + editHeadLen_;

  uint32_t written = 0;
  while (written < len) {
    const uint32_t space = EDIT_HEAD_CAPACITY - editHeadLen_;
    if (space == 0) {
      if (!flushEditHeadInternal()) return UINT32_MAX;
      continue;
    }
    const uint32_t chunk = (len - written < space) ? (len - written) : space;
    memcpy(editHead_.get() + editHeadLen_, bytes + written, chunk);
    editHeadLen_ += chunk;
    written += chunk;
  }

  return startOffset;
}

uint32_t EditorDocument::readFromEditStore(uint32_t pos, char* out, uint32_t count) {
  uint32_t read = 0;

  if (pos < editFlushedSize_) {
    const uint32_t fromSd = (count < editFlushedSize_ - pos) ? count : (editFlushedSize_ - pos);
    if (!editFile_ || !editFile_.seek(pos)) return 0;
    const int got = editFile_.read(out, fromSd);
    if (got < 0) return 0;
    read = static_cast<uint32_t>(got);
    if (read < fromSd) return read;  // short read; stop here rather than guess
  }

  if (read < count && pos + read >= editFlushedSize_) {
    const uint32_t headOffset = (pos + read) - editFlushedSize_;
    if (headOffset < editHeadLen_) {
      const uint32_t available = editHeadLen_ - headOffset;
      const uint32_t fromHead = (count - read < available) ? (count - read) : available;
      memcpy(out + read, editHead_.get() + headOffset, fromHead);
      read += fromHead;
    }
  }

  return read;
}

uint32_t EditorDocument::readAt(uint32_t pos, char* out, uint32_t count) {
  uint32_t written = 0;
  pieceTable_.visitRange(pos, count, [&](PieceTable::Source source, uint32_t offset, uint32_t length) {
    if (source == PieceTable::Source::Base) {
      if (baseFile_ && baseFile_.seek(offset)) {
        const int got = baseFile_.read(out + written, length);
        if (got > 0) written += static_cast<uint32_t>(got);
      }
    } else {
      written += readFromEditStore(offset, out + written, length);
    }
  });
  return written;
}

bool EditorDocument::insertAt(uint32_t pos, std::string_view text) {
  if (text.empty()) return true;
  if (pos > pieceTable_.length()) pos = pieceTable_.length();

  // Pre-emptive safety-net flatten: guarantees headroom for the worst case (a
  // mid-piece split needs 2 new slots) *before* we touch the edit store below —
  // flatten() resets the edit store, so it must never run after we've already
  // appended bytes we're about to reference from it. Should essentially never
  // trigger if the caller flattens on an idle timer as intended (see needsFlatten()).
  if (pieceTable_.pieceCount() + 2 > PieceTable::MAX_PIECES) {
    if (!flatten()) return false;
  }

  const uint32_t sourceOffset = appendToEditStore(text.data(), static_cast<uint32_t>(text.size()));
  if (sourceOffset == UINT32_MAX) {
    LOG_ERR(TAG, "Failed to append %zu bytes to edit store", text.size());
    return false;
  }

  if (!pieceTable_.insert(pos, PieceTable::Source::Edit, sourceOffset, static_cast<uint32_t>(text.size()))) {
    LOG_ERR(TAG, "insert() refused after pre-flatten headroom check — should be unreachable");
    return false;
  }

  dirty_ = true;
  return true;
}

void EditorDocument::deleteAt(uint32_t pos, uint32_t count) {
  pieceTable_.remove(pos, count);
  dirty_ = true;
}

bool EditorDocument::flatten() { return consolidateTo(scratchFlatPath_); }

bool EditorDocument::save(const std::string& path) {
  if (!consolidateTo(path)) return false;
  currentPath_ = path;
  dirty_ = false;
  return true;
}

bool EditorDocument::consolidateTo(const std::string& targetPath) {
  const std::string tmpPath = targetPath + ".tmp";
  const std::string bakPath = targetPath + ".bak";

  // Write the whole document to a fresh temp file first; the destination is never
  // touched until this is done and closed.
  {
    HalFile tmp = Storage.open(tmpPath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
    if (!tmp) {
      LOG_ERR(TAG, "Failed to open temp file: %s", tmpPath.c_str());
      return false;
    }

    char buf[512];
    bool ok = true;
    const uint32_t total = pieceTable_.length();
    uint32_t pos = 0;
    while (ok && pos < total) {
      const uint32_t want = (total - pos < sizeof(buf)) ? (total - pos) : static_cast<uint32_t>(sizeof(buf));
      const uint32_t got = readAt(pos, buf, want);
      if (got == 0) {
        ok = false;
        break;
      }
      if (tmp.write(buf, got) != got) {
        ok = false;
        break;
      }
      pos += got;
    }
    tmp.close();  // must close before the rename/remove calls below touch this path

    if (!ok) {
      Storage.remove(tmpPath.c_str());
      LOG_ERR(TAG, "Failed to write %s", tmpPath.c_str());
      return false;
    }
  }

  // baseFile_ may already be open on targetPath (a plain re-save, or a repeat
  // flatten() targeting the same scratch file) — must close it before the rename
  // dance below touches that same path.
  if (baseFile_) baseFile_.close();

  // Crash-safe three-step swap (SdFat's rename() refuses to overwrite an existing
  // destination, so a plain "remove old, rename tmp" would have a window where
  // *neither* file exists if power is lost in between):
  //   1. old target -> .bak (if a target already exists)
  //   2. tmp -> target
  //   3. delete .bak
  // A crash at any point leaves the document recoverable: before step 2, the
  // original is intact (at its path or at .bak); after step 2, the new content is
  // already in place and .bak is just a harmless leftover to delete next time.
  const bool hadExisting = Storage.exists(targetPath.c_str());
  if (hadExisting) {
    Storage.remove(bakPath.c_str());  // drop any stale .bak from a previous crash
    if (!Storage.rename(targetPath.c_str(), bakPath.c_str())) {
      Storage.remove(tmpPath.c_str());
      LOG_ERR(TAG, "Failed to back up existing file: %s", targetPath.c_str());
      return false;
    }
  }
  if (!Storage.rename(tmpPath.c_str(), targetPath.c_str())) {
    LOG_ERR(TAG, "Failed to rename %s -> %s", tmpPath.c_str(), targetPath.c_str());
    if (hadExisting) Storage.rename(bakPath.c_str(), targetPath.c_str());  // best-effort restore
    return false;
  }
  if (hadExisting) Storage.remove(bakPath.c_str());

  // Repoint Base at the freshly consolidated file and reset Edit to empty.
  baseFile_ = Storage.open(targetPath.c_str(), O_READ);
  if (!baseFile_) {
    LOG_ERR(TAG, "Failed to reopen consolidated file for read: %s", targetPath.c_str());
    return false;
  }
  const uint32_t newLen = static_cast<uint32_t>(baseFile_.fileSize());

  editFile_ = Storage.open(scratchEditPath_.c_str(), O_RDWR | O_CREAT | O_TRUNC);
  if (!editFile_) {
    LOG_ERR(TAG, "Failed to reset edit store: %s", scratchEditPath_.c_str());
    return false;
  }
  editFlushedSize_ = 0;
  editHeadLen_ = 0;

  pieceTable_.reset(PieceTable::Source::Base, newLen);
  return true;
}
