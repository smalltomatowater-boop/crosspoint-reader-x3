#include <gtest/gtest.h>

#include <random>
#include <string>

#include "PieceTable.h"

namespace {

// Mirrors how EditorDocument actually drives PieceTable: `base` is the fixed,
// immutable backing text (Source::Base), `edit` is an append-only stream that every
// insertAt() call appends new bytes to before splicing (Source::Edit). materialize()
// walks the table with visitRange() and reconstructs the text those pieces describe,
// so tests can compare it against an independent std::string reference.
struct Doc {
  PieceTable table;
  std::string base;
  std::string edit;

  explicit Doc(std::string baseText) : base(std::move(baseText)) {
    table.reset(PieceTable::Source::Base, static_cast<uint32_t>(base.size()));
  }

  void insertAt(uint32_t pos, const std::string& text) {
    const uint32_t sourceOffset = static_cast<uint32_t>(edit.size());
    edit += text;
    ASSERT_TRUE(table.insert(pos, PieceTable::Source::Edit, sourceOffset, static_cast<uint32_t>(text.size())));
  }

  void removeAt(uint32_t pos, uint32_t count) { table.remove(pos, count); }

  std::string materialize() const {
    std::string out;
    table.visitRange(0, table.length(), [&](PieceTable::Source source, uint32_t offset, uint32_t length) {
      const std::string& store = (source == PieceTable::Source::Base) ? base : edit;
      out.append(store, offset, length);
    });
    return out;
  }
};

}  // namespace

TEST(PieceTable, EmptyReset) {
  PieceTable table;
  table.reset(PieceTable::Source::Base, 0);
  EXPECT_EQ(table.length(), 0u);
  EXPECT_EQ(table.pieceCount(), 0u);
}

TEST(PieceTable, ResetWithBaseContent) {
  Doc doc("hello world");
  EXPECT_EQ(doc.table.length(), 11u);
  EXPECT_EQ(doc.table.pieceCount(), 1u);
  EXPECT_EQ(doc.materialize(), "hello world");
}

TEST(PieceTable, AppendAtEndStaysOnePiece) {
  Doc doc("");
  doc.insertAt(0, "h");
  doc.insertAt(1, "e");
  doc.insertAt(2, "l");
  doc.insertAt(3, "l");
  doc.insertAt(4, "o");
  EXPECT_EQ(doc.materialize(), "hello");
  // Forward typing must coalesce into a single piece via the fast path.
  EXPECT_EQ(doc.table.pieceCount(), 1u);
}

TEST(PieceTable, InsertAtStartOfNonEmptyBase) {
  Doc doc("world");
  doc.insertAt(0, "hello ");
  EXPECT_EQ(doc.materialize(), "hello world");
  // Clean boundary insert before piece 0: no split needed, 2 pieces.
  EXPECT_EQ(doc.table.pieceCount(), 2u);
}

TEST(PieceTable, InsertInMiddleSplitsPiece) {
  Doc doc("helloworld");
  doc.insertAt(5, " ");
  EXPECT_EQ(doc.materialize(), "hello world");
  // Split into [hello][ ][world]: 3 pieces.
  EXPECT_EQ(doc.table.pieceCount(), 3u);
}

TEST(PieceTable, JumpingAroundCreatesSeparatePieces) {
  Doc doc("");
  doc.insertAt(0, "abc");
  EXPECT_EQ(doc.table.pieceCount(), 1u);
  doc.insertAt(0, "X");  // Jump to the start, breaking the fast-path contiguity.
  EXPECT_EQ(doc.materialize(), "Xabc");
  EXPECT_EQ(doc.table.pieceCount(), 2u);
  // The jump-insert above spliced "X" in via the insert-before path, which always
  // drops the fast-path hint (that piece is never the document's last one once
  // something follows it) — so appending "!" at the new true end can't coalesce with
  // anything and must splice a third piece, even though it's a plain forward append.
  doc.insertAt(4, "!");
  EXPECT_EQ(doc.materialize(), "Xabc!");
  EXPECT_EQ(doc.table.pieceCount(), 3u);
}

TEST(PieceTable, RemoveWholePiece) {
  Doc doc("world");
  doc.insertAt(0, "hello ");  // 2 pieces: [hello ][world]
  doc.removeAt(0, 6);         // Remove the whole first piece.
  EXPECT_EQ(doc.materialize(), "world");
  EXPECT_EQ(doc.table.pieceCount(), 1u);
}

TEST(PieceTable, RemoveShrinksFromFront) {
  Doc doc("hello world");
  doc.removeAt(0, 6);
  EXPECT_EQ(doc.materialize(), "world");
  EXPECT_EQ(doc.table.pieceCount(), 1u);
}

TEST(PieceTable, RemoveTruncatesTail) {
  Doc doc("hello world");
  doc.removeAt(5, 6);  // Remove " world" (everything after "hello").
  EXPECT_EQ(doc.materialize(), "hello");
  EXPECT_EQ(doc.table.pieceCount(), 1u);
}

TEST(PieceTable, RemoveStrictlyInsideSplits) {
  Doc doc("hello world");
  doc.removeAt(5, 1);  // Remove just the space.
  EXPECT_EQ(doc.materialize(), "helloworld");
  EXPECT_EQ(doc.table.pieceCount(), 2u);
}

TEST(PieceTable, RemoveSpanningMultiplePieces) {
  Doc doc("world");
  doc.insertAt(0, "hello ");  // [hello ][world]
  doc.insertAt(11, "!");      // append at the true end -> [hello ][world][!]
  doc.removeAt(3, 6);         // removes "lo wor", straddling the first two pieces
  EXPECT_EQ(doc.materialize(), "helld!");
}

TEST(PieceTable, RemoveClampsAtDocumentEnd) {
  Doc doc("hello");
  doc.removeAt(2, 100);  // Way more than remains.
  EXPECT_EQ(doc.materialize(), "he");
  EXPECT_EQ(doc.table.length(), 2u);
}

TEST(PieceTable, RemovePastEndIsNoop) {
  Doc doc("hello");
  doc.removeAt(10, 5);
  EXPECT_EQ(doc.materialize(), "hello");
}

TEST(PieceTable, BackspaceRightAfterTypingTruncatesInPlace) {
  Doc doc("");
  doc.insertAt(0, "hell");
  doc.insertAt(4, "x");  // "hellx" — typo, still one piece via the fast path
  doc.removeAt(4, 1);    // backspace it from the tail: truncates in place, no split
  EXPECT_EQ(doc.materialize(), "hell");
  EXPECT_EQ(doc.table.pieceCount(), 1u);

  // Retyping now splices a second piece rather than re-extending the first. This is
  // an unavoidable consequence of the edit store being append-only: the removed 'x'
  // still physically occupies the store's true tail even though no piece references
  // it anymore, so the next insert's bytes land further along and can't be treated as
  // contiguous with the truncated piece. Content correctness is unaffected — only the
  // piece count is one higher than the theoretical minimum.
  doc.insertAt(4, "o");
  EXPECT_EQ(doc.materialize(), "hello");
  EXPECT_EQ(doc.table.pieceCount(), 2u);
}

TEST(PieceTable, InsertClampsPastEnd) {
  Doc doc("hi");
  doc.insertAt(999, "!");
  EXPECT_EQ(doc.materialize(), "hi!");
}

TEST(PieceTable, InsertRefusesAtCapacityAndTableStaysValid) {
  Doc doc("");
  // Force one non-coalescing (jump-around) insert per iteration so every call adds a
  // real piece instead of taking the fast path, until the table is exactly full.
  uint32_t pos = 0;
  for (uint32_t i = 0; i < PieceTable::MAX_PIECES; ++i) {
    doc.insertAt(pos, "a");
    pos = 0;  // Always jump back to the start so the next insert can't coalesce.
  }
  EXPECT_EQ(doc.table.pieceCount(), PieceTable::MAX_PIECES);
  EXPECT_TRUE(doc.table.atCapacity());

  const uint32_t sourceOffset = static_cast<uint32_t>(doc.edit.size());
  doc.edit += "z";
  EXPECT_FALSE(doc.table.insert(0, PieceTable::Source::Edit, sourceOffset, 1));
  // Refusal must leave the table exactly as it was.
  EXPECT_EQ(doc.table.pieceCount(), PieceTable::MAX_PIECES);
  EXPECT_EQ(doc.table.length(), PieceTable::MAX_PIECES);
}

TEST(PieceTable, NeedsFlattenWatermark) {
  Doc doc("");
  uint32_t pos = 0;
  for (uint32_t i = 0; i <= PieceTable::FLATTEN_WATERMARK; ++i) {
    doc.insertAt(pos, "a");
    pos = 0;
  }
  EXPECT_TRUE(doc.table.needsFlatten());
}

TEST(PieceTable, VisitRangeClipsToRequestedWindow) {
  Doc doc("hello world");
  std::string collected;
  doc.table.visitRange(3, 5, [&](PieceTable::Source source, uint32_t offset, uint32_t length) {
    ASSERT_EQ(source, PieceTable::Source::Base);
    collected += doc.base.substr(offset, length);
  });
  EXPECT_EQ(collected, "lo wo");
}

TEST(PieceTable, RandomizedInsertRemoveAgreesWithStringReference) {
  std::mt19937 rng(42);
  std::string reference;
  Doc doc("");

  for (int step = 0; step < 500; ++step) {
    const bool doInsert = reference.empty() || (rng() % 3 != 0);
    if (doInsert) {
      const uint32_t pos = reference.empty() ? 0 : static_cast<uint32_t>(rng() % (reference.size() + 1));
      const int len = 1 + static_cast<int>(rng() % 5);
      std::string text;
      for (int i = 0; i < len; ++i) text += static_cast<char>('a' + (rng() % 26));

      reference.insert(pos, text);
      doc.insertAt(pos, text);
    } else {
      const uint32_t pos = static_cast<uint32_t>(rng() % reference.size());
      const uint32_t maxLen = static_cast<uint32_t>(reference.size() - pos);
      const uint32_t len = 1 + static_cast<uint32_t>(rng() % maxLen);

      reference.erase(pos, len);
      doc.removeAt(pos, len);
    }

    ASSERT_EQ(doc.materialize(), reference) << "diverged at step " << step;
    ASSERT_EQ(doc.table.length(), reference.size()) << "diverged at step " << step;
  }
}
