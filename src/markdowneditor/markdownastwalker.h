#ifndef MARKDOWNASTWALKER_H
#define MARKDOWNASTWALKER_H

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVector>

#include <vtextedit/markdownhighlighterdata.h>

namespace vte {
namespace md {

// Typed data of one parsed element, captured while the cmark AST and the
// original input are still alive. Positions are absolute UTF-16 document
// offsets and half open.
struct TypedPreviewElement {
  int m_startPos = 0;
  int m_endPos = 0;
};

struct ImageElement : public TypedPreviewElement {
  QString m_destination;
  QString m_alternateText;
  QString m_title;

  // Whether the image is the sole content of its source line.
  bool m_standalone = false;
};

struct CodeElement : public TypedPreviewElement {
  QString m_language;
  QString m_code;
};

struct MathElement : public TypedPreviewElement {
  QString m_expression;
  bool m_display = true;
};

enum class TableRowType { Header, Delimiter, Data };

struct TableRowElement {
  TableRowType m_type = TableRowType::Data;

  // Block container prefix preceding the leading pipe.
  QString m_prefix;

  // Raw Markdown of each cell, trimmed of the framing whitespace only.
  QVector<QString> m_cells;

  // Offset within the source line of each trimmed cell's first character.
  // Parallel to m_cells.
  QVector<int> m_cellOffsets;

  // Highlight units of each cell, in cell-local coordinates. Parallel to
  // m_cells; an empty entry means the cell carries no inline highlighting.
  QVector<QVector<HLUnit>> m_cellHighlights;
};

struct TableElement : public TypedPreviewElement {
  // Global block number of the table's first (header) row.
  int m_startBlock = -1;

  // Column count declared by the header/delimiter rows.
  int m_columns = 0;

  // Per-column alignment, matching cmark_table_align ordinals.
  // 0 none, 1 left, 2 center, 3 right.
  QVector<int> m_alignments;

  // Rows in source order. Index 1 is always the delimiter row.
  QVector<TableRowElement> m_rows;
};

struct ASTWalkResult {
  QVector<QVector<HLUnit>> blocksHighlights; // indexed by block number
  QVector<ElementRegion> imageRegions;
  QVector<ElementRegion> headerRegions;
  QMap<int, ElementRegion> codeBlockRegions;
  QVector<ElementRegion> inlineEquationRegions;
  QVector<ElementRegion> displayFormulaRegions;
  QVector<ElementRegion> hruleRegions;
  QVector<ElementRegion> tableRegions;
  QVector<ElementRegion> tableHeaderRegions;
  QVector<ElementRegion> tableBorderRegions;
  QVector<FoldingRegion> foldingRegions;

  // Typed element data for interactive previews.
  QVector<ImageElement> imageElements;
  QVector<CodeElement> codeElements;
  QVector<MathElement> mathElements;
  QVector<TableElement> tableElements;
};


// Single-pass AST walker. Parses markdown with cmark, walks AST once,
// produces per-block HLUnits and region vectors directly.
// p_numBlocks: total blocks in document (sizes blocksHighlights vector)
// p_offset: QChar offset of text start in document (for region positions)
// p_startBlock: first block number of the sliced text (maps local line 0 -> global block p_startBlock)
// p_fast: if true, skip region collection (only produce blocksHighlights)
ASTWalkResult walkAndConvert(const QByteArray &p_utf8Text, int p_numBlocks,
                             int p_offset = 0, int p_startBlock = 0, bool p_fast = false);

} // namespace md
} // namespace vte

#endif // MARKDOWNASTWALKER_H
