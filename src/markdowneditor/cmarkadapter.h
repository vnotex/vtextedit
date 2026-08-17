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

  // Return the QChar offset of the end of the given 0-indexed line, excluding
  // the line terminator ("\n" or "\r\n"). Used to build source ranges which
  // exclude the paragraph separator.
  int lineEndQCharOffset(int p_lineIdx) const;

  // Return the byte range [p_start, p_start + p_length) of the given 0-indexed
  // line within the original UTF-8 buffer, excluding the line terminator.
  // Returns false when the line does not exist.
  bool lineByteRange(int p_lineIdx, int &p_start, int &p_length) const;

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

// Half-open QChar span [p_startQChar, p_endQChar) of p_node, derived from
// cmark's reported source coordinates and corrected for cmark's block_offset
// accounting on continuation lines. Returns false when the node carries no
// source position, in which case the out-params are untouched.
//
// This is the single implementation of cmark-coordinates-to-document-offset
// mapping: the walker uses it to place highlights and previews, and the
// snapshot API uses it to locate images in raw text. Two implementations
// previously disagreed.
bool cmarkNodeSpan(cmark_node *p_node, const LineOffsetTable &p_offsets, int &p_startQChar,
                   int &p_endQChar);

// As cmarkNodeSpan(), but for the *raw* destination of an inline link or image
// -- the bytes as spelled in the source, so angle brackets, backslash escapes
// and entities are all still present. Returns false for reference-style links
// and for an empty destination, neither of which spans any source bytes.
bool cmarkNodeUrlSpan(cmark_node *p_node, const LineOffsetTable &p_offsets, int &p_startQChar,
                      int &p_endQChar);

#endif
