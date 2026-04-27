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

#endif
