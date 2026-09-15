#ifndef MARKDOWNASTWALKER_H
#define MARKDOWNASTWALKER_H

#include <QByteArray>
#include <QHash>
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

// Split raw source without decoding escapes. Optional borders contain the leading
// pipe and each unescaped closing pipe: one more entry than cells on success.
bool splitTableRow(const QString &p_line, QString &p_prefix, QVector<QString> &p_cells,
                   QVector<int> *p_cellOffsets = nullptr, QVector<int> *p_cellBorders = nullptr);

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

// Value-only list projection. Container and item indexes belong to this snapshot.
struct ListContainerInfo {
  enum class Kind { Quote, Item, Indent };
  Kind m_kind = Kind::Item;
  int m_parent = -1;
  int m_item = -1;
  int m_startBlock = -1;
  int m_endBlock = -1;
  int m_markerOffset = 0;
  int m_padding = 0;
};

struct ListItemInfo {
  int m_list = -1;
  // This item's own Kind::Item container, whose parent encloses the LIST.
  int m_container = -1;
  int m_startBlock = -1;
  int m_endBlock = -1;
  // Absolute UTF-16, half-open marker bounds; the checkbox is not the marker.
  int m_markerStart = -1;
  int m_markerEnd = -1;
  int m_contentStart = -1;
  int m_sourceNumber = 0;
  QChar m_marker;
  bool m_task = false;
  bool m_empty = false;
  bool m_sourceValid = false;
  bool m_prefixValid = false;
  QString m_siblingPrefix;
};

struct ListInfo {
  int m_parentContainer = -1;
  int m_startNumber = 0;
  bool m_ordered = false;
  QChar m_marker;
  QVector<int> m_items;
};

struct ListParagraphInfo {
  int m_startBlock = -1;
  int m_endBlock = -1;
  int m_item = -1;
};

struct ListStructure {
  bool m_valid = false;
  // Implicitly shared original UTF-8, retained only when there are lists.
  QByteArray m_source;
  QVector<ListContainerInfo> m_containers;
  QVector<ListInfo> m_lists;
  // Source order, with deeper same-line markers after their ancestors.
  QVector<ListItemInfo> m_items;
  // Source-sorted, disjoint inclusive ranges, only paragraphs directly in ITEMs.
  QVector<ListParagraphInfo> m_paragraphs;
};

struct ListSourceEdit {
  int m_start = 0;
  int m_end = 0;
  QString m_before;
  QString m_after;
};

// Anchored, line-local lexical fields only; membership/verification stay unset.
bool scanListMarker(const QString &p_line, int p_start, ListItemInfo &p_marker);
ListStructure parseListStructure(const QByteArray &p_utf8Text, int p_offset = 0,
                                 int p_startBlock = 0);
// Constant-time access to the prefix verified and cached during projection.
bool listContinuationPrefix(const ListStructure &p_structure, int p_item, QString &p_prefix);
bool buildListNumberEdits(const QString &p_source, const ListStructure &p_structure,
                          const QHash<int, int> &p_listStarts, QVector<ListSourceEdit> &p_edits,
                          ListStructure &p_after);

struct ASTWalkResult {
  QVector<QVector<HLUnit>> blocksHighlights; // indexed by block number
  // Source-ordered, disjoint foreground overlays; only populated blocks are stored.
  QHash<int, QVector<HLUnitStyle>> blockOverlays; // keyed by global block number
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
  QVector<ConcealRange> concealRanges; // full parse only; raw URL payloads

  // Typed element data for interactive previews.
  QVector<ImageElement> imageElements;
  QVector<CodeElement> codeElements;
  QVector<MathElement> mathElements;
  QVector<TableElement> tableElements;
  ListStructure listStructure;

  // Headings with their AST-derived title and anchor text.
  // Sorted by start position.
  QVector<HeadingInfo> headingElements;
};

// Single-pass AST walker. Parses markdown with cmark, walks AST once,
// produces per-block HLUnits, sparse foreground overlays and region vectors directly.
// p_numBlocks: total blocks in document (sizes blocksHighlights vector)
// p_offset: QChar offset of text start in document (for region positions)
// p_startBlock: first block number of the sliced text (maps local line 0 -> global block
// p_startBlock) p_fast: if true, skip region collection (retain highlights and overlays)
// p_collectLists: collect the editing projection only for a non-fast walk
ASTWalkResult walkAndConvert(const QByteArray &p_utf8Text, int p_numBlocks, int p_offset = 0,
                             int p_startBlock = 0, bool p_fast = false,
                             bool p_collectLists = false);

// Project the walker's image elements onto what the highlighter publishes:
// region, destination and declared size. Order is preserved, one entry per
// element.
QVector<ImageLinkInfo> buildImageLinks(const QVector<ImageElement> &p_elements);

// Cell-local highlights and typed elements for one snippet of Markdown source.
//
// One-way and BEST EFFORT: the snippet is parsed as a whole document, so a
// payload that happens to parse as a block construct (`# x`) is highlighted as
// one. There is no way to ask cmark for inline-only parsing, and a cell's
// source is by definition detached from its surrounding block context, so this
// is the closest honest answer. Used by the live table document's cache for
// Markdown and Markdown-backed HTML cells; an HTML-only table gets no runs.
ASTWalkResult parseInlineSnippet(const QString &p_snippet);

// Total number of nonempty snippet parse attempts parseInlineSnippet() has
// performed since the last reset. Measures live-cell cache misses. Diagnostics
// only, and process wide - a benchmark drives one document at a time.
// Not thread safe: it is a counter, not a synchronization primitive.
quint64 inlineSnippetParseCount();

void resetInlineSnippetParseCount();
} // namespace md
} // namespace vte

#endif // MARKDOWNASTWALKER_H
