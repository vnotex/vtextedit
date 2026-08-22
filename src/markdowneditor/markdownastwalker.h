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

  // Declared size from the `=WxH` extension. 0 means unspecified for that axis.
  int m_width = 0;
  int m_height = 0;

  // Whether the image is the sole content of its source line.
  bool m_standalone = false;

  // How the image is spelled in the source: a Markdown `![…](…)` link, or an
  // HTML `<img …>` tag found inside an HTML_INLINE / HTML_BLOCK node.
  ImageLinkInfo::Syntax m_syntax = ImageLinkInfo::Syntax::Markdown;
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

  // --- HTML syntax only; empty for a Markdown table. ---

  // Column span of each cell in the logical grid. Parallel to m_cells.
  QVector<int> m_colSpans;

  // Row span of each cell in the logical grid. Parallel to m_cells.
  QVector<int> m_rowSpans;

  // The grid COLUMN each cell originates at. Parallel to m_cells. Not derivable
  // from the index, because a cell covered by a rowspan above shifts every
  // later cell of the row rightwards.
  QVector<int> m_slotColumns;

  // The verbatim `<td …>` / `<th …>` opening tag of each cell, exactly as
  // authored. Parallel to m_cells. Kept so a rewrite can be attribute-local and
  // never regenerate a tag it did not author (AGENTS.md D9, decision D-g).
  QVector<QString> m_cellTags;

  // The verbatim `<tr …>` opening tag of this row.
  QString m_rowTag;
};

struct TableElement : public TypedPreviewElement {
  // How the table is spelled in the source.
  enum class Syntax {
    // A GFM pipe table.
    Markdown,

    // A top-level `<table>` HTML block in the canonical subset of decision
    // D-i. Never a table under a container prefix and never one nested inside
    // another element (D-a).
    Html
  };

  Syntax m_syntax = Syntax::Markdown;

  // Decision D-j: backing is per TABLE, never per cell. True when the cells'
  // text is Markdown source -- always so for the Markdown syntax, and for an
  // HTML table when at least one cell carried a well-formed `<!--vte-md:-->`
  // payload and none was malformed (D-n). When false the cells hold literal
  // HTML text, shown and written back verbatim with no comment ever
  // synthesized (D-d).
  bool m_markdownBacked = true;

  // Global block number of the table's first (header) row. MARKDOWN ONLY: an
  // HTML table has no one-source-line-per-row correspondence, so this is -1 and
  // every consumer that indexes m_startBlock + row must skip it.
  int m_startBlock = -1;

  // Column count declared by the header/delimiter rows.
  int m_columns = 0;

  // The LOGICAL grid, which is always rectangular and always tiles exactly.
  // For a Markdown table every slot is a 1x1 origin.
  int m_rowCount = 0;
  int m_columnCount = 0;

  // Whether row 0 is a header row. Always true for the Markdown syntax; for
  // HTML it is true iff row 0 is entirely `<th>` (D-i).
  bool m_hasHeaderRow = true;

  // The verbatim `<table …>` opening tag, empty for the Markdown syntax.
  QString m_openTag;

  // Per-column alignment, matching cmark_table_align ordinals.
  // 0 none, 1 left, 2 center, 3 right.
  QVector<int> m_alignments;

  // Rows in source order. For the Markdown syntax index 1 is always the
  // delimiter row; an HTML table emits no delimiter row at all.
  QVector<TableRowElement> m_rows;
};

struct ASTWalkResult {
  QVector<QVector<HLUnit>> blocksHighlights; // indexed by block number
  // NOT the editor's image channel. Nothing in production reads this any more:
  // the highlighter publishes md::ImageLinkInfo built from imageElements, which
  // also carries the destination and the declared `=WxH` size. This survives
  // only as parser-level test surface (and via MarkdownParseResult, whose
  // parseImageRegions() likewise has no caller). Use imageElements.
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

  // Headings with their AST-derived title and anchor text.
  // Sorted by start position.
  QVector<HeadingInfo> headingElements;
};

// Single-pass AST walker. Parses markdown with cmark, walks AST once,
// produces per-block HLUnits and region vectors directly.
// p_numBlocks: total blocks in document (sizes blocksHighlights vector)
// p_offset: QChar offset of text start in document (for region positions)
// p_startBlock: first block number of the sliced text (maps local line 0 -> global block
// p_startBlock) p_fast: if true, skip region collection (only produce blocksHighlights)
ASTWalkResult walkAndConvert(const QByteArray &p_utf8Text, int p_numBlocks, int p_offset = 0,
                             int p_startBlock = 0, bool p_fast = false);

// Project the walker's image elements onto what the highlighter publishes:
// region, destination and declared size. Order is preserved, one entry per
// element.
QVector<ImageLinkInfo> buildImageLinks(const QVector<ImageElement> &p_elements);

// Cell-local highlight units for one snippet of Markdown source.
//
// One-way and BEST EFFORT: the snippet is parsed as a whole document, so a
// payload that happens to parse as a block construct (`# x`) is highlighted as
// one. There is no way to ask cmark for inline-only parsing, and a cell's
// source is by definition detached from its surrounding block context, so this
// is the closest honest answer. Used only for the Markdown-backed cells of an
// HTML table; a pipe table's cells are sliced out of their own source line by
// sliceTableCellHighlights() instead, and an HTML-only table gets no runs.
QVector<HLUnit> highlightInlineSnippet(const QString &p_snippet);


} // namespace md
} // namespace vte

#endif // MARKDOWNASTWALKER_H
