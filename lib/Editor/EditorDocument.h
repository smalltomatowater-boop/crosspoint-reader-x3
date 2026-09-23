#pragma once

#include <HalStorage.h>

#include <memory>
#include <string>
#include <string_view>

#include "PieceTable.h"

// Thin SD-I/O glue around PieceTable: owns the actual file handles — an immutable
// "Base" (the original file, or the last flatten()/save() snapshot) and an
// append-only "Edit" store for new bytes — plus a small in-RAM write-combining buffer
// for the Edit store's hot tail. PieceTable does all the offset bookkeeping; this
// class only resolves pieces to real bytes via HalFile and manages the scratch files
// under /.crosspoint/edit/.
//
// Not host-testable — HalFile can't be constructed off-device. The piece-table math
// this depends on is covered by test/piece_table instead; keep this class thin enough
// that every line here is "call the right HalFile method at the right offset," not
// logic worth re-deriving on its own.
//
// Single document at a time — matches the editor's own single-document scope. Opening
// a new document (or destroying this object) closes whatever was open first.
class EditorDocument {
 public:
  EditorDocument();
  ~EditorDocument();

  EditorDocument(const EditorDocument&) = delete;
  EditorDocument& operator=(const EditorDocument&) = delete;

  // Opens `path` for editing ("" => brand-new untitled document). Closes any
  // previously open document first. Returns false on I/O failure (left closed).
  bool open(const std::string& path);

  uint32_t length() const { return pieceTable_.length(); }
  bool isDirty() const { return dirty_; }
  const std::string& currentPath() const { return currentPath_; }

  // Reads up to `count` bytes at logical offset `pos` into `out`. Returns bytes
  // actually read (less than `count` at EOF or on a read failure). For materializing
  // the small text windows EditorActivity renders/wraps — never call this with the
  // whole document.
  uint32_t readAt(uint32_t pos, char* out, uint32_t count);

  // Inserts UTF-8 bytes at logical offset `pos` (clamped to [0, length()]). Returns
  // false on OOM/IO failure — the caller should treat that as "this keystroke could
  // not be applied," not silently swallow it.
  bool insertAt(uint32_t pos, std::string_view text);

  // Deletes `count` bytes at logical offset `pos` (clamped to document bounds).
  // Cannot fail — see PieceTable::remove().
  void deleteAt(uint32_t pos, uint32_t count);

  bool needsFlatten() const { return pieceTable_.needsFlatten(); }
  // Consolidates the piece table down to a single piece by rewriting the whole
  // document into the internal scratch "flat" file (crash-safe rename swap — see
  // consolidateTo()). Call on an idle debounce, never mid-keystroke: it reads and
  // rewrites the whole document once. insertAt() also calls this itself as a
  // last-resort safety net if the piece table is ever actually at capacity, which
  // should not happen if the caller flattens on idle as intended.
  bool flatten();

  // Flushes the in-RAM edit-head buffer to the SD edit-store file. Bounds how much
  // recently-typed text would be lost on power loss to "whatever was typed since the
  // last flush" — call this on the same idle debounce as flatten() and before
  // save()/close(). A cheap no-op when the head is already empty.
  void flushEditHead();

  // Crash-safe save: materializes the full document into `path` via a temp file plus
  // a backup-rotate rename swap (SdFat's rename() refuses to overwrite an existing
  // destination, so a plain "remove old, rename tmp" would have a window where
  // neither file exists — see consolidateTo()), then reopens Base pointing at `path`
  // and clears the Edit store, exactly like flatten() but targeting the caller's real
  // file instead of the internal scratch one. On success, currentPath() == path and
  // isDirty() == false.
  bool save(const std::string& path);

  // Closes file handles, deletes the internal scratch files, and resets to an empty,
  // unopened document. Safe whether or not the last edits were saved — an unsaved
  // document's scratch files are simply discarded (no crash-recovery of unsaved
  // edits in v1; see handoff.md). Safe to call when nothing is open.
  void close();

 private:
  PieceTable pieceTable_;

  std::string currentPath_;  // "" for a never-saved document
  std::string scratchFlatPath_;
  std::string scratchEditPath_;
  bool dirty_ = false;
  bool open_ = false;

  HalFile baseFile_;  // read-only; unopened for a brand-new empty document
  HalFile editFile_;  // read-write append store for new bytes since the last flatten

  static constexpr uint32_t EDIT_HEAD_CAPACITY = 8192;
  std::unique_ptr<uint8_t[]> editHead_;
  uint32_t editHeadLen_ = 0;      // bytes currently buffered in editHead_
  uint32_t editFlushedSize_ = 0;  // editFile_'s on-SD size (bytes already durably written)

  // Appends `len` bytes to the conceptual Edit stream (editHead_ RAM, spilling to
  // editFile_ as needed) and returns the offset they landed at (for the new piece's
  // sourceOffset), or UINT32_MAX on I/O failure.
  uint32_t appendToEditStore(const char* bytes, uint32_t len);
  // Reads `count` bytes at Edit-stream offset `pos` into `out`, transparently from
  // editHead_ RAM or editFile_ on SD. Returns bytes actually read.
  uint32_t readFromEditStore(uint32_t pos, char* out, uint32_t count);
  // Unconditional flush used by both flushEditHead() and appendToEditStore() (when the
  // head is full). Returns false on I/O failure.
  bool flushEditHeadInternal();

  // Rewrites the whole current document to `targetPath` via temp file + backup-rotate
  // rename swap, then repoints baseFile_ at it and resets the Edit store to empty.
  // Shared by flatten() (targetPath == scratchFlatPath_) and save() (targetPath == the
  // caller's path).
  bool consolidateTo(const std::string& targetPath);

  static void scratchPathsFor(const std::string& path, std::string& flatPath, std::string& editPath);
};
