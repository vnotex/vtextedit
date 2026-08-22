#ifndef VTEXTEDIT_HTMLTABLESCANNER_H
#define VTEXTEDIT_HTMLTABLESCANNER_H

#include "htmlimgscanner.h"
#include "vtextedit_export.h"

#include <QPoint>
#include <QString>
#include <QVector>

namespace vte {

// One `<td>` / `<th>` of a scanned HTML table.
//
// Every span is ABSOLUTE: the base offset passed to scanHtmlTables() has
// already been added.
struct VTEXTEDIT_EXPORT HtmlTableCell {
  // Half-open span of the whole `<td …>` / `<th …>` OPENING tag. A rewriter
  // replaces an attribute inside this span attribute-locally (AGENTS.md D9);
  // it never regenerates the tag, which would destroy `class`, `style` and
  // anything else the author wrote.
  int m_tagStart = -1;
  int m_tagEnd = -1;

  // Half-open span of the cell's inner source: just past the opening tag's '>'
  // up to the '<' of its closing tag. Single-line by construction (D-i).
  int m_innerStart = -1;
  int m_innerEnd = -1;

  // The inner source, verbatim.
  QString m_inner;

  // `<th>` rather than `<td>`.
  bool m_header = false;

  // Resolved from `colspan` / `rowspan`; always >= 1.
  int m_colSpan = 1;
  int m_rowSpan = 1;

  // Lower-cased `align` value, empty when the attribute is absent.
  QString m_align;

  // ALL attributes of the opening tag, in source order, duplicates kept.
  QVector<HtmlAttr> m_attrs;

  // Whether the inner source begins with a `<!--vte-md:…-->` payload comment.
  bool m_hasPayload = false;

  // The DECODED payload, valid only when m_hasPayload && !m_payloadMalformed.
  QString m_payload;

  // A payload comment was present but its escape sequence did not decode. Per
  // decision D-n one such cell poisons the WHOLE table: classification is
  // table-wide, so a half-decoded table can never be written back.
  bool m_payloadMalformed = false;

  // The logical grid slot this cell OWNS (x = column, y = row).
  QPoint m_origin;
};

// One `<tr>` of a scanned HTML table.
struct VTEXTEDIT_EXPORT HtmlTableRow {
  // Half-open span of the `<tr …>` opening tag, kept verbatim for the same
  // reason as the cell tag.
  int m_tagStart = -1;
  int m_tagEnd = -1;

  QVector<HtmlAttr> m_attrs;

  QVector<HtmlTableCell> m_cells;
};

// One `<table>…</table>` in the canonical subset of decision D-i.
struct VTEXTEDIT_EXPORT HtmlTable {
  // Half-open span of the whole `<table …>…</table>` construct.
  int m_tableStart = -1;
  int m_tableEnd = -1;

  // Half-open span of the `<table …>` opening tag alone.
  int m_openTagStart = -1;
  int m_openTagEnd = -1;

  QVector<HtmlAttr> m_attrs;

  QVector<HtmlTableRow> m_rows;

  // The LOGICAL grid. m_rowCount == m_rows.size(); m_columnCount is the width
  // the `colspan`/`rowspan` grid tiles exactly -- the scanner refuses the table
  // otherwise, so these are always consistent.
  int m_rowCount = 0;
  int m_columnCount = 0;

  // Row-major, m_rowCount * m_columnCount entries. Each slot names the grid
  // coordinates (x = column, y = row) of the cell that owns it; a slot whose
  // entry equals its own coordinates is an ORIGIN.
  QVector<QPoint> m_originAt;

  // Row 0 is entirely `<th>` and every later row entirely `<td>`. When false
  // every row is entirely `<td>` -- D-i admits no other shape.
  bool m_hasHeaderRow = false;

  // Per-column alignment, empty when the column declares none. Size equals
  // m_columnCount.
  QVector<QString> m_alignments;

  // Any cell's payload comment failed to decode (D-n). The table must then be
  // treated as HTML-only regardless of how many other payloads were valid.
  bool m_anyPayloadMalformed = false;

  // At least one cell carried a well-formed payload. Together with
  // !m_anyPayloadMalformed this is decision D-j's "the table is Markdown-
  // backed": backing is per TABLE, never per cell, so no mutation ever has to
  // reconcile two kinds of cell.
  bool m_anyPayloadPresent = false;

  // The origin owning slot (@p_row, @p_column).
  QPoint originAt(int p_row, int p_column) const;

  // The cell owning slot (@p_row, @p_column), or nullptr when out of range.
  const HtmlTableCell *cellAt(int p_row, int p_column) const;
};

// Every canonical `<table>…</table>` in @p_text, with spans offset by
// @p_baseOffset.
//
// This is the ONLY place in the tree allowed to pattern-match `<table` in NOTE
// SOURCE, exactly as scanHtmlImgTags() is for `<img`. See AGENTS.md.
//
// The top-level lexer is deliberately identical to the `<img>` scanner's, down
// to visiting every tag inside an accepted table rather than jumping over it:
// the two scanners run over the same slice with independent copies of
// @p_state, and the walker ASSERTS that they agree on the outgoing state. Any
// divergence in comment or raw-text handling would break that.
//
// A table is captured only when EVERY condition of decision D-i holds:
// - no `<caption>`, `<colgroup>`, `<thead>`, `<tbody>`, `<tfoot>`, `<col>`;
// - no nested `<table>` -- and a table whose parse was refused suppresses any
//   table nested inside it too, so a refusal never degrades into capturing the
//   inner one;
// - every tag opens and closes within one line, and every cell's inner source
//   lies on one line;
// - all tags explicitly balanced; no implicit `</tr>` / `</td>`;
// - either row 0 is entirely `<th>` and every later row entirely `<td>`, or
//   every row is entirely `<td>`;
// - each column has at most one distinct `align` value among the cells that
//   declare one;
// - the `colspan`/`rowspan` grid tiles a rectangle EXACTLY: no gap, no
//   overlap, no overflow past the last row or column.
// Anything else is dropped whole, and the block then renders as plain source.
//
// @p_state may be null, in which case no raw-text context is carried.
VTEXTEDIT_EXPORT QVector<HtmlTable> scanHtmlTables(const QString &p_text, int p_baseOffset,
                                                   RawTextState *p_state);

// The prefix of a cell's inner source that introduces its Markdown payload.
VTEXTEDIT_EXPORT extern const char *const c_vteMarkdownPayloadPrefix;

// Encode a cell's Markdown source for transport inside an HTML comment
// (decision D-b): `\` -> `\\`, then `-` -> `\-`.
//
// Every encoded `-` is preceded by a backslash, so `--` -- and therefore the
// comment terminator `-->` -- is unrepresentable in the output. The map is
// injective, so the round trip is lossless for any input whatsoever.
VTEXTEDIT_EXPORT QString escapePayload(const QString &p_text);

// Inverse of escapePayload(). Returns false, leaving @p_out untouched, for a
// MALFORMED sequence: a dangling trailing `\`, or a `\` followed by anything
// but `\` or `-`. Malformed input is a hard failure and never a best-effort
// decode -- per D-n it makes the whole table HTML-only, which is the only
// interpretation that cannot silently corrupt a cell on write-back.
VTEXTEDIT_EXPORT bool unescapePayload(const QString &p_text, QString &p_out);

// Rewrite one attribute inside the VERBATIM opening tag @p_tag,
// ATTRIBUTE-LOCALLY: replace the whole `name="value"` when it is present,
// append one just before the closing `>` when it is absent, and remove it when
// @p_value is null.
//
// This is AGENTS.md D9 applied to a table tag. Regenerating the tag instead
// would silently destroy `class`, `style`, `data-*` and anything else the
// author wrote. The WHOLE attribute is replaced, never just its value, for the
// same reason spellHtmlSrcAttr() exists: an unquoted `colspan=2` rewritten to a
// value containing a space would otherwise split into two attributes.
//
// Returns @p_tag unchanged when it cannot be parsed, which the caller must
// treat as a refusal rather than as success.
VTEXTEDIT_EXPORT QString rewriteHtmlTagAttr(const QString &p_tag, const QString &p_name,
                                            const QString &p_value);


} // namespace vte

#endif // VTEXTEDIT_HTMLTABLESCANNER_H
