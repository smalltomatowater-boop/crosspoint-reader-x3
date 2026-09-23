#include "PieceTable.h"

#include <cstring>

PieceTable::PieceTable() : pieces_(new Piece[PHYSICAL_CAPACITY]) {}

void PieceTable::reset(Source source, uint32_t length) {
  count_ = 0;
  totalLength_ = 0;
  lastEditPieceIndex_ = kNoHint;
  if (length > 0) {
    pieces_[0] = {source, 0, length};
    count_ = 1;
    totalLength_ = length;
  }
}

PieceTable::Locate PieceTable::locate(uint32_t pos) const {
  uint32_t cursor = 0;
  for (uint32_t i = 0; i < count_; ++i) {
    const uint32_t pieceEnd = cursor + pieces_[i].length;
    if (pos < pieceEnd) {
      return {i, pos - cursor, cursor};
    }
    cursor = pieceEnd;
  }
  return {count_, 0, cursor};  // pos == totalLength_: append position
}

void PieceTable::makeRoom(uint32_t from, uint32_t n) {
  if (count_ > from) {
    memmove(&pieces_[from + n], &pieces_[from], (count_ - from) * sizeof(Piece));
  }
  count_ += n;
}

void PieceTable::closeGap(uint32_t from, uint32_t n) {
  if (count_ > from + n) {
    memmove(&pieces_[from], &pieces_[from + n], (count_ - from - n) * sizeof(Piece));
  }
  count_ -= n;
}

bool PieceTable::insert(uint32_t pos, Source source, uint32_t sourceOffset, uint32_t insertLength) {
  if (insertLength == 0) return true;
  if (pos > totalLength_) pos = totalLength_;

  // Fast path: extend the piece we most recently appended, if this insert continues
  // it contiguously. See the class comment for why this is the case that matters.
  if (lastEditPieceIndex_ != kNoHint && pos == lastEditPieceLogicalEnd_ && source == Source::Edit) {
    Piece& last = pieces_[lastEditPieceIndex_];
    if (last.offset + last.length == sourceOffset) {
      last.length += insertLength;
      totalLength_ += insertLength;
      lastEditPieceLogicalEnd_ += insertLength;
      return true;
    }
  }

  const Locate loc = locate(pos);

  if (loc.index == count_) {
    // Append at the true end of the document.
    if (count_ >= MAX_PIECES) return false;
    pieces_[count_] = {source, sourceOffset, insertLength};
    if (source == Source::Edit) {
      lastEditPieceIndex_ = count_;
      lastEditPieceLogicalEnd_ = totalLength_ + insertLength;
    } else {
      lastEditPieceIndex_ = kNoHint;
    }
    count_ += 1;
    totalLength_ += insertLength;
    return true;
  }

  if (loc.offsetInPiece == 0) {
    // Insert cleanly before pieces_[loc.index]; that piece (and whatever follows it)
    // slides right by one slot.
    if (count_ >= MAX_PIECES) return false;
    makeRoom(loc.index, 1);
    pieces_[loc.index] = {source, sourceOffset, insertLength};
  } else {
    // Strictly inside pieces_[loc.index]: split it, with the new piece in between.
    if (count_ + 2 > MAX_PIECES) return false;
    const Piece original = pieces_[loc.index];
    makeRoom(loc.index + 1, 2);
    pieces_[loc.index].length = loc.offsetInPiece;
    pieces_[loc.index + 1] = {source, sourceOffset, insertLength};
    pieces_[loc.index + 2] = {original.source, original.offset + loc.offsetInPiece,
                              original.length - loc.offsetInPiece};
  }

  // The spliced piece is always followed by at least one more piece in both branches
  // above, so it can never be the fast-path target next time.
  lastEditPieceIndex_ = kNoHint;
  totalLength_ += insertLength;
  return true;
}

void PieceTable::remove(uint32_t pos, uint32_t count) {
  lastEditPieceIndex_ = kNoHint;  // Indices may shift; the hint can't survive this.

  if (count == 0 || pos >= totalLength_) return;
  if (count > totalLength_ - pos) count = totalLength_ - pos;

  uint32_t remaining = count;
  const uint32_t currentPos = pos;  // Deliberately never advanced — see loop comment.

  while (remaining > 0) {
    const Locate loc = locate(currentPos);
    Piece& piece = pieces_[loc.index];
    const uint32_t available = piece.length - loc.offsetInPiece;
    const uint32_t take = (remaining < available) ? remaining : available;

    if (loc.offsetInPiece == 0 && take == piece.length) {
      closeGap(loc.index, 1);  // Whole piece consumed.
    } else if (loc.offsetInPiece == 0) {
      // take < piece.length here, so this is the last step (see remove()'s doc
      // comment) — shrink from the front and we're done.
      piece.offset += take;
      piece.length -= take;
    } else if (take == available) {
      // Truncate the tail; whatever piece follows now starts exactly at currentPos.
      piece.length = loc.offsetInPiece;
    } else {
      // Strictly inside the piece and not reaching its end: split into the kept left
      // half and a new right-remainder piece. take == remaining here (this is the
      // last step), and count_ <= MAX_PIECES on entry to remove(), so the one spare
      // physical slot always covers this +1.
      const Piece original = piece;
      makeRoom(loc.index + 1, 1);
      pieces_[loc.index].length = loc.offsetInPiece;
      pieces_[loc.index + 1] = {original.source, original.offset + loc.offsetInPiece + take,
                                original.length - loc.offsetInPiece - take};
    }

    totalLength_ -= take;
    remaining -= take;
    // currentPos is intentionally left unchanged: after shrinking/removing at this
    // spot, whatever used to come next has slid into the same logical position.
  }
}
