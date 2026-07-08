#ifndef CMARKADAPTER_H
#define CMARKADAPTER_H

#include <QByteArray>
#include <QVector>

#include <cmark.h>

class LineOffsetTable {
public:
  explicit LineOffsetTable(const QByteArray &p_utf8Text);

  // Convert cmark 1-indexed line:col (col = byte offset within line)
  // to document-absolute QChar offset.
  int toDocPosition(int p_line, int p_col) const;

  // Return the number of QChars occupied by the character whose LAST byte is at
  // the given cmark 1-indexed byte column (line = 1-indexed). This is 2 for a
  // 4-byte UTF-8 char (astral / surrogate pair) and 1 otherwise. Returns 1 when
  // the column is past the line's content, so callers converting an inclusive
  // end column to an exclusive QChar end preserve their existing behavior there.
  int qcharWidthAtEndColumn(int p_line, int p_col) const;

  // Return the QChar offset of the start of the given 0-indexed line.
  int lineStartQCharOffset(int p_lineIdx) const;

  // Return the number of lines in the table.
  int lineCount() const;

  // Return the count of leading space (0x20) bytes at the start of the given 0-indexed line.
  int lineLeadingSpaces(int p_lineIdx) const;

  // Return the width (in columns/bytes) of the block-container prefix cmark
  // strips/skips from the start of the given 0-indexed line: the leading run of
  // spaces, followed by block-quote '>' markers that BEGIN no further than
  // p_blockOffset columns into the line (each marker also absorbing the spaces
  // after it). A '>' that begins beyond p_blockOffset is content (e.g. an
  // over-indented ">" inside a list item), not a stripped marker, so it is
  // excluded. cmark removes this prefix at the block level and skips remaining
  // leading spaces during inline parsing, then folds the paragraph's first-line
  // prefix width into block_offset; subtracting the per-line prefix here undoes
  // cmark's over/under-reporting of inline columns on continuation lines. For a
  // pure-space line (no countable '>') the result equals lineLeadingSpaces(), so
  // list-item continuation behavior (including over-indentation) is unchanged; '>'
  // markers (invisible to lineLeadingSpaces) are what block quotes need.
  // p_blockOffset is the paragraph's block_offset (start_column - 1). (Deeply
  // indented continuation markers beyond block_offset — e.g. a block quote nested
  // in a list with extra indent — are approximated; a fully faithful result would
  // require replicating cmark's per-container prefix walk.)
  int lineStrippedPrefixWidth(int p_lineIdx, int p_blockOffset) const;

private:
  // Per-line: byte offset of line start in the full UTF-8 buffer.
  QVector<int> m_lineByteOffsets;

  // Per-line: cumulative QChar offset at line start.
  QVector<int> m_lineQCharOffsets;

  // Per-line: mapping from byte offset within line to QChar count.
  // m_byteToQChar[lineIdx] is a vector where index = byte offset within line,
  // value = cumulative QChar count at that byte.
  QVector<QVector<int>> m_byteToQChar;

  const unsigned char *m_data = nullptr;
  int m_dataLen = 0;
};

// Map a cmark node type to a MarkdownSyntaxStyle ordinal (matching pmh_element_type).
// Returns -1 if the node type should be skipped.
int mapCmarkNodeToStyle(cmark_node_type p_type, cmark_node *p_node);

#endif
