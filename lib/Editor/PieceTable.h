#pragma once

#include <cstdint>
#include <memory>

// Storage-agnostic piece-table index. Tracks which byte ranges of which backing
// stores (an immutable "Base" and an append-only "Edit" stream) make up the logical
// document, in order — it never touches the bytes themselves. EditorDocument resolves
// each piece to actual bytes via HalStorage; this class only manages the (source,
// offset, length) bookkeeping (insert/remove/split/coalesce), so that logic — the part
// most likely to hide an off-by-one that corrupts a user's notes — can be verified on
// the host without SD hardware (test/piece_table).
//
// Fixed capacity, never reallocates: RAM cost is exactly MAX_PIECES * sizeof(Piece)
// (~18KB), reserved once at construction and held for the life of the editing session,
// per the project's no-repeated-heap-growth rule. Reaching capacity makes insert()/
// remove() return false; EditorDocument must flatten() (which calls reset()) before
// retrying. EditorActivity should trigger that flatten on an idle timer once
// needsFlatten() goes true, well before capacity is actually reached, so the hard
// limit is a safety net rather than the normal path.
class PieceTable {
 public:
  enum class Source : uint8_t { Base, Edit };

  struct Piece {
    Source source;
    uint32_t offset;
    uint32_t length;
  };

  static constexpr uint32_t MAX_PIECES = 1536;
  static constexpr uint32_t FLATTEN_WATERMARK = 1024;

  PieceTable();

  // Resets to a single piece covering [0, length) of `source` (length == 0 => empty
  // document, zero pieces). Used when opening a document and after a flatten/save
  // collapse. Drops the forward-typing fast-path hint.
  void reset(Source source, uint32_t length);

  uint32_t length() const { return totalLength_; }
  uint32_t pieceCount() const { return count_; }
  bool needsFlatten() const { return count_ > FLATTEN_WATERMARK; }
  bool atCapacity() const { return count_ >= MAX_PIECES; }

  // Splices a piece of `insertLength` bytes at logical offset `pos` (clamped to
  // [0, length()]). The caller must have already written those bytes at
  // [sourceOffset, sourceOffset + insertLength) in `source`'s backing store — this call
  // only updates the index.
  //
  // Fast path: if `pos` sits exactly at the end of the most-recently-touched Edit
  // piece, and `source == Edit` with `sourceOffset` immediately following that piece's
  // byte range (i.e. the caller just appended these bytes right after the previous
  // ones with nothing else created in between), the piece is *extended* in place
  // instead of splicing a new entry. This is what keeps the piece count bounded during
  // ordinary forward typing — pieces only grow when the user jumps to a new edit
  // location.
  //
  // Known limitation: this can't fire right after a remove() truncates a piece's tail
  // (the common "type a char, backspace it, retype" pattern), because the backing
  // Edit store is append-only — the removed byte still physically occupies the
  // store's true tail even though no piece references it, so the next insert's bytes
  // land further along and fail the contiguity check below. Content stays correct;
  // the piece count is just one higher than the theoretical minimum in that case. Not
  // worth the extra complexity of an un-appendable RAM head for v1 — see
  // EditorDocument's edit-head comment.
  //
  // Returns false only when a new entry must be spliced and the table is already at
  // MAX_PIECES; the table is left unchanged and the caller must flatten first.
  bool insert(uint32_t pos, Source source, uint32_t sourceOffset, uint32_t insertLength);

  // Removes `count` bytes starting at logical offset `pos` (clamped to document
  // bounds). Always drops the forward-typing fast-path hint (indices can shift).
  //
  // Unlike insert(), this never fails: removing can add at most one new piece for the
  // whole call (splitting the single piece straddling the end of the range, which can
  // only happen on the final step — seeing why is a short argument, not a coincidence:
  // the split branch only runs when the clipped length is less than what's available in
  // that piece, which forces it to equal all of `remaining`, so `remaining` hits zero
  // right after). The backing array reserves one slot beyond MAX_PIECES exactly for
  // that case, so a remove() can never be left partially applied by a capacity refusal
  // the way a partially-completed, failing insert() would be.
  void remove(uint32_t pos, uint32_t count);

  // Visits the pieces overlapping [pos, pos + count) in logical order, each clipped to
  // that range, calling visit(source, sourceOffset, clippedLength) for every one.
  // Used both to materialize bytes for rendering/reads and to walk the whole document
  // for flatten/save. Takes the visitor by template parameter (not std::function) to
  // avoid a heap-allocating closure on what can be a per-render-frame call.
  template <typename Visitor>
  void visitRange(uint32_t pos, uint32_t count, Visitor&& visit) const {
    if (count == 0 || pos >= totalLength_) return;
    const uint32_t end = (count > totalLength_ - pos) ? totalLength_ : pos + count;

    uint32_t cursor = 0;  // logical start of the piece currently being examined
    for (uint32_t i = 0; i < count_ && cursor < end; ++i) {
      const Piece& p = pieces_[i];
      const uint32_t pieceEnd = cursor + p.length;
      if (pieceEnd > pos) {
        const uint32_t clipStart = (cursor > pos) ? cursor : pos;
        const uint32_t clipEnd = (pieceEnd < end) ? pieceEnd : end;
        if (clipEnd > clipStart) {
          visit(p.source, p.offset + (clipStart - cursor), clipEnd - clipStart);
        }
      }
      cursor = pieceEnd;
    }
  }

 private:
  // One slot beyond MAX_PIECES, reserved exclusively for remove()'s split case — see
  // remove()'s doc comment. insert() never lets count_ pass MAX_PIECES itself, so that
  // slot is always free when remove() might need it.
  static constexpr uint32_t PHYSICAL_CAPACITY = MAX_PIECES + 1;
  std::unique_ptr<Piece[]> pieces_;  // capacity PHYSICAL_CAPACITY
  uint32_t count_ = 0;
  uint32_t totalLength_ = 0;

  // Forward-typing fast path: index of the most recently extended/created Edit piece,
  // and its cached logical end offset (avoids re-summing to find it). kNoHint when the
  // next insert can't take the fast path.
  static constexpr uint32_t kNoHint = UINT32_MAX;
  uint32_t lastEditPieceIndex_ = kNoHint;
  uint32_t lastEditPieceLogicalEnd_ = 0;

  struct Locate {
    uint32_t index;  // piece index containing pos, or count_ if pos == totalLength_
    uint32_t offsetInPiece;
    uint32_t pieceStart;  // logical start offset of pieces_[index]
  };
  Locate locate(uint32_t pos) const;

  // Shifts pieces_[from..count_) right by `n` slots (n == 1 or 2) and grows count_ by
  // n. Caller must have verified count_ + n <= MAX_PIECES.
  void makeRoom(uint32_t from, uint32_t n);
  // Shifts pieces_[from + n..count_) left by `n` slots and shrinks count_ by n.
  void closeGap(uint32_t from, uint32_t n);
};
