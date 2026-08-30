#ifndef TABLEPREVIEWWIDGET_H
#define TABLEPREVIEWWIDGET_H

#include <QMetaType>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QScopedPointer>
#include <QSize>
#include <QString>
#include <QTextEdit>
#include <QVector>

#include <vtextedit/preview.h>
#include <vtextedit/previewwidget.h>
#include <vtextedit/vtextedit.h>

class QMenu;
class QTextDocument;
class QTextTable;
class QTimer;

namespace vte {
class InputModeStatusWidget;
class TablePreviewInputMode;

// Total number of table sheet grid cells constructed by
// TablePreviewDocument::build() since the last reset. Diagnostics only.
//
// Process wide, and deliberately so: the count originates deep inside the
// document builder, which has no back pointer to the host that would publish
// it, and threading one down purely for a counter would put a diagnostics
// concern into the sheet's ownership graph. InteractivePreviewHost mirrors the
// value into its c_tableCellsBuiltProperty, and the benchmark that reads it
// drives a single editor at a time.
quint64 tablePreviewCellsBuilt();

void resetTablePreviewCellsBuilt();

// Why a sheet is handing the caret back to the editor, and where the caret
// should land when it gets there.
//
// The destination is deliberately not a document position: a sheet only knows
// the snapshot coordinates of its own source, and those go stale on any
// unrelated edit. Only the host holds a live anchor, so it resolves the
// position at delivery time.
enum class FocusEscapeDirection {
  // The editor takes the focus back and the caret stays where it was.
  //
  // No longer produced by the built-in sheet: Escape used to mean this, and
  // decision D3 gave Escape to the input mode instead - it is how Vi's insert
  // and visual modes are left, and KateVi::NormalViMode answers it
  // unconditionally. Kept because it is the only direction that carries "do
  // not move the caret", which a third-party preview widget may still want.
  Keep,

  // Up out of the first row. The Markdown source is rendered above the sheet
  // (PreviewPlacement::BlockAfterSource), so "up" is the end of the source.
  Up,

  // Down out of the last row, i.e. just past the source's own line.
  Down
};

// Canonical Markdown serialization of an editable table sheet.
//
// The dialect targeted here is the one this cmark fork accepts: both the
// leading and the trailing pipe are mandatory.
//
// The canonical form is compact: every cell is emitted with exactly one space
// inside each border and columns are never padded to a common width, so a
// single long cell cannot amplify the source by the row count.
//
// Padding is available as an OPT-IN (@p_align): the cells of a column are then
// padded to its widest entry - measured in display columns, so a CJK character
// counts 2 and a combining mark 0 - and the delimiter row's dashes are grown
// to match, with the `:` markers still at the edges. Text placement follows the
// column's alignment. A table with a column wider than 200 display columns
// falls back to the compact form, which is the reason the compact contract
// exists in the first place.
class TablePreviewSerializer {
public:
  // Escape a '|' only when it is preceded by an even number of backslashes.
  // Idempotent: already escaped pipes are left untouched.
  static QString escapeCell(const QString &p_cell);

  // Whether the row prefixes can be reproduced without corrupting nesting.
  // The first row may carry a list marker; every following row (including the
  // delimiter row) must share one identical whitespace/block-quote prefix.
  static bool arePrefixesSafe(const QVector<QString> &p_rowPrefixes,
                              const QString &p_delimiterPrefix);

  // Returns an empty string when the table cannot be serialized safely.
  //
  // @p_align opts into the padded, column-aligned form. It defaults to false,
  // so the compact form stays the contract for every caller which does not ask
  // for anything else. Note that the ceiling fallback is NOT this function's
  // failure channel: it emits compact output rather than an empty string.
  static QString serialize(const QVector<QVector<QString>> &p_cells,
                           const QVector<PreviewTableAlignment> &p_alignments,
                           const QVector<QString> &p_rowPrefixes, const QString &p_delimiterPrefix,
                           bool p_align = false);
};

// HTML serialization of an editable table sheet, for the tables GFM pipe
// syntax cannot express.
//
// A table is written back as HTML when it was read as HTML, or as soon as any
// cell spans more than one row or column: `colspan` and `rowspan` have no pipe
// spelling at all. The conversion is one-way (decision D-e).
//
// The output must satisfy the SAME canonical subset the scanner enforces
// (decision D-i), because the host validates a write-back by re-parsing it: a
// commit whose output the scanner then refuses is unrecoverable in place -- the
// source is replaced, the re-parse finds no table, and the user is left with
// raw HTML and no sheet. Every path therefore fails CLOSED, returning an empty
// string, which flushPendingCommit() already reads as a rejection.
class TablePreviewHtmlSerializer {
public:
  // Render one cell's Markdown source to the HTML that follows its payload
  // comment, guaranteed to be a SINGLE LINE.
  //
  // The guarantee is not cosmetic. cmark terminates every block with a newline,
  // and one line of Markdown can render to multi-line HTML: `- x` becomes a
  // `<ul>`/`<li>` block, `# x` a block plus a newline, four leading spaces a
  // `<pre><code>x\n</code></pre>`. A multi-line cell fails the scanner's
  // one-line rule and the whole table stops previewing.
  //
  // The rule: strip the wrapping `<p>…</p>` when the render is a single
  // paragraph, strip trailing newlines, then encode every REMAINING line
  // separator as the character reference `&#10;` -- never as a space. `&#10;`
  // is whitespace-collapsed exactly like a newline outside `<pre>` and
  // preserved exactly like one inside it, so the rendered result is unchanged
  // in both contexts; a literal space would silently rewrite `x\n` to `x `
  // inside preformatted text.
  //
  // Returns an empty string when the result is still not single-line.
  static QString renderCellHtml(const QString &p_markdown);

  // `<!--vte-md:PAYLOAD-->` for @p_text (decision D-b).
  static QString payloadComment(const QString &p_text);

  // Serialize the logical grid.
  //
  // @p_cells is row major over the grid, empty at a covered slot. @p_spans
  // carries (colSpan, rowSpan) at each origin and (0, 0) at a covered slot.
  // @p_cellTags and @p_rowTags hold the verbatim source tags, empty for
  // anything this sheet generated.
  //
  // Returns an empty string for anything the Markdown serializer also refuses:
  // no cells, zero width, or a line separator inside a cell.
  static QString serialize(const QVector<QVector<QString>> &p_cells,
                           const QVector<QVector<QPoint>> &p_spans,
                           const QVector<QVector<QString>> &p_cellTags,
                           const QVector<QString> &p_rowTags, const QString &p_openTag,
                           const QVector<PreviewTableAlignment> &p_alignments, bool p_hasHeaderRow,
                           bool p_markdownBacked);
};

// One origin cell of the logical grid, as the document holds it.

//
// The GEOMETRY is never stored here: QTextTable owns it and maintains it across
// every structural operation, so a second copy could only drift. What is stored
// is what a QTextTable cannot carry -- the verbatim source tag decision D-g
// requires, so a write-back rewrites `colspan`, `rowspan` and `align`
// attribute-locally and never regenerates a tag it did not author.
struct TablePreviewCellMeta {
  // The verbatim `<td …>` / `<th …>` opening tag, or empty for a cell this
  // sheet generated (an inserted row or column, or a slot exposed by a split).
  QString m_tag;
};

// The rich text document behind one editable sheet: a QTextDocument holding a
// single QTextTable, plus the metadata a QTextTable cannot carry.
//
// The document is the single source of truth. There is no parallel cell
// matrix: the cells are derived from the table on demand, so a caret sitting
// in a half-typed cell can never disagree with what is about to be serialized.
//
// The table is a LOGICAL GRID of rowCount() x columnCount() slots. Each slot is
// either an ORIGIN carrying text and metadata, or is COVERED by an origin above
// or to the left of it. A pipe table is the degenerate case where every slot is
// a 1x1 origin.
class TablePreviewDocument {

public:
  // Upper bound on the number of cells one sheet materializes.
  //
  // Measured, not guessed. A QTextTable is not virtualized: every cell is a
  // QTextBlock, the sheet renders at its full natural height, and Qt relays
  // the whole table out on any change inside it. On a release build the cost
  // is linear in the cell count and independent of the shape - at a constant
  // 300 cells, 300x1, 30x10 and 1x300 all land within 10.6 to 17.4 ms - at
  // roughly 0.055 ms per cell for the first layout, for a width reflow and,
  // crucially, for every single keystroke:
  //
  //     cells     first layout   reflow   one keystroke
  //       200            10 ms    10 ms         10.6 ms
  //       300            16 ms    16 ms         16.5 ms
  //       500            28 ms    29 ms         29.1 ms
  //      2000           136 ms   140 ms          138 ms
  //    200000         36702 ms 36367 ms        36386 ms
  //
  // 300 cells is therefore the last shape whose keystroke still costs about
  // one 16 ms frame rather than a multiple of it. The previous QTableView
  // sheet could afford 200000 because it was virtualized and fitted only the
  // rows it actually showed; this substrate cannot, so the bound is roughly
  // three orders of magnitude lower. A table over it is left to the static
  // source rendering, exactly as before.
  static const int c_maxCells;

  // Upper bound on the number of columns.
  //
  // Not a latency bound - at a constant cell count the shape does not affect
  // the layout cost at all - but it still binds on its own for a short table:
  // a one-row 250-column table is 250 cells, inside the bound above, and is
  // refused here. Inherited unchanged from the QTableView sheet.
  //
  // There is deliberately no matching row bound: with c_maxCells at 300, more
  // than 300 rows already means more than 300 cells for any table with at
  // least one column, so a row bound could never decide anything.
  static const int c_maxColumns;

  // Whether the normalized sheet of @p_table stays inside every bound above.
  static bool isWithinLimits(const TablePreview &p_table);

  // The number of cells the normalized sheet of @p_table would occupy.
  static qint64 normalizedCellCount(const TablePreview &p_table);

  // The width the normalized sheet of @p_table would have.
  static int normalizedColumnCount(const TablePreview &p_table);

  TablePreviewDocument();

  ~TablePreviewDocument();

  // Never null. Not parented, so whoever renders it must not outlive this.
  QTextDocument *document() const;

  // Null until the first setTable().
  QTextTable *table() const;

  // Rebuild the document from @p_table, normalizing the ragged matrix exactly
  // as the source snapshot describes it: every row padded to the widest row,
  // the alignments extended with PreviewTableAlignment::None.
  void setTable(const QSharedPointer<const TablePreview> &p_table);

  int rowCount() const;

  int columnCount() const;

  // --- The logical grid. ---

  // How the table is spelled in the source, and therefore which serializer a
  // commit uses. Sticky: once Html, never Markdown again (decision D-e).
  PreviewTableSyntax syntax() const;

  // Decision D-j: whether the cells hold Markdown source rather than literal
  // HTML text. Per table, never per cell.
  bool isMarkdownBacked() const;

  // Whether row 0 is a header row. Always true for a pipe table; for an HTML
  // table true iff row 0 was entirely `<th>`. Drives the header row count, the
  // row-0 bold weight, the `<th>` vs `<td>` choice for a generated cell, and
  // the serializer.
  bool hasHeaderRow() const;

  // Whether slot (@p_row, @p_column) owns its content rather than being covered
  // by a merged cell.
  //
  // Note that QTextTableCell::column() reports the ORIGIN column for every
  // covered slot, so it can NEVER answer "which half of a colspan is this" --
  // any slot-to-column mapping must go through the grid coordinates, not
  // through the cell.
  bool isOrigin(int p_row, int p_column) const;

  // Span of the cell OWNING slot (@p_row, @p_column); 1 outside the table.
  int rowSpanAt(int p_row, int p_column) const;
  int colSpanAt(int p_row, int p_column) const;

  // Whether any cell spans more than one row or column. A table holding one is
  // written back as HTML even when it was authored as pipe syntax, because GFM
  // cannot express a span (decision D-h).
  bool hasMergedCells() const;

  // Whether @p_column is spanned by a merged cell, which makes a per-column
  // alignment unrepresentable: a spanning cell carries ONE `align` attribute
  // (decision D-m). Splitting the cell restores alignment control.
  bool isColumnSpanned(int p_column) const;

  // The cell matrix, derived from the live table.
  QVector<QVector<QString>> cells() const;

  // Whether the contents can be written back without changing what the table
  // renders to. A body row wider than the header declares would otherwise
  // promote its excess cells into the header and delimiter rows, silently
  // giving the table a column GFM currently ignores.
  bool isRoundTrippable() const;

  // Whether the document still holds a table at all.
  //
  // The last line of defence. Every mutation is supposed to be confined to one
  // cell, because removing a selection which crosses a frame boundary removes
  // the frame - and this object would then be holding a dangling QTextTable.
  // Re-resolved from the document rather than compared against the cached
  // pointer, which is exactly what dangles in the case this detects.
  bool isIntact() const;

  // Incremented by every STRUCTURAL mutation - a build or rebuild, a row or
  // column insertion or removal, a merge, a split.
  //
  // Monotonic on purpose. The sheet's undo ring (decision D2) also fingerprints
  // the live geometry, but geometry alone is not enough: a structural change is
  // REVERSIBLE, so a merge followed by a split restores the row and column
  // counts and every slot's owner while leaving the cell TEXT rearranged. A
  // ring keyed only to the shape would come back to life and replay a
  // pre-merge cell over a post-split one. A counter cannot come back.
  quint64 structureGeneration() const { return m_structureGeneration; }

  // Canonical Markdown of the current document contents.
  //
  // @p_align opts into the padded, column-aligned pipe form. It is forwarded to
  // the Markdown serializer only: an HTML-backed or merged table is written by
  // TablePreviewHtmlSerializer, whose output has to match the scanner's
  // canonical subset exactly, so it ignores the flag.
  QString toMarkdown(bool p_align = false) const;

  // Copy-oriented Markdown of the current contents: the same table as
  // toMarkdown(), but deliberately without the row/delimiter prefixes the
  // binding carried (a blockquote's "> ", a list item's indent). The two are
  // not interchangeable - toMarkdown() is what the commit path writes back,
  // and dropping the prefixes there would tear the table out of its block.
  //
  // isRoundTrippable() is deliberately not required here: a ragged sheet is
  // widened by the serializer, which is harmless because nothing is written
  // back from this. Empty when the contents cannot be serialized safely (no
  // cells, zero width, or a cell holding a line separator).
  //
  // Pipe syntax CANNOT express a span, so a merged origin's text is put in its
  // origin slot and every covered slot is an empty string. The copy is
  // therefore a flattened view of a merged table, not a faithful one; toHtml()
  // is the faithful copy.
  //
  // @p_align opts into the padded form exactly as toMarkdown() does, so what
  // the user pastes matches what the document would get.
  QString toStandaloneMarkdown(bool p_align = false) const;

  // HTML rendered from toStandaloneMarkdown() by cmark, so inline Markdown
  // inside the cells is rendered too. Copy-oriented like its source: it also
  // drops the row prefixes and is never fed back into the document. Empty
  // whenever toStandaloneMarkdown() is empty.
  //
  // Deliberately never padded: the rendered HTML is layout independent, so
  // padding would only inject meaningless whitespace into the cell payloads.
  QString toHtml() const;

  // Whether one more row would still fit inside the cell bound.
  //
  // Growth by keystroke is the one path which is not covered by
  // isWithinLimits(), which only ever runs at bind time. The bound is the
  // latency budget documented on c_maxCells, and a keystroke which crossed it
  // would make every following keystroke miss its frame.
  bool canAppendRow() const;

  // Whether one more row would still fit inside the cell bound. Identical to
  // canAppendRow(): where a row is inserted does not change the cell count.
  bool canInsertRow() const;

  // Whether one more column would still fit inside both bounds. A column costs
  // one cell per row, and columns carry a bound of their own.
  bool canInsertColumn() const;

  // Whether @p_row may be removed. Row 0 is the header and is not an ordinary
  // row: removing it would mean promoting the next one, which also has to
  // carry the header's prefix - the only prefix which may hold a list marker -
  // and the host re-validates that prefix against the original binding. The
  // last remaining body row may go, but the table may not become empty.
  bool canDeleteRow(int p_row) const;

  // Whether @p_column may be removed. A table needs at least one column.
  bool canDeleteColumn(int p_column) const;

  // Insert one empty row above @p_row, which is refused for row 0: see
  // canDeleteRow() for why the header is not an ordinary row. Passing
  // rowCount() appends, which is what appendRow() is.
  //
  // Every precondition is checked before the edit block opens, because
  // QTextTable clamps an out-of-range index silently while QVector::insert()
  // asserts on one - and the two diverging is exactly the metadata/table drift
  // the serializer answers by throwing the edit away.
  //
  // Returns false when the row could not be inserted, in which case nothing
  // was touched.
  bool insertRow(int p_row);

  // Remove @p_row, which must be an ordinary body row.
  bool removeRow(int p_row);

  // Insert one empty column at @p_column, shifting the rest right. Passing
  // columnCount() appends.
  //
  // m_declaredColumnCount is moved with it: it is the width the document
  // intends to serialize as, and leaving it stale makes toMarkdown() return an
  // empty string, which flushPendingCommit() answers by restoring the source -
  // the user's column would silently vanish at commit time.
  bool insertColumn(int p_column);

  // Remove @p_column, together with its alignment.
  bool removeColumn(int p_column);

  // The grid rectangle a selection resolves to, or an invalid rectangle when
  // it does not lie inside the table at all. A caret with no selection
  // resolves to the 1x1 rectangle of its own origin.
  //
  // Expressed in GRID coordinates, so it distinguishes the halves of a colspan
  // that QTextTableCell::column() cannot.
  QRect selectionRect(const QTextCursor &p_cursor) const;

  // Whether the cells @p_cursor selects can be merged.
  //
  // Requires: a rectangle larger than 1x1, wholly inside the table, not
  // straddling the header row and a body row, not on a Markdown table carrying
  // container prefixes (decision D-l), and -- crucially -- CONTAINMENT: every
  // distinct origin the rectangle touches must have its whole
  // rowSpan x colSpan box inside it.
  //
  // The containment rule is not cosmetic. An imported HTML table can already
  // carry spans, and QTextTable::mergeCells() SILENTLY REFUSES a request whose
  // edge cuts a cell. Clearing the texts first and then not merging would erase
  // content and leave the metadata describing geometry that never changed.
  bool canMergeCells(const QTextCursor &p_cursor) const;

  // Merge the cells @p_cursor selects into one.
  //
  // The surviving cell's text is the non-empty source texts in grid order
  // joined with a single space (decision D-k). QTextTable::mergeCells()' own
  // concatenation is deliberately not used: it introduces paragraph breaks,
  // which hasLineSeparator() rejects, so the table would stop serializing.
  //
  // RECOVERY, NOT ROLLBACK. A beginEditBlock/endEditBlock pair groups edits; it
  // is not a transaction, and this document's undo stack is disabled, so there
  // is nothing to abort into. The full grid is snapshotted before the block and
  // the document is rebuilt from it if the post-merge verification fails. That
  // is a defence against an unexpected Qt refusal, not the normal path: the
  // containment preflight in canMergeCells() is what makes it unreachable.
  bool mergeCells(const QTextCursor &p_cursor);

  // Whether the cell owning slot (@p_row, @p_column) spans more than one slot
  // and may therefore be split back into 1x1 cells.
  bool canSplitCell(int p_row, int p_column) const;

  // Split the cell owning slot (@p_row, @p_column) into 1x1 cells. The origin
  // keeps its text and its verbatim tag; each newly exposed slot gets a
  // generated `<th>` or `<td>` per hasHeaderRow().
  bool splitCell(int p_row, int p_column);

  // Set the alignment of @p_column, which is what the delimiter row of the
  // serialized table carries.
  //
  // Returns false when the value is unchanged, so an idempotent menu click
  // does not arm a commit. The re-formatting is a document change like any
  // other, which is what carries a None -> Left switch - invisible in the
  // rendered table, both being Qt::AlignLeft - to the commit machinery.
  bool setColumnAlignment(int p_column, PreviewTableAlignment p_alignment);

  PreviewTableAlignment columnAlignment(int p_column) const;

  // Append one empty row at the bottom of the table.
  //
  // Not a plain QTextTable::appendRows(1): m_rowPrefixes carries one entry per
  // row and TablePreviewSerializer::serialize() returns an empty string on a
  // size mismatch, which flushPendingCommit() reads as a rejection and answers
  // by rebuilding from source - the appended row would silently disappear. The
  // prefix appended here is m_delimiterPrefix, which is exactly the prefix
  // arePrefixesSafe() demands of every row after the header. The per-cell
  // formats are not carried over by appendRows() either, so they are written
  // explicitly.
  //
  // Returns false when the row could not be added, in which case nothing was
  // touched.
  bool appendRow();

  // The character format a cell of @p_row starts from, before any syntax run
  // is merged over it: the header weight and nothing else.
  //
  // This is what newly typed or pasted text must carry. Qt otherwise gives an
  // insertion the format of the character to its left, so typing right after a
  // highlighted span - '**bold**' - would silently extend that span's colour
  // and weight over the new text, which the next parse does not necessarily
  // undo: a run whose start and length did not move leaves the format matrix
  // unchanged, and the refresh pass then has nothing to do.
  QTextCharFormat baselineCellFormat(int p_row) const;

  // Re-apply the per-cell formats - the column alignment and the header
  // weight - without touching a single character of cell text.
  //
  // A font, palette or style change has to be answered without rebuilding the
  // document: rebuilding would destroy the caret and any selection, which for
  // a theme switch during typing is a silent loss of the user's place.
  //
  // The per-character syntax highlight runs are deliberately not re-applied
  // here: they are owned by build() and refreshCellSyntaxFormats(), and
  // re-applying a stored matrix from a font or palette change would paint a
  // stale row/column/offset mapping after in-cell typing or a row/column
  // insert or delete.
  void applyCellFormats();

  // Re-apply the per-cell syntax highlight runs of a snapshot which describes
  // exactly the live cells, without rebuilding the document. Returns false
  // when the matrix is already the applied one and nothing was done.
  bool refreshCellSyntaxFormats(const QVector<QVector<QVector<PreviewFormatRun>>> &p_cellFormats);

  // The one colour the table itself owns. Separate from applyCellFormats()
  // because a rebuild has just written every per-cell format and only this is
  // still owed.
  void applyPalette(const QPalette &p_palette);

private:
  // A complete, self-contained description of the grid, used only to rebuild
  // the document after a merge verification failure. Deliberately built from
  // the same fields setTable() feeds build(), so the rebuild takes exactly the
  // path a fresh bind does.
  struct GridSnapshot {
    QVector<QVector<QString>> m_source;
    QVector<QVector<QVector<PreviewFormatRun>>> m_cellFormats;
    QVector<QVector<TablePreviewCellMeta>> m_cellMeta;
    QVector<QVector<QPoint>> m_spans; // (colSpan, rowSpan) at each origin.
    QVector<PreviewTableAlignment> m_alignments;
    QVector<QString> m_rowPrefixes;
    QVector<QString> m_rowTags;
    QString m_delimiterPrefix;
    int m_rowCount = 0;
    int m_columnCount = 0;
    int m_declaredColumnCount = 0;
  };

  GridSnapshot captureGrid() const;

  // The verbatim cell tags, row major over the grid. Empty at a slot whose cell
  // this sheet generated.
  QVector<QVector<QString>> cellTagGrid() const;

  // The verbatim tag of the origin OWNING each slot, repeated on every slot it
  // covers. Captured before a structural mutation, so a spanning origin whose
  // own coordinates are about to be deleted can still be found from a slot that
  // survives.
  QVector<QVector<QString>> ownerTagGrid() const;

  // Rebuild m_cellMeta and m_rowTags after a structural mutation, keyed to the
  // LIVE geometry QTextTable now reports.
  //
  // A positional insert()/remove() on the metadata is wrong for a spanning
  // cell: deleting the origin ROW of a rowspan (or the origin COLUMN of a
  // colspan) makes Qt keep the cell and MOVE its origin, and the positional
  // edit would discard the only copy of its authored tag -- silently destroying
  // `class`, `style` and `data-*` at the next commit, against decision D-g.
  //
  // @p_rowMap and @p_columnMap give, for each POST-mutation index, the
  // pre-mutation index it came from, or -1 for one this sheet inserted.
  // Bump structureGeneration(). Called from every structural mutator, just
  // before it closes its edit block.
  void noteStructuralChange() { ++m_structureGeneration; }

  void remapCellMeta(const QVector<QVector<QString>> &p_ownerTags,
                     const QVector<QString> &p_rowTags, const QVector<int> &p_rowMap,
                     const QVector<int> &p_columnMap);

  void restoreGrid(const GridSnapshot &p_snapshot);

  void build();

  // The single writer of one cell's formats, so build() and applyCellFormats()
  // cannot drift apart.
  void applyCellFormat(int p_row, int p_column);

  // Overlay the resolved syntax highlight runs of one cell. Runs are merged,
  // in order, over whatever applyCellFormat() has just written, so the header
  // weight survives while inline styles win on overlap. A run whose range does
  // not fit the cell text exactly is skipped entirely rather than clipped: the
  // walker already clipped to the cell, so a bad range means offset drift and
  // clipping would paint the wrong substring.
  void applyCellSyntaxFormats(int p_row, int p_column);

  // Normalize a snapshot's run matrix onto the current m_rowCount/m_columnCount
  // shape, so a padded cell gets an empty run list.
  QVector<QVector<QVector<PreviewFormatRun>>>
  normalizeCellFormats(const QVector<QVector<QVector<PreviewFormatRun>>> &p_cellFormats) const;

  // The block format one column's cells carry.
  Qt::Alignment blockAlignment(int p_column) const;

  // Rewrite the table format's column width constraints to one VariableLength
  // entry per column.
  //
  // Defensive canonicalization rather than a workaround: both supported Qt
  // majors do resize a non-empty constraints vector on insertColumns() and
  // removeColumns(), but the vector is what build() hands the layout to share
  // the band out by content, and a stale one would silently change how every
  // column is measured.
  void rewriteColumnConstraints();

  QScopedPointer<QTextDocument> m_doc;

  // Owned by m_doc.
  QTextTable *m_table = nullptr;

  // The raw matrix of the bound snapshot, normalized. Only used to build the
  // document; cells() reads the document itself afterwards.
  QVector<QVector<QString>> m_source;

  // The syntax highlight runs currently written into the document, normalized
  // onto the same shape as m_source. Compared against an incoming matrix to
  // decide whether a refresh has anything to do: a theme or font-size change
  // keeps every start, length and count identical and only changes the format.
  QVector<QVector<QVector<PreviewFormatRun>>> m_appliedCellFormats;

  QVector<PreviewTableAlignment> m_alignments;

  QVector<QString> m_rowPrefixes;

  QString m_delimiterPrefix;

  int m_rowCount = 0;

  int m_columnCount = 0;

  // Column count the document intends to serialize as, i.e. the width its
  // header and delimiter rows declare. Initialized from the source and moved
  // by every deliberate column operation; a mismatch with m_columnCount means
  // a body row is wider than the header declares, which is not
  // round-trippable.
  int m_declaredColumnCount = 0;

  // See structureGeneration().
  quint64 m_structureGeneration = 0;

  // --- HTML syntax state (decisions D-b, D-d, D-e, D-g, D-j). ---

  PreviewTableSyntax m_syntax = PreviewTableSyntax::Markdown;

  bool m_markdownBacked = true;

  bool m_hasHeaderRow = true;

  // The verbatim `<table …>` tag, empty when this sheet must generate one.
  QString m_openTag;

  // One verbatim `<tr …>` tag per row; an entry is empty for a generated row.
  QVector<QString> m_rowTags;

  // Row major over the grid, indexed by ORIGIN coordinates. Entries at covered
  // slots are meaningless and are never read.
  QVector<QVector<TablePreviewCellMeta>> m_cellMeta;

  // The spans build() must apply, filled by setTable() and by restoreGrid()
  // and consumed by build(). The LIVE geometry always comes from QTextTable,
  // which maintains it across every structural operation; a second live copy
  // could only drift.
  QVector<QVector<QPoint>> m_pendingSpans;
};

// The text edit the sheet is rendered and edited in.
//
// One caret roams every cell, and wrapping plus height-for-width come from
// Qt's rich text layout rather than a bespoke solver. It never scrolls
// internally: both bars are off, the sheet renders at its full natural height
// and every wheel movement belongs to the editor underneath.
//
// The base is VTextEdit, not QTextEdit, and that is the whole reason the three
// input modes can run in here at all: an AbstractInputMode is installed
// through VTextEdit::setInputMode() and consulted from VTextEdit's own
// keyPressEvent(), and Qt delivers a key event to the FOCUS WIDGET -- being a
// child of the editor's viewport routes nothing through the editor's mode. The
// same inheritance is what answers Qt::ImEnabled from the sheet's own mode, so
// input-method enablement inside a previewed table matches the editor outside
// it.
//
// VTextEdit was written for a plain, whole-document text editor, so several of
// the facilities it brings are either neutral here or actively hostile. The
// classification, in full:
//
//  - USED. The input mode plumbing (setInputMode(), the mode consultation in
//    keyPressEvent(), the ShortcutOverride steal path in its self-installed
//    event filter), inputMethodQuery(Qt::ImEnabled)/setInputMethodEnabled(),
//    the overridden-selection bookkeeping Vi's visual modes drive and the
//    Vi-inclusive createMimeDataFromSelection() built on it, mouseReleased()
//    (which katevi connects to), and the block-caret width timers behind
//    setDrawCursorAsBlock().
//  - NEUTRAL. The custom ScrollBar (both bars are permanently off here), the
//    content-width margins (m_maxContentWidth stays 0, so
//    updateContentWidthMargins() only ever resets the margins to zero),
//    checkCenterCursor() (CenterCursor::NeverCenter is the default and is left
//    alone), the cursor-line tracking (one signal nothing in this file
//    listens to), and resized().
//  - DISABLED. Auto brackets, switched off in the constructor: a cell holds
//    raw Markdown, and silently inserting a closing bracket would edit the
//    source the user is typing.
//  - UNREACHABLE BY CONSTRUCTION. VTextEdit's Tab/Backtab indentation and its
//    Return auto-indent live in handleDefaultKeyPress(), which is only reached
//    for keys keyPressEvent() below has already let through -- and those four
//    keys never are.
//  - HIDDEN. removeSelectedText(), insertFromMimeDataOfBase() and
//    setOverriddenSelection() are newly inherited mutation bypasses; see the
//    refusals further down.
class TablePreviewSheet : public VTextEdit {
  Q_OBJECT
public:
  explicit TablePreviewSheet(QWidget *p_parent = nullptr);

  ~TablePreviewSheet() Q_DECL_OVERRIDE;

  // The document this sheet renders. Not owned.
  void setTableDocument(TablePreviewDocument *p_document);

  // The height the sheet needs when its *outer* width is p_outerWidth. The
  // conversion to a document text width is not optional: assigning the outer
  // width straight to QTextDocument::setTextWidth() under-measures, and with
  // the scroll bars off the clipped content would be unreachable.
  int heightForWidth(int p_outerWidth) const Q_DECL_OVERRIDE;

  bool hasHeightForWidth() const Q_DECL_OVERRIDE;

  // Width 0 on purpose: preferredWidthFraction() is 1.0, so the host resolves
  // the band to the full available content width.
  QSize sizeHint() const Q_DECL_OVERRIDE;

  QSize minimumSizeHint() const Q_DECL_OVERRIDE;

  // Row major index of the cell holding the caret, or -1 when it is outside
  // the table.
  int currentCellIndex() const;

  // Commit whatever the input method is still holding in a preedit.
  //
  // A preedit is not in the document yet, so serializing across one would
  // silently drop the characters the user has already typed. A no-op unless
  // this sheet owns the input focus: QInputMethod acts on the application's
  // focus object, and a flush also runs on background sheets.
  void commitPreedit();

  // Cancel an open composition, on the platform and in the text control.
  //
  // Called just before the sheet becomes a viewer, which refuses every input
  // method event outright: a preedit still installed at that point would be
  // left rendered over a sheet which can never accept it, and the platform and
  // the control would start the next composition from different states.
  void cancelComposition();

  // Re-apply everything derived from the palette and the font, including the
  // per-cell formats.
  void refreshFormats();

  // Only the palette-derived parts. Used right after a rebuild, which has
  // already written every per-cell format.
  void refreshPalette();

  // Whether the "Copy as Markdown" payload this sheet's context menu builds is
  // padded and column aligned. Mirrored down from the widget so the copy and
  // the write-back cannot disagree about the shape of the source.
  void setSourceAlignEnabled(bool p_enabled) { m_alignSource = p_enabled; }

  // Put the caret back inside the table when something parked it in the empty
  // block a QTextDocument always keeps after a table.
  void clampCursorIntoTable();

  // Confine an ORDINARY selection to the cell holding the caret.
  //
  // QTextCursor::removeSelectedText() over a range which crosses a frame
  // boundary removes the frame, so a Ctrl+A followed by one keystroke, a
  // Delete, a Cut, a paste or an internal drag would take the whole table out
  // of the document - and TablePreviewDocument would be left holding a
  // dangling QTextTable. This runs from cursorPositionChanged and
  // selectionChanged, so it covers every way a selection can be made,
  // including the ones which come from QTextEdit's own context menu and never
  // pass through keyPressEvent(). The retired QTableView sheet was
  // SingleSelection for the same reason, so no affordance is lost.
  //
  // It is NOT the frame-safety gate on its own: a CELL RECTANGLE is
  // deliberately left alone here so Merge and the copy actions can read it
  // (decision D-f), and collapseComplexSelectionForMutation() below is what
  // every text-mutating path goes through instead.
  void clampSelectionIntoOneCell();

  // Collapse a CELL-RECTANGLE selection onto one cell, then clamp.
  //
  // A rectangle is preserved by clampSelectionIntoOneCell() so Merge and the
  // copy actions can see it (decision D-f). That reopens every path the single
  // clamp used to close, so this is the gate every text-mutating path must go
  // through instead - including the ones which never reach keyPressEvent():
  // the standard context menu's Cut and Delete, the programmatic cut() and
  // paste(), a drop, and the source removal of an internal move-drag.
  //
  // EXTERNAL move-drag is not covered by any of them. On both Qt majors
  // QWidgetTextControlPrivate::startDrag() removes the source selection after a
  // successful Qt::MoveAction whose target is not this text control, AFTER the
  // drag returns and without passing through dropEvent(),
  // insertFromMimeData() or keyPressEvent(). mousePressEvent() therefore
  // collapses a rectangle the press lands inside BEFORE the base handler can
  // arm a drag at all.
  //
  // isIntact() remains the backstop, not the gate.
  void collapseComplexSelectionForMutation();

  // Gated re-spellings of QTextEdit's clipboard slots, which are not virtual
  // and would otherwise mutate through a rectangle selection.
  void cut();
  void paste();

  // Pin the format of the next insertion to the caret cell's baseline.
  //
  // Qt gives an insertion the character format of the text to its left, so
  // typing or pasting right after a highlighted span - '**bold**' - would
  // extend that span's colour and weight over the new characters. The next
  // parse does not necessarily undo it: when the run's start and length do not
  // move, the format matrix is unchanged and the refresh pass is skipped.
  void resetInsertionFormat();

  // Collapse any selection onto the caret, without moving the caret.
  void clearSelection();

  // The menu shown for a right click at @p_viewportPos: a Table submenu
  // holding the row, column and alignment operations, then a separator, then
  // QTextEdit's own menu. Never null; the caller owns it.
  //
  // Moves the caret to the cell under @p_viewportPos, so the operations are
  // relative to what the user pointed at. A click which is not in a cell at
  // all - the shrunken block after the table, for one - leaves the caret alone
  // and gets the standard menu unchanged, and so does a click on a selection:
  // that one is aimed at the text, which is what the standard menu acts on.
  //
  // Public because it is the only seam a test can inspect without entering a
  // modal exec() - the action tree, the enabled states and the checked
  // alignment are the whole contract of the menu.
  QMenu *createContextMenu(const QPoint &p_viewportPos);

  // --- The input mode seam (decisions D1, D2, D5, D7). ---

  // Record which of the three modes this sheet should run, WITHOUT creating
  // it (decision D5: one mode per sheet, created lazily on first focus).
  //
  // A note can hold dozens of previewed tables and a mode is not free: Vi
  // builds a KateVi::InputModeManager and a command bar per instance. Only a
  // sheet the user actually interacts with pays for one. A sheet which has
  // already been given a mode replaces it immediately instead, so a
  // configuration change reaches the sheet being typed into at once.
  void setDesiredInputMode(InputMode p_mode);

  // Create the recorded mode if it has not been created yet. Idempotent.
  void ensureInputMode();

  // Install @p_mode, replacing whatever is installed. Idempotent.
  //
  // Created from the same InputModeMgr factory the editor uses, so the shared
  // KateVi::GlobalState - registers, macros, mappings, the command and search
  // histories - is the editor's (decision D5).
  //
  // Decision D7: the mode is installed exactly once, through
  // VTextEdit::setInputMode(), which already deactivates the outgoing mode and
  // activates the incoming one. activate()/deactivate() are never called from
  // here; a second activate() trips ViInputMode's own assertion.
  void installInputMode(InputMode p_mode);

  // Uninstall whatever is installed, back to plain typing. Idempotent, and
  // safe to call during destruction - which is where it is called from.
  void removeInputMode();

  // The installed mode's status widget, or null. The Vi command bar lives in
  // here.
  QSharedPointer<InputModeStatusWidget> inputModeStatusWidget() const;

  // The document the sheet renders, or null before the first binding. The
  // input mode resolves the caret's cell through it.
  TablePreviewDocument *tableDocument() const { return m_document; }

  // The cell holding the caret and its [first, last] character range, which is
  // the WHOLE addressable universe of the input mode: the sheet guarantees one
  // cell is one QTextBlock, so the mode projects it as a one-line document.
  // Returns false when the caret is not in a cell at all.
  bool currentCellRange(int &p_first, int &p_last) const;

  // The one separator policy, exposed so the mode's mutating overrides run
  // their payloads through exactly what a paste and an input method commit go
  // through.
  static QString sanitizeCellPayload(const QString &p_text);

  // Whether a key press in the current mode inserts text.
  //
  // False in Vi normal and the three visual modes, where a printable key is a
  // command. Two things hang off it: the input method is disabled in exactly
  // those modes (which is the whole point of the feature), and
  // resetInsertionFormat() is skipped, because QTextEdit::setCurrentCharFormat()
  // applies to the SELECTION when there is one and visual mode always has one.
  bool isTextInsertingMode() const;

  // Drive the input method from the sheet's OWN mode, using the same rule
  // VTextEditor and VRichTextEditor apply to the editor.
  //
  // Not a plain setInputMethodEnabled(): that calls QInputMethod::reset(),
  // which on Windows re-enters this very widget with a synchronous commit
  // while katevi is still processing the key that caused the transition. The
  // open composition is therefore resolved deliberately - committed on the way
  // OUT of an inserting mode, cancelled otherwise - under the same
  // m_cancellingComposition guard cancelComposition() uses.
  void syncInputMethodToMode();

  // --- The undo ring (decision D2). ---

  // Flush whatever has been typed into the caret's cell since the last
  // checkpoint into the undo ring, so the next mutation becomes a separate
  // step. A no-op when the cell is unchanged.
  //
  // This is what gives the ring vi's granularity without a QTextDocument undo
  // stack: a checkpoint is taken when the caret leaves a cell, before every
  // mutation the mode drives, and before a replay - so one `u` undoes one
  // insert session or one operator.
  void commitUndoCheckpoint();

  // Replay one step. Return false when the ring is empty, which is what makes
  // Ctrl+Z fall through to the editor's own stack.
  bool undoFromRing();

  bool redoFromRing();

  // Ring depth including an uncommitted in-cell edit, which is what the mode's
  // undoCount()/redoCount() report.
  int undoRingDepth() const;

  int redoRingDepth() const;

  // Drop everything. Called on every structural operation, on a rebuild from
  // source, on a successful commit and on a rebind: a ring entry may never
  // outlive the geometry it was recorded against.
  //
  // The BASELINE is re-anchored on the caret's current cell rather than
  // dropped, so the first edit after a clear is still undoable.
  void clearUndoRing();

  // Every wheel movement belongs to the editor underneath: the sheet has no
  // scroll bars and nothing it could scroll.
  //
  // Public because VTextEdit declares it so; narrowing an inherited public
  // member on the derived type only makes the two disagree, since a caller
  // holding a VTextEdit * reaches it either way.
  void wheelEvent(QWheelEvent *p_event) Q_DECL_OVERRIDE;

  // Public for the same reason as wheelEvent().
  void mousePressEvent(QMouseEvent *p_event) Q_DECL_OVERRIDE;

signals:
  // The document's laid out size settled on something the host has not been
  // told about yet.
  void preferredGeometryChanged();

  void focusEscapeRequested(vte::FocusEscapeDirection p_direction);

  void undoRequested();

  void redoRequested();

  // The caret left the cell it was in, which commits that cell.
  void cellLeft();

  void focusLost();

  // The installed mode's status widget was replaced or dropped. Carries
  // nothing: the host asks inputModeStatusWidget() for the current one, and
  // the ORDER matters more than the payload - this is emitted while the
  // outgoing mode is still alive, so the host can unmount its status widget
  // before it dies.
  void inputModeStatusWidgetChanged();

protected:
  void keyPressEvent(QKeyEvent *p_event) Q_DECL_OVERRIDE;

  void focusInEvent(QFocusEvent *p_event) Q_DECL_OVERRIDE;

  void focusOutEvent(QFocusEvent *p_event) Q_DECL_OVERRIDE;

  // A drop is a mutation which never reaches keyPressEvent(); it is also where
  // an INTERNAL move-drag removes its source.
  void dropEvent(QDropEvent *p_event) Q_DECL_OVERRIDE;

  // QTextEdit has no hook to extend its standard menu, so the whole handler is
  // replaced: the base one would only ever show the plain menu.
  void contextMenuEvent(QContextMenuEvent *p_event) Q_DECL_OVERRIDE;

  void resizeEvent(QResizeEvent *p_event) Q_DECL_OVERRIDE;

  // A committed input method string is an insertion like any other: it can
  // carry a line separator, and both its replacement range and any Selection
  // attribute it carries are resolved by the base handler in an intermediate
  // document state, independently of the selection - so neither is covered by
  // the clamp and both are handled here.
  void inputMethodEvent(QInputMethodEvent *p_event) Q_DECL_OVERRIDE;

  // Plain text only, and never a line separator: the serializer rejects
  // '\n', '\r', U+2028 and U+2029, so a cell which contains one can no longer
  // be written back at all.
  bool canInsertFromMimeData(const QMimeData *p_source) const Q_DECL_OVERRIDE;

  void insertFromMimeData(const QMimeData *p_source) Q_DECL_OVERRIDE;

private slots:
  void handleDocumentSizeChanged();

  void handleCursorPositionChanged();

  // QTextEdit::selectAll() - and therefore Ctrl+A and the context menu's
  // Select All - moves the anchor without moving the caret's *position*, so it
  // emits selectionChanged() and no cursorPositionChanged() at all. The
  // selection clamp has to be reachable from both.
  void handleSelectionChanged();

private slots:
  // Inherited BULK MUTATORS and MUTATION BYPASSES, hidden AND re-registered.
  //
  // The sheet inherits VTextEdit publicly, and every one of these either
  // replaces or empties the whole document - destroying the QTextTable
  // outright, leaving TablePreviewDocument holding a dangling pointer - or
  // reaches past the one gate that keeps a mutation inside a single cell. They
  // are not selection-mediated at all, so the collapse gate above does not
  // cover them: "every text-mutating path is gated" must NOT be read as
  // including these.
  //
  // Declared as SLOTS rather than plain private members on purpose. None of
  // them is virtual, so hiding alone only closes the compile-time route; moc
  // registers these on the most-derived metaobject, which closes the
  // QMetaObject::invokeMethod() and connect()-by-name routes as well. A caller
  // which casts to QTextEdit* or VTextEdit* first still reaches the base
  // implementation; that case is caught after the fact by isIntact(), which
  // rebuilds the sheet from source. Exactly one controlled insertion route
  // stays open, for the already-gated paste/drop path.
  void clear();
  void setText(const QString &p_text);
  void setPlainText(const QString &p_text);
  void setHtml(const QString &p_html);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
  void setMarkdown(const QString &p_markdown);
#endif
  void append(const QString &p_text);
  void insertPlainText(const QString &p_text);
  void insertHtml(const QString &p_html);
  void setDocument(QTextDocument *p_document);

  // The three newly inherited from VTextEdit.
  //
  // removeSelectedText() removes VTextEdit's *selected range* - the overridden
  // one when Vi has installed one - directly through a fresh QTextCursor,
  // without consulting the clamp, so it can delete across a frame boundary
  // from a preserved cell rectangle and take the table with it.
  //
  // insertFromMimeDataOfBase() calls QTextEdit::insertFromMimeData() straight
  // through, which is precisely the plain-text sanitizer and the separator
  // policy this sheet exists to enforce.
  //
  // setOverriddenSelection() takes arbitrary physical document positions and
  // publishes them as *the* selection, which defeats the one-cell confinement
  // every mutating path is measured against. The input mode reaches the base
  // implementation through its own VTextEdit * for the cell-confined range it
  // has already computed; nothing else may.
  void removeSelectedText();
  void insertFromMimeDataOfBase(const QMimeData *p_source);
  void setOverriddenSelection(int p_start, int p_end);

private:
  // What the frame and the viewport margins take off the outer width and
  // height. Read from the live geometry once there is one, because a style may
  // inset the viewport by more than the frame width alone.
  int horizontalChrome() const;

  int verticalChrome() const;

  // Move the caret to the next or previous cell.
  //
  // Returns false when there is no adjacent cell - past the last, before the
  // first, or no table at all. Deliberately does NOT wrap (decision D3): the
  // caller answers false by handing the caret back to the editor, which since
  // Escape became the input mode's is the only way out of the sheet other than
  // the edge arrows.
  bool moveToAdjacentCell(bool p_forward);

  // Perform one of the delete shortcuts here, bounded by the caret's cell.
  //
  // QWidgetTextControl answers these by building a selection *and* removing it
  // inside the same call, on its own cursor and without emitting a signal in
  // between - so a clamp before the base handler sees no selection yet and the
  // signal-driven clamps never run at all. Ctrl+Delete at the end of a cell
  // selects into the next one, and at the end of the last cell out of the
  // table entirely, where QTextCursor expands the range to the whole frame.
  // Returns true when @p_event was one of them and has been handled.
  bool deleteWithinCell(const QKeyEvent *p_event);

  // Whether the caret is on the first visual line of the first row, or the
  // last visual line of the last row - the two edges at which an arrow key
  // hands the caret back to the editor.
  bool isAtTopEdge() const;

  bool isAtBottomEdge() const;

  // Grow the table by one row when the caret sits in the last cell of the last
  // row, and put the caret in the first cell of the new row.
  //
  // Returns false when the key does not apply here, in which case *nothing*
  // has been touched: the checks deliberately run before commitPreedit() and
  // clearSelection(), both of which have side effects (the former can insert
  // the composition text synchronously, the latter rewrites the cursor), and a
  // refused Enter has to stay as inert as the plain swallow it replaces.
  bool appendRowFromLastCell();

  // The Table submenu, parented to @p_parent. Returns null when the caret is
  // not in a valid cell, in which case the standard menu is shown unchanged.
  //
  // With @p_offerMutations false only the copy entries are emitted: the row,
  // column and alignment operations are relative to one cell, and the caller
  // suppresses them when a selection survived the click - a mutation would
  // then act on a cell other than the one the selection covers. Copying the
  // whole table is a read, so it stays reachable in that case.
  //
  // @p_slot is the GRID slot the pointer resolved to, which is not derivable
  // from the caret: QTextTableCell::column() reports the ORIGIN column for
  // every covered slot, so it cannot distinguish which half of a colspan was
  // clicked. Merge is offered separately from @p_offerMutations, because the
  // suppression fires precisely when a selection survived the click - which is
  // exactly when a merge is possible.
  QMenu *buildTableMenu(QMenu *p_parent, bool p_offerMutations, const QPoint &p_slot);

  // The grid slot (x = column, y = row) under @p_viewportPos, or (-1, -1).
  //
  // Resolved GEOMETRICALLY inside a spanning cell: the pointer's x is compared
  // against a row in which the candidate column is one slot wide. When every
  // row spans it there is nothing to distinguish, and the origin is returned.
  QPoint gridSlotAt(const QPoint &p_viewportPos) const;

  // Put the caret in @p_row/@p_column, clamped to the table's current bounds,
  // and re-establish both frame invariants. Called after every mutation: Qt
  // can park the caret in the trailing block when the row or column it was in
  // has just been removed.
  void focusCell(int p_row, int p_column);

  // Re-seed the sheet's palette from its parent and hand the derived colours
  // to the document. Returns the effective palette.
  QPalette applyPalette();

  // Unmount the current mode's status widget: tell the host first, then make
  // sure it is unparented. Both halves are required before the mode which owns
  // it is destroyed - ViInputMode's destructor asserts on the parentage.
  void detachInputModeStatusWidget();

  // The body of syncInputMethodToMode(). @p_resolveComposition is false for the
  // callers which have already resolved an open composition themselves and know
  // a reason this function cannot see - a mode replacement or removal.
  void applyInputMethodState(bool p_resolveComposition);

  // The interface the installed mode holds by RAW pointer, so it must outlive
  // the mode. That is why this class has an explicit destructor: VTextEdit
  // owns the mode and destroys it from ~VTextEdit, which runs AFTER this
  // class's members are gone.
  QScopedPointer<TablePreviewInputMode> m_inputModeInterface;

  QSharedPointer<InputModeStatusWidget> m_inputModeStatusWidget;

  // The mode this sheet should run once it is first interacted with, and
  // whether anything has asked for one yet. See setDesiredInputMode().
  InputMode m_desiredInputMode = InputMode::NormalMode;

  bool m_desiredInputModeSet = false;

  // Not owned.
  TablePreviewDocument *m_document = nullptr;

  // heightForWidth() lays the live document out, which synchronously emits
  // documentSizeChanged. Reporting that back to the host is how a measurement
  // loop starts.
  mutable bool m_measuring = false;

  // Set while the sheet is applying a geometry the host chose, for the same
  // reason.
  bool m_applyingGeometry = false;

  // Set while refreshFormats() rewrites the character and block formats, which
  // is a document change the commit machinery must not see as an edit.
  bool m_applyingFormats = false;

  // See setSourceAlignEnabled(). Read only when a copy payload is built.
  bool m_alignSource = false;

  // The last (outer width, height) pair reported to the host. An unchanged
  // pass terminates here instead of looping against the host's own cache.
  int m_notifiedOuterWidth = -1;

  int m_notifiedHeight = -1;

  // Set while a clamp is rewriting the cursor, whose own
  // cursorPositionChanged would otherwise re-enter it.
  bool m_clampingCursor = false;

  // Set across QInputMethod::reset(), which several platforms answer by
  // sending this very widget a synchronous commit or clearing event.
  bool m_cancellingComposition = false;

  // The input method state the sheet's mode calls for, and whether it has been
  // applied to the platform yet.
  //
  // Compared against the DESIRED value rather than against
  // inputMethodQuery(Qt::ImEnabled), which also folds in the process-wide
  // VTextEdit::forceInputMethodDisabled(). "Applied" is false while the sheet
  // does not own the focus: QInputMethod acts on the application's focus
  // object, so a background sheet may only record what it wants.
  bool m_inputMethodDesired = true;

  bool m_inputMethodApplied = false;

  // The cell the caret was last seen in, so cellLeft() fires once per move
  // rather than once per keystroke.
  int m_lastCellIndex = -1;

  // --- The undo ring (decision D2). ---

  // One recorded state of one cell. The GEOMETRY is deliberately not part of
  // it: a ring entry is only ever replayed against a table whose structure has
  // not moved, which is what m_undoRingFingerprint enforces.
  struct CellSnapshot {
    // Row major over the grid, exactly as currentCellIndex() reports it - so
    // for a merged cell it is the ORIGIN's index.
    int m_cell = -1;

    QString m_text;
  };

  // Upper bound on the ring depth. Small on purpose: this is not a document
  // history, it is the last few cell edits, and every entry holds a whole cell
  // of text.
  static const int c_maxUndoDepth;

  // A cheap description of the table's identity for undo purposes: the
  // document's monotonic structure generation, then the row and column counts
  // and every slot's owner.
  //
  // Derived from the live document rather than maintained beside it, so a
  // structural operation added later cannot forget to invalidate the ring -
  // decision D2's failure mode is exactly a replay which reverts a cell of a
  // geometry that no longer exists. The generation is what makes it monotonic;
  // the shape is what still catches a mutator which forgot to bump it.
  //
  // Cheap enough to recompute per use: c_maxCells bounds it at 300 slots.
  QByteArray tableStructureFingerprint() const;

  // Whether the ring still describes the live table. Clears it when it does
  // not, so every caller may simply ask.
  bool isUndoRingLive();

  // Record the caret cell as the state the next checkpoint compares against.
  void captureUndoBaseline();

  // The text of the cell at row-major index @p_cell, or a null string when
  // that slot is not a live origin.
  QString cellTextAt(int p_cell) const;

  // Replace the text of @p_cell, leaving the caret in it. Returns false when
  // the slot is gone.
  bool setCellTextAt(int p_cell, const QString &p_text);

  // Apply one ring entry and record the state it replaced on the other ring.
  bool replaySnapshot(QVector<CellSnapshot> &p_from, QVector<CellSnapshot> &p_to);

  QVector<CellSnapshot> m_undoRing;

  QVector<CellSnapshot> m_redoRing;

  // The state of the caret's cell as of the last checkpoint. m_cell is -1 when
  // there is nothing to compare against.
  CellSnapshot m_undoBaseline;

  // The shape both rings were recorded against.
  QByteArray m_undoRingFingerprint;

  // Set while a replay is rewriting a cell, so the checkpoint machinery does
  // not mistake the replay for a fresh edit.
  bool m_replayingRing = false;
};

class TablePreviewWidget : public PreviewWidget {
  Q_OBJECT
public:
  // Outcome of a write-back attempt.
  enum class FlushOutcome {
    // Nothing is owed anymore: the commit was accepted, there was nothing to
    // commit, or the sheet is not in a state where it may commit at all.
    Settled,

    // The host could not apply the request right now and nothing was touched.
    // The sheet keeps its authority, its edit and its debounce, and the write
    // back is retried later.
    Deferred,

    // The request was answered with a rejection.
    Rejected
  };

  // Share of the available content width a sheet spans. The whole band: the
  // column widths are owned by Qt's table layout now, which distributes
  // whatever width it is given, so there is no natural width to fall back to.
  static const qreal c_widthFraction;

  // Idle time after the last keystroke before the sheet writes itself back.
  static const int c_commitDebounceMs;

  TablePreviewWidget(PreviewWidgetContext *p_context, QWidget *p_parent);

  ~TablePreviewWidget() Q_DECL_OVERRIDE;

  QVector<PreviewElementType> supportedTypes() const Q_DECL_OVERRIDE;

  bool setPreview(const QSharedPointer<const Preview> &p_preview) Q_DECL_OVERRIDE;

  qreal preferredWidthFraction() const Q_DECL_OVERRIDE;

  void clearSelection() Q_DECL_OVERRIDE;

  // Mirror the editor's read-only state so the sheet cannot swallow edits the
  // host is going to reject anyway.
  void setReadOnly(bool p_readOnly);

  // Mirror the editor's configured input mode onto the sheet (decision D6).
  //
  // Unlike setReadOnly() this changes nothing about editability: an input mode
  // is orthogonal to the four ANDed inputs applyEditability() folds together.
  void setInputMode(InputMode p_mode);

  // The sheet's mode's status widget, or null. Published into the editor's
  // single status slot while this sheet holds the focus (decision D4).
  QSharedPointer<InputModeStatusWidget> inputModeStatusWidget() const;

  // Give the sheet the keyboard focus. Used to send it back after the Vi
  // command bar closes: the bar is not a descendant of this widget, so the
  // editor's own "the status widget lost the focus -> focus me" fallback would
  // drop the user out of the cell they were editing.
  void focusSheet();

  // Whether this sheet writes back a padded, column-aligned pipe table instead
  // of the compact one.
  //
  // Unlike setReadOnly() nothing about the sheet itself changes: no relayout
  // and no editability recompute, because only the shape of a FUTURE commit is
  // affected. Existing source is never reformatted on its own, and neither is
  // it by an edit which cancels out: the commit path asks whether anything
  // really changed in the shape its baseline was recorded in.
  void setSourceAlignEnabled(bool p_enabled);

  // Write back anything the debounce still owes, while this sheet's context
  // and anchor are still authoritative. The host calls it immediately before
  // it drops the identity, and again before it snapshots the live anchors for
  // a rebuild - an accepted flush retargets the anchor, and a copy taken
  // beforehand would have been collapsed by the very edit it applies.
  FlushOutcome flushNow();

  // The host has revoked this sheet's authority: the identity is about to be
  // erased, so every remaining commit path has to become a no-op. A late
  // timeout, a hide, a focus-out and the deferred deletion all pass through
  // here.
  void revokeAuthority();

  QSize sizeHint() const Q_DECL_OVERRIDE;

  bool hasHeightForWidth() const Q_DECL_OVERRIDE;

  int heightForWidth(int p_width) const Q_DECL_OVERRIDE;

  // The host's event filter watches this widget rather than the sheet inside
  // it, so the sheet's own layout request has to be posted here.
  bool event(QEvent *p_event) Q_DECL_OVERRIDE;

signals:
  void focusEscapeRequested(vte::FocusEscapeDirection p_direction);

  void undoRequested();

  void redoRequested();

  // Relayed from the sheet. See TablePreviewSheet::inputModeStatusWidgetChanged.
  void inputModeStatusWidgetChanged();

protected:
  void changeEvent(QEvent *p_event) Q_DECL_OVERRIDE;

private slots:
  void handleContentsChanged();

  void handleCellLeft();

  void handleFocusLost();

  void handleEscapeRequested(vte::FocusEscapeDirection p_direction);

  void handleUndoRequested();

  void handleRedoRequested();

  void handleReplacementFinished(const vte::PreviewReplacementResult &p_result);

  // The sheet settled on a geometry the host has not measured yet.
  void handlePreferredGeometryChanged();

  void handleCommitTimeout();

private:
  void resetFromSource();

  // Repaint the cells with the syntax runs of @p_table without rebuilding the
  // document, so the caret and the selection survive.
  //
  // Only applied when the snapshot's normalized cell matrix is equal, cell by
  // cell, to what the document currently holds: the unchanged-source path of
  // setPreview() also accepts the echo of a commit which the user has already
  // typed past, and painting that snapshot's offsets over newer text would
  // highlight the wrong characters. A theme change landing during an
  // uncommitted cell edit therefore stays stale until that edit is committed
  // and re-parsed.
  void refreshCellSyntaxFormats(const vte::TablePreview &p_table);

  // Take the bound snapshot from the context, which is the authoritative
  // binding: an accepted replacement rebases it onto the text now in the
  // document, and the cached one still describes the pre-commit source.
  void rebindFromContext();

  // Restart the idle debounce.
  void armCommit();

  // Write the sheet back now. "Nothing to commit" is Settled; a request the
  // host postponed is Deferred, which the caller must not treat as a loss.
  FlushOutcome flushPendingCommit();

  // A sheet only offers editing when the host would accept the commit.
  void applyEditability();

  // Managed by QObject.
  TablePreviewSheet *m_sheet = nullptr;

  // Destroyed after m_sheet, which renders it. See the destructor.
  QScopedPointer<TablePreviewDocument> m_document;

  // Managed by QObject.
  QTimer *m_commitTimer = nullptr;

  QSharedPointer<const TablePreview> m_table;

  // Set while the sheet is being rewritten from the bound snapshot, so the
  // resulting document changes are not mistaken for user edits.
  bool m_applyingSource = false;

  // The document revision that last had a real character change, so a
  // contentsChanged raised by an empty edit block or by a rehighlight is not
  // counted as an edit. See handleContentsChanged().
  int m_lastDocumentRevisionWithChanges = 0;

  bool m_readOnly = false;

  // The input mode currently mirrored onto the sheet (decision D6).
  InputMode m_inputMode = InputMode::NormalMode;

  // Whether an input mode has ever been mirrored down. Without it the very
  // first setInputMode(NormalMode) would be swallowed as "unchanged" and the
  // sheet would be left with no mode at all - which is a different thing from
  // the Normal mode.
  bool m_inputModeApplied = false;

  // Whether a commit writes the padded, column-aligned form. Read at
  // serialization time only; flipping it never rewrites existing source.
  bool m_alignSource = false;

  // Whether the bound snapshot still describes the document. Cleared by a
  // rejection the sheet cannot recover from, restored by the next snapshot.
  bool m_authoritative = true;

  // Set by revokeAuthority(). Terminal: nothing clears it.
  bool m_suppressed = false;

  // One posted layout request at a time, so a burst of settlements cannot
  // queue a burst of host measurements.
  bool m_layoutRequestPending = false;

  // Monotonic counter of accepted document changes. The commit machinery
  // compares generations rather than text, because the document can move on
  // while a replacement is in flight.
  quint64 m_editGeneration = 0;

  // The generation the last accepted commit carried.
  quint64 m_committedGeneration = 0;

  quint64 m_inFlightGeneration = 0;

  bool m_commitInFlight = false;

  // The outcome the last completion recorded, written synchronously by
  // handleReplacementFinished() so flushPendingCommit() can tell a postponed
  // request from a rejected one - the committed generation alone collapses
  // both into "not settled".
  FlushOutcome m_lastFlushOutcome = FlushOutcome::Settled;

  QString m_inFlightMarkdown;

  // The shape m_inFlightMarkdown was serialized in, captured when the request
  // was issued: a callback the replacement runs can flip the option before the
  // completion arrives.
  bool m_inFlightAligned = false;

  // The Markdown this sheet last knew the document to hold: the canonical form
  // of the bound source at bind time, and the accepted replacement after every
  // successful commit. It is both the "nothing changed" baseline a flush
  // compares against and the echo baseline setPreview() needs, so a parse
  // generation which merely replays this sheet's own commit is not mistaken
  // for an authoritative external change.
  //
  // It always describes text the document really holds, never a synthetic
  // other-shape rendering of it - which is what m_committedMarkdownAligned
  // records, so a flushed sheet can still ask "did anything really change"
  // after the aligned option moved underneath it.
  QString m_committedMarkdown;

  // Whether m_committedMarkdown was serialized in the aligned shape. Compared
  // against m_alignSource at commit time: when the two disagree the option was
  // flipped since, and the divergence test has to be run in the shape recorded
  // here before it can mean anything.
  bool m_committedMarkdownAligned = false;
};

class TablePreviewWidgetFactory : public PreviewWidgetFactory, public PreviewSizeEstimator {
  Q_OBJECT
  Q_INTERFACES(vte::PreviewSizeEstimator)
public:
  explicit TablePreviewWidgetFactory(QObject *p_parent = nullptr);

  QVector<PreviewElementType> supportedTypes() const Q_DECL_OVERRIDE;

  PreviewWidget *createWidget(PreviewWidgetContext *p_context,
                              const QSharedPointer<const Preview> &p_preview,
                              QWidget *p_parent) Q_DECL_OVERRIDE;

  // How tall the sheet for @p_preview would be, computed from the grid shape
  // and the font alone - no QTextDocument, no QTextTable, no cell matrix.
  //
  // This is what lets the host reserve a band for a table it has not built
  // yet. It is intentionally an over- rather than an under-estimate where it
  // has to guess, because a band that is slightly too tall shrinks when the
  // real widget arrives, whereas one that is too short makes the following
  // text jump DOWN, which is the more disorienting of the two.
  //
  // Returns an invalid QSizeF - i.e. "build me and measure properly" - for a
  // preview whose shape it cannot read: a non-table snapshot, an empty grid,
  // or a grid whose cell text is long enough that the wrap estimate below
  // would be guesswork rather than an approximation.
  QSizeF estimatedSize(const QSharedPointer<const Preview> &p_preview, qreal p_widthBasis,
                       const QFont &p_font) const Q_DECL_OVERRIDE;

  // Mirror the editor's read-only state onto every live sheet.
  void setReadOnly(bool p_readOnly);

  // Mirror the editor's configured input mode onto every live sheet, and onto
  // every sheet created afterwards (decision D6).
  void setInputMode(InputMode p_mode);

  // Mirror the aligned-source option onto every live sheet, and onto every
  // sheet created afterwards.
  void setSourceAlignEnabled(bool p_enabled);

signals:
  // Relayed from a live sheet, carrying the widget so the host can resolve the
  // identity - and therefore the live anchor - it belongs to.
  void focusEscapeRequested(vte::TablePreviewWidget *p_widget,
                            vte::FocusEscapeDirection p_direction);

  void undoRequested(vte::TablePreviewWidget *p_widget);

  void redoRequested(vte::TablePreviewWidget *p_widget);

  // A live sheet's input mode status widget was replaced or dropped. Emitted
  // while the outgoing mode is still alive, so the host can unmount the widget
  // before it dies.
  void inputModeStatusWidgetChanged(vte::TablePreviewWidget *p_widget);

private:
  // Drop the entries whose sheet has been destroyed.
  void pruneWidgets();

  bool m_readOnly = false;

  InputMode m_inputMode = InputMode::NormalMode;

  bool m_inputModeApplied = false;

  bool m_alignSource = false;

  QVector<QPointer<TablePreviewWidget>> m_widgets;
};
} // namespace vte

// The enum travels through signals, so Qt 5 needs it registered explicitly to
// store it in a QVariant (queued connections, QSignalSpy). Qt 6 registers
// enumerations automatically, which is why the omission only broke the Qt 5
// build.
Q_DECLARE_METATYPE(vte::FocusEscapeDirection)

#endif // TABLEPREVIEWWIDGET_H
