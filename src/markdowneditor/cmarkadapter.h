#ifndef CMARKADAPTER_H
#define CMARKADAPTER_H

#include <QByteArray>
#include <QVector>

#include <cmark.h>

struct HighlightElement;

class LineOffsetTable {
public:
  explicit LineOffsetTable(const QByteArray &p_utf8Text);

  // Convert cmark 1-indexed line:col (col = byte offset within line)
  // to document-absolute QChar offset.
  int toDocPosition(int p_line, int p_col) const;

  // Return the QChar offset of the start of the given 0-indexed line.
  int lineStartQCharOffset(int p_lineIdx) const;

  // Return the number of lines in the table.
  int lineCount() const;

private:
  // Per-line: byte offset of line start in the full UTF-8 buffer.
  QVector<int> m_lineByteOffsets;

  // Per-line: cumulative QChar offset at line start.
  QVector<int> m_lineQCharOffsets;

  // Per-line: mapping from byte offset within line to QChar count.
  // m_byteToQChar[lineIdx] is a vector where index = byte offset within line,
  // value = cumulative QChar count at that byte.
  QVector<QVector<int>> m_byteToQChar;
};

// Map a cmark node type to a MarkdownSyntaxStyle ordinal (matching pmh_element_type).
// Returns -1 if the node type should be skipped.
int mapCmarkNodeToStyle(cmark_node_type p_type, cmark_node *p_node);

// Walk the cmark AST rooted at p_doc, converting positions via p_offsets,
// and fill the p_result array (indexed by style ordinal) with linked lists
// of HighlightElement.
// p_result must be a pre-allocated array of size p_numTypes, initialized to nullptr.
void walkCmarkTree(cmark_node *p_doc,
                   const LineOffsetTable &p_offsets,
                   HighlightElement **p_result,
                   int p_numTypes);

// Free all elements in the p_result array and the array itself.
void freeHighlightElements(HighlightElement **p_result, int p_numTypes);

// Parse markdown text with cmark, returning HighlightElement** array.
// Caller must free with freeHighlightElements().
// Returns nullptr if cancelled or empty input.
HighlightElement **parseCmark(const QByteArray &p_utf8Text);

#endif
